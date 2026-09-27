/**
 * shtc3 — 板载温湿度传感器驱动 (最小实现)
 *
 * 时序 (Sensirion SHTC3 datasheet):
 *   唤醒   0x3517
 *   读 ID  0xEFC8 → 3 字节 (ID_MSB, ID_LSB, CRC), SHTC3 = 0x0807
 *   测量   0x7866 (温度在前, 不启用时钟拉伸) → 等 ~12ms → 读 6 字节
 *   休眠   0xB098
 *
 * 数据按 MSB-first 两字节 + CRC8 (poly 0x31, init 0xFF) 校验。
 * 换算: T = -45 + 175 × raw/65536,  RH = 100 × raw/65536
 */

#include "shtc3.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "shtc3";

#define SHTC3_ADDR       0x70
#define SHTC3_SDA_GPIO   13
#define SHTC3_SCL_GPIO   14
#define SHTC3_FREQ_HZ    100000
#define SHTC3_TIMEOUT_MS 100

#define SHTC3_ID_EXPECT  0x0807

static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_dev = NULL;

/* CRC-8: poly 0x31, init 0xFF, 不反转, 不异或输出 */
static uint8_t crc8(const uint8_t *d, int n) {
    uint8_t crc = 0xFF;
    for (int i = 0; i < n; i++) {
        crc ^= d[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
    }
    return crc;
}

static esp_err_t shtc3_cmd(uint16_t c) {
    uint8_t b[2] = { (uint8_t)(c >> 8), (uint8_t)(c & 0xFF) };
    return i2c_master_transmit(s_dev, b, sizeof(b), SHTC3_TIMEOUT_MS);
}

bool shtc3_init(void) {
    if (s_dev) return true;

    i2c_master_bus_config_t bus_cfg = {
        .clk_source                = I2C_CLK_SRC_DEFAULT,
        .i2c_port                  = I2C_NUM_0,
        .scl_io_num                = SHTC3_SCL_GPIO,
        .sda_io_num                = SHTC3_SDA_GPIO,
        .glitch_ignore_cnt         = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bus_cfg, &s_bus) != ESP_OK) {
        ESP_LOGW(TAG, "I2C 总线初始化失败 (SDA=GPIO%d SCL=GPIO%d)",
                 SHTC3_SDA_GPIO, SHTC3_SCL_GPIO);
        s_bus = NULL;
        return false;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = SHTC3_ADDR,
        .scl_speed_hz    = SHTC3_FREQ_HZ,
    };
    if (i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev) != ESP_OK) {
        ESP_LOGW(TAG, "挂载设备 0x%02X 失败", SHTC3_ADDR);
        s_dev = NULL;
        return false;
    }

    /* 读 ID 确认芯片在位 (顺便验证 CRC 通路) */
    uint8_t id[3] = { 0 };
    bool ok = false;
    if (shtc3_cmd(0x3517) == ESP_OK) {              /* wake */
        esp_rom_delay_us(300);                      /* t_wu ≥ 100us */
        if (shtc3_cmd(0xEFC8) == ESP_OK &&          /* read ID */
            i2c_master_receive(s_dev, id, sizeof(id), SHTC3_TIMEOUT_MS) == ESP_OK) {
            uint16_t v = (uint16_t)((id[0] << 8) | id[1]);
            bool crc_ok = (crc8(id, 2) == id[2]);
            ok = crc_ok && (v == SHTC3_ID_EXPECT);
            ESP_LOGI(TAG, "SHTC3 id=0x%04X crc=%s → %s", v, crc_ok ? "ok" : "BAD",
                     ok ? "已就绪" : "型号不符");
        }
    }
    shtc3_cmd(0xB098);                              /* sleep */

    if (!ok) ESP_LOGW(TAG, "未检测到 SHTC3 (I2C 0x%02X), 室内温湿度功能关闭", SHTC3_ADDR);
    return ok;
}

bool shtc3_read(float *temp_c, float *rh) {
    if (!s_dev) return false;

    if (shtc3_cmd(0x3517) != ESP_OK) return false;  /* wake */
    esp_rom_delay_us(300);

    if (shtc3_cmd(0x7866) != ESP_OK) {              /* measure T first */
        shtc3_cmd(0xB098);
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(20));                  /* 测量 ~12.1ms, 留余量 */

    uint8_t d[6] = { 0 };
    esp_err_t err = i2c_master_receive(s_dev, d, sizeof(d), SHTC3_TIMEOUT_MS);
    shtc3_cmd(0xB098);                              /* sleep */
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "读测量值失败: %s", esp_err_to_name(err));
        return false;
    }
    if (crc8(d, 2) != d[2] || crc8(d + 3, 2) != d[5]) {
        ESP_LOGW(TAG, "CRC 校验失败");
        return false;
    }

    uint16_t raw_t  = (uint16_t)((d[0] << 8) | d[1]);
    uint16_t raw_rh = (uint16_t)((d[3] << 8) | d[4]);
    if (temp_c) *temp_c = -45.0f + 175.0f * (float)raw_t / 65536.0f;
    if (rh)     *rh     = 100.0f * (float)raw_rh / 65536.0f;
    return true;
}
