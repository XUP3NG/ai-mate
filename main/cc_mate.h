/**
 * AI_Mate — AI 额度监控桌面摆件
 * ESP32-S3 + RLCD 4.2" 副屏显示智谱 GLM Coding Plan 额度与 DeepSeek 余额
 *
 * 架构：
 *   ESP32 WiFi 直连 → bigmodel.cn / api.deepseek.com (HTTPS, API Key 认证)
 *   每日消费 = 余额快照推算 (昨日余额 - 今日余额 + 充值)
 *   AP 配网门户 + NVS 存储，BOOT 键长按 3s 重新配网
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 屏幕尺寸 (ST7305, 400×300 横屏) */
#define DISPLAY_WIDTH   400
#define DISPLAY_HEIGHT  300

/* ── 网络状态 ── */
typedef enum {
    NET_UNCONFIGURED = 0,   /* NVS 无配置 → AP 配网 */
    NET_CONNECTING,         /* 正在连接 WiFi */
    NET_CONNECTED,          /* WiFi 已连接 */
    NET_FAILED,             /* 连接失败 (重试后进入配网) */
    NET_PORTAL,             /* AP 配网模式 */
    NET_RADIO_SLEEP,        /* 查询间隔内射频关闭 (省电, 定时唤醒) */
} net_state_t;

/* ── 智谱 GLM Coding Plan 窗口 ── */
typedef struct {
    bool     valid;
    uint8_t  pct;           /* 已用百分比 0–100 */
    uint32_t usage;         /* 总额度 (积分) */
    uint32_t current;       /* 已用 (积分) */
    uint32_t remaining;     /* 剩余 (积分) */
    int64_t  reset_ms;      /* 重置时间 epoch 毫秒, 0=未知 */
} glm_window_t;

typedef struct {
    bool          valid;
    glm_window_t  win_5h;   /* 5 小时滚动窗口 */
    glm_window_t  win_w;    /* 周窗口 */
    uint32_t      last_ok_ms;   /* 上次成功查询 (开机毫秒) */
    char          err[48];      /* 最近错误 */
} glm_info_t;

/* ── DeepSeek 余额 ── */
typedef struct {
    bool     valid;
    bool     is_available;
    double   total;         /* 总余额 */
    double   granted;       /* 赠送余额 */
    double   topped;        /* 充值余额 */
    char     currency[8];
    uint32_t last_ok_ms;
    char     err[48];
} dsk_info_t;

/* ── 消费历史 (余额快照推算) ── */
#define HIST_DAYS   63          /* 保留 9 周 */

/* 注意: cents 由 net_task 写、主任务(UI)读, 无锁。
 * 对齐的 int32 单读写为原子操作, 最坏情况是柱状图短暂显示
 * 新旧混合的一帧, 下一秒自愈。e-ink 低刷新率场景可接受。 */
typedef struct {
    int32_t  cents[HIST_DAYS];  /* 每日消费 (分), -1=无数据 */
    uint16_t year;
    uint8_t  month;
    uint8_t  day;               /* cents[0] 对应的日期 */
    uint8_t  count;             /* 有效天数 */
    /* 当日累计 */
    int32_t  today_cents;
    int32_t  month_cents;
} hist_info_t;

/* ── 天气预警 (和风 weatheralert) ── */
typedef struct {
    bool     valid;                 /* 已成功获取 (含"无预警"的情况) */
    uint8_t  count;                 /* 生效预警条数 */
    bool     severe;                /* 橙/红/黑色 → 反白强调 */
    char     title[40];             /* 如 "大风蓝色预警" */
} wx_alert_t;

/* ── 分钟级降水 (和风 minutely, 未来 2 小时 × 5 分钟 = 24 格) ── */
#define WX_MIN_N 24

typedef struct {
    bool     valid;
    uint8_t  bar[WX_MIN_N];         /* 归一化柱高 0..100 */
    uint16_t peak_x100;             /* 峰值 (mm/5min × 100) */
    char     summary[40];           /* 和风自然语言, 如 "95分钟后雨就停了" */
} wx_minutely_t;

/* ── 天气 (Open-Meteo 免 Key / 和风天气 需 Key, 双源) ── */
#define WX_DAYS 4

typedef struct {
    bool     valid;
    char     city[24];               /* 城市名 (显示用) */
    char     src[16];               /* 数据来源: Open-Meteo / 和风天气 (4 汉字=12B+NUL) */
    int16_t  temp_x10;              /* 当前温度 (0.1°C) */
    uint8_t  humidity;              /* 当前湿度 % */
    int16_t  feels_x10;             /* 体感温度 (0.1°C) */
    bool     feels_valid;           /* 体感温度是否有效 */
    uint8_t  wind_scale;            /* 蒲福风级 (0-12) */
    bool     wind_valid;
    /* 空气质量 (和风独有; Open-Meteo 无此数据) */
    bool     aqi_valid;
    uint16_t aqi;                   /* 中国标准 AQI */
    char     aqi_cat[16];           /* 优 / 良 / 轻度污染 … (跟随 lang=zh) */
    char     text[20];              /* 当前天气文字 (中文) */
    char     icon[8];               /* 当前图标 (UTF-8 符号) */
    char     dtext[WX_DAYS][20];    /* 每日天气文字 */
    char     dicon[WX_DAYS][8];     /* 每日图标 */
    int16_t  tmax_x10[WX_DAYS];
    int16_t  tmin_x10[WX_DAYS];
    /* 未来几小时降水提醒 (逐小时预报推算) */
    bool     rain_valid;            /* 逐小时数据是否拿到 */
    uint8_t  rain_in_hours;         /* 几小时后开始降水 (0=窗口内无) */
    uint8_t  rain_prob;             /* 窗口内最大降水概率 % */
    char     rain_icon[8];
    char     rain_text[48];         /* 如 "2小时后有雨 (68%)" / "未来6小时无降水" */
    wx_alert_t    alert;            /* 天气预警 (和风) */
    wx_minutely_t minutely;         /* 分钟级降水 (和风) */
    uint32_t last_ok_ms;
    char     err[48];
} weather_info_t;

/* ── 每日一图 (Bing 壁纸) ── */
typedef struct {
    bool     valid;             /* 有可显示的图 (NVS 里的也算) */
    bool     generating;        /* 正在生成 (UI 显示提示) */
    char     title[64];         /* 图片说明 (Bing copyright, 去掉版权括号) */
} art_info_t;

/* ── 全局应用状态 ── */
typedef struct {
    net_state_t net;
    glm_info_t  glm;
    dsk_info_t  dsk;
    hist_info_t hist;
    weather_info_t wx;
    art_info_t  art;

    bool     time_valid;        /* SNTP 已同步 */
    uint32_t boot_ms;           /* 开机毫秒 (用于显示运行时长) */
    char     ssid[33];          /* 当前 WiFi SSID (显示用) */

    /* 配网门户信息 */
    char     ap_ssid[24];
    char     portal_ip[16];

    bool     battery_configured;
    uint8_t  battery_pct;
    uint16_t battery_mv;         /* 电池电压 mV (开路电压, 射频关闭时采样) */
    bool     battery_charging;

    /* 板载 SHTC3 室内温湿度 (传感器不在位时 indoor_valid 恒为 false) */
    bool     indoor_valid;
    int16_t  indoor_temp_x10;    /* 室内温度 0.1°C */
    uint8_t  indoor_rh;          /* 室内湿度 % */
} app_state_t;

#ifdef __cplusplus
}
#endif
