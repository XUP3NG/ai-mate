# AI_Mate — AI 额度监控桌面摆件

ESP32-S3 + RLCD 4.2" 副屏，WiFi 直连查询智谱 GLM Coding Plan 额度与 DeepSeek 余额，
余额快照推算每日消费并显示热力图。完全独立运行，无需 PC 参与。

## 硬件

| 项目 | 规格 |
|------|------|
| 主控 | ESP32-S3 (Waveshare ESP32-S3-RLCD-4.2) |
| 屏幕 | 400×300 横屏, 1-bit B/W, ST7305 驱动 |
| 通信 | WiFi STA (查询) + SoftAP (配网门户) |
| 按键 | BOOT (GPIO0) 长按 3s 重新配网 |
| 电池 | ADC GPIO2 (可选) |

屏幕引脚同旧版: MOSI=12 SCLK=11 CS=40 DC=5 RST=41 TE=6

## 数据源 (均为 API Key 认证, 无需登录)

| 数据 | 接口 | 认证 |
|------|------|------|
| 智谱额度 | `GET https://bigmodel.cn/api/monitor/usage/quota/limit?type=1` | Header: `Authorization`(raw key) + `bigmodel-organization` + `bigmodel-project` |
| DeepSeek 余额 | `GET https://api.deepseek.com/user/balance` | Header: `Authorization: Bearer <key>` |

- 智谱响应 `limits[]`: `unit=3`→5小时窗, `unit=6`→周窗; 字段 `percentage/usage/currentValue/remaining/nextResetTime`
- type=1 个人版, type=2 团队版
- org/project 获取: 浏览器登录 bigmodel.cn/coding-plan → F12 Network → quota/limit 请求头

## 每日消费推算 (方案1, 无需 userToken)

```
每日消费 = 当日开盘余额 − 当日收盘余额 + 当日充值
充值检测: 两次快照间隔 <35min 且余额跳增 ≥8元 → 记为充值
设备离线跨多天 → 中间天记 -1 (无数据)
```
NVS 命名空间 `ai_hist`: cents[63](分) / base(日期) / dbase / rechg / lbal / lepo

## 配网

- 首次开机 / WiFi 连接失败(20次重试) / 长按 BOOT 3s → AP `AI-Mate-Setup` (密码 `aimate123`)
- 浏览器打开 `http://192.168.4.1`, 填写 WiFi + 智谱 Key/org/project/type + DeepSeek Key + 轮询间隔
- 保存 → NVS (命名空间 `ai_mate`) → 自动重启连接
- 换 WiFi/换 Key 同一入口

## 项目结构

```
main/
├── cc_mate.c/h       # 入口, 主循环, 页面轮播, 电池 ADC, BOOT 键
├── config_store.c/h  # NVS 配置读写
├── wifi_mgr.c/h      # WiFi STA + AP 配网门户 (esp_http_server)
├── net_query.c/h     # HTTPS 查询 (esp_crt_bundle) + SNTP + 消费历史推算
└── ui/
    ├── ui.c/h        # 三页 UI: 额度总览 / 9周热力图 / 配网提示
    └── font_cjk_16.c # CJK 字体
components/rlcd_display/  # ST7305 驱动 (不变)
```

已移除: BLE NUS, bridge/, protocol.c, clawd.c, 蜂鸣器/ES8311 音频, PC 系统信息页

## UI

- 顶栏: 标题 | 日期时间 | WiFi 状态 | 电池
- 页0 (额度总览, 15s): 智谱 5h/周 双进度条+百分比+剩余积分+重置倒计时; DeepSeek 余额/赠金/充值/今日本月消费
- 页1 (热力图, 15s): 9列×7行=63天, 格内自底向上四分填充表示 5 档消费 (1-bit 无灰阶)
- 页2 (配网): AP 名 + 配置地址提示

## 构建 (注意: 工作区路径含中文, 需经 ASCII 盘符构建)

```bat
subst X: D:\ESP\监控
X:\cc_mate\build.bat clean
X:\cc_mate\build.bat
build.bat flash COM3
build.bat monitor COM3
```
ESP-IDF v5.5.4 @ C:\esp\v5.5.4\esp-idf, 工具链 C:\Espressif
