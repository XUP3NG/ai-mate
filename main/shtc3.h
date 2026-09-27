/**
 * shtc3 — 板载 SHTC3 温湿度传感器 (I2C 0x70, SDA=GPIO13, SCL=GPIO14)
 *
 * 与 ES8311(codec) / ES7210(ADC) / PCF85063(RTC) 共用同一条 I2C 总线,
 * 本项目只用 SHTC3。芯片不在位时全部接口安全降级 (init 返回 false,
 * read 恒返回 false), UI 上不显示任何内容。
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 建 I2C 总线并探测芯片; 返回 false 表示没有 SHTC3 (功能静默关闭) */
bool shtc3_init(void);

/* 读一次: 温度 (°C) 与相对湿度 (%RH)。任一指针可为 NULL。成功返回 true */
bool shtc3_read(float *temp_c, float *rh);

#ifdef __cplusplus
}
#endif
