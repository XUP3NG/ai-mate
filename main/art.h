/**
 * art — 每日一图 (Bing 每日壁纸 → 1-bit 全屏抖动)
 *
 * 每天 (UTC 日) 从 Bing 拉当天壁纸, 在设备端解码 + 二值化, 全屏显示在 Page 4。
 *
 *   1. GET https://www.bing.com/HPImageArchive.aspx?format=js&idx=0&n=1&mkt=zh-CN
 *      → images[0].urlbase / title
 *   2. GET https://www.bing.com{urlbase}_1920x1080.jpg&w=400&h=248&rs=1&c=4&pid=hp
 *      Bing 服务端直接裁成 400×248, 只有 **20KB** (原图 340KB) —— 省流量也省解码
 *   3. esp_new_jpeg 解码 → RGB565 → 亮度
 *   4. 自动色阶(10%/90% 分位) + 平坦区保护抖动 → 1-bit
 *   5. 存到 storage 分区 (裸分区, 非文件系统), 开机直接读回
 *
 * 为什么不用 LLM 画: 文本模型是"盲画"的(逐字符输出无法回看), 40×30 已是极限,
 * 而这块屏有 400×248; 用真照片 + 抖动反而信息量最大。
 *
 * 成本: 0 元 (无需 API Key), 每天 20KB 流量。
 */

#pragma once

#include "cc_mate.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 画布: 页面可用区 400×276, 图占上部 248, 底下留给标题行 */
#define ART_W 400
#define ART_H 248

/* 开机加载: 从 storage 分区读回上次的图 (有则立即可显示) */
void art_init(app_state_t *st);

/* net_task 每轮调用: 时间已同步且今天还没取图 → 拉取 (阻塞数秒) */
void art_poll(app_state_t *st);

/* 1-bit 位图, 行主序 MSB-first, 每行 ART_W/8 = 50 字节, 共 12400 字节 */
const uint8_t *art_bitmap(void);

/* 画布版本号: 每次有新图 +1, UI 据此判断是否需要重绘 */
int art_rev(void);

#ifdef __cplusplus
}
#endif
