# AI-Mate — AI 额度监控桌面摆件

ESP32-S3 + 4.2" RLCD 墨水屏（400×300 纯黑白），**WiFi 直连**查询 **智谱 GLM Coding Plan 额度** 与 **DeepSeek 余额**，并用余额快照推算每日消费、显示 30 天柱状图。完全独立运行，插个 USB 电源即可，无需电脑参与。

> 由 Claude Code 监控伴侣 [cc_mate](https://github.com/vincezhaojie-lang/cc_mate) 改造而来（BLE/PC 桥接 → WiFi 独立运行）。

## 硬件

| 项目 | 规格 |
|------|------|
| 主控 | ESP32-S3 (Waveshare ESP32-S3-RLCD-4.2) |
| 屏幕 | 400×300, 1-bit B/W, ST7305 反射式 LCD |
| 通信 | WiFi STA（查询）+ SoftAP（配网门户） |
| 按键 | BOOT (GPIO0) 长按 3s 重新配网 |
| 电池 | ADC GPIO2（可选） |

## 显示

**第一页 · 额度总览**（15s 轮播）
```
┌ 智谱 GLM Coding Plan ──────────────┐
│ 5h 窗 ████░░░░░░░░░░░░░░  15%      │
│ 周 窗 █████░░░░░░░░░░░░  17%      │
│ 剩余积分  5h 10181/12000 周 49373/60000 │
│ 重置倒计时 5h 2h41m  周 4d3h       │
├ DeepSeek 余额 ─────────────────────┤
│ 115.88 元                          │
│ 赠金余额 0.00 元   充值余额 115.88 元 │
│ 今日消费 1.23 元   本月消费 1.23 元  │
└────────────────────────────────────┘
```

**第二页 · 30 天消费柱状图**：黑色柱体 + 虚线参考线 + 横轴日期，统计今日/本月/30天/日均/最高。

**配网页**：AP 名 + 配置地址提示。

## 数据源（均为 API Key 认证，无需账号登录）

| 数据 | 接口 |
|------|------|
| 智谱额度 | `GET https://bigmodel.cn/api/monitor/usage/quota/limit?type=1`，Header: `Authorization`(raw key) + `bigmodel-organization` + `bigmodel-project` |
| DeepSeek 余额 | `GET https://api.deepseek.com/user/balance`，Header: `Authorization: Bearer <key>` |

org/project 获取：浏览器登录 bigmodel.cn/coding-plan → F12 → Network → `quota/limit` 请求头。

## 每日消费推算（无需 userToken）

```
每日消费 = 当日开盘余额 − 当日收盘余额 + 当日充值
充值检测: 两次快照间隔 <35min 且余额跳增 ≥8元 → 记为充值
离线跨多天 → 中间天记为无数据
```

> 注意：这是设备观测值。当天设备首次运行之前的消耗无法计入；精确的平台官方明细需 userToken（会过期），故未采用。

## 配网

- 首次开机 / WiFi 失败(20 次重试) / **长按 BOOT 3 秒** → 热点 `AI-Mate-Setup`（密码 `aimate123`）
- 浏览器打开 `http://192.168.4.1` 填写：WiFi、智谱 Key/org/project/套餐类型(1 个人 2 团队)、DeepSeek Key、轮询间隔(分钟)
- 保存 → NVS 持久化 → 自动重启连接。换 WiFi/换 Key 同一入口。

## 构建

ESP-IDF v5.5+，ESP32-S3。

```bat
:: Windows (注意: 路径含中文时用 subst 映射成 ASCII 盘符, 部分 IDF 工具链对非 ASCII 路径敏感)
subst X: <项目路径>
X:\build.bat clean
X:\build.bat
build.bat flash COM3
build.bat monitor COM3
```

或在 ESP-IDF 环境中标准流程：`idf.py set-target esp32s3 && idf.py build flash monitor`。

## 项目结构

```
main/
├── cc_mate.c/h       # 入口, 主循环, 页面轮播, 电池 ADC, BOOT 键
├── config_store.c/h  # NVS 配置读写
├── wifi_mgr.c/h      # WiFi STA + AP 配网门户 (事件回调只置标志, 主任务切门户)
├── net_query.c/h     # HTTPS 查询 (esp_crt_bundle) + SNTP + 消费历史推算
└── ui/
    ├── ui.c/h        # 三页 UI (1-bit 适配: 描边轨道/虚线网格/四分填充)
    └── font_cjk_16.c # CJK 字体 (0x20-0x7F, 0x2000-0x206F, 0x2580-0x259F, 0x3000-0x303F, 0x4E00-0x9FFF, 0xFF00-0xFF5F)
components/rlcd_display/  # ST7305 驱动 (继承自 cc_mate)
```

## 1-bit 黑白屏适配要点

驱动把 RGB565 按亮度阈值二值化（`lum >= 80 → 白`），因此：
- 任何"浅灰"都会变成白色而不可见 → 轨道用**描边**、参考线用**虚线**、等级用**四分填充**
- Montserrat 字体没有 CJK 字形 → 数字与"元"分属两个 label 动态贴合

## License

MIT
