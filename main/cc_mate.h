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

/* ── 全局应用状态 ── */
typedef struct {
    net_state_t net;
    glm_info_t  glm;
    dsk_info_t  dsk;
    hist_info_t hist;

    bool     time_valid;        /* SNTP 已同步 */
    uint32_t boot_ms;           /* 开机毫秒 (用于显示运行时长) */

    /* 配网门户信息 */
    char     ap_ssid[24];
    char     portal_ip[16];

    bool     battery_configured;
    uint8_t  battery_pct;
} app_state_t;

#ifdef __cplusplus
}
#endif
