# AI_Mate — AI 额度 / 消费 / 天气 桌面摆件

ESP32-S3 + RLCD 4.2" 反射式墨水屏（400×300，1-bit 黑白），**WiFi 直连**查询：
智谱 GLM Coding Plan 额度、DeepSeek 余额、余额快照推算的每日消费、本地天气（含预警与分钟级降水）。
完全独立运行，无需 PC / 蓝牙 / 账号登录（全部走 API Key）。

## 硬件

| 项目 | 规格 |
|------|------|
| 主控 | ESP32-S3 (Waveshare ESP32-S3-RLCD-4.2) |
| 屏幕 | 400×300 横屏, 1-bit B/W, ST7305, 驱动把 RGB565 按 `lum>=80 → 白` 二值化 |
| 通信 | WiFi STA (查询) + SoftAP (配网门户) |
| 按键 | BOOT (GPIO0) 长按 3s 重新配网 |
| 电池 | ADC GPIO2 (ADC1_CH3, 板载 100k/300k 分压, ×3) |

屏幕引脚: MOSI=12 SCLK=11 CS=40 DC=5 RST=41 TE=6

## 数据源

| 数据 | 接口 | 认证 |
|------|------|------|
| 智谱额度 | `GET https://bigmodel.cn/api/monitor/usage/quota/limit?type=N` | Header: `Authorization`(raw key) + `bigmodel-organization` + `bigmodel-project` |
| DeepSeek 余额 | `GET https://api.deepseek.com/user/balance` | `Authorization: Bearer <key>` |
| 天气/预警/分钟级 | 和风 `https://<API Host>/weather/v1/{current,daily,hourly}/..`、`/weatheralert/v1/current/..`、`/v7/minutely/5m` | `X-QW-Api-Key: <key>` |
| 天气(备选,免Key) | `https://api.open-meteo.com/v1/forecast`（含 current/daily/hourly） | — |
| 城市定位 | `https://geocoding-api.open-meteo.com/v1/search` | — |
| IP 定位 | `http://ip-api.com/json/` (主) / `https://api.ip.sb/geoip` (备) | — |
| 时间 | SNTP ntp.aliyun.com / pool.ntp.org | — |

- 智谱 `limits[]`: `unit=3`→5小时窗, `unit=6`→周窗; 字段 `percentage/usage/currentValue/remaining/nextResetTime`; type=1 个人, 2 团队
- org/project: 浏览器登录 bigmodel.cn/coding-plan → F12 → Network → `quota/limit` 请求头
- **和风返回 gzip（无视 `Accept-Encoding: identity`）**，ESP-IDF 无 zlib → 内置 `components/puff` 解压
- 和风 `type=1`…见上；预警 `alerts[].eventType.name` + `color.code`；分钟级 `summary` + `minutely[24].precip`(mm/5min)

## 每日消费推算 (无 userToken)

```
每日消费 = 当日开盘余额 − 当日收盘余额 + 当日充值
充值检测: 两次快照间隔 <35min 且余额跳增 ≥8元 → 记为充值
设备离线跨多天 → 中间天记 -1 (无数据)
```
NVS `ai_hist`: `cents[63]`(分) `base` `dbase` `rechg` `lbal` `lepo` `wxlat` `wxlon` `wxcity2` `wxepo`
> 是设备观测值；当天首次轮询前的消耗无法计入。日期换算用 Howard Hinnant 算法（正反必须成对）。

## 电池电量

- 采样：16 次、丢弃前 4 次、**只在射频关闭时采**（WiFi 发射瞬时压降 0.1~0.2V 会污染 OCV）
- 换算：22 点开路电压(OCV)曲线 + 线性插值；**不要用线性映射**（锂电平台区误差可达 30~50%）
- 充电识别：≥4.15V 或 10 分钟内电压涨 ≥15mV；≤10% 且未充电时状态栏提示"电量低"
- 分压比可在配网页校准（`bdiv`，默认 300 = 3.00）

## 省电

CPU 80MHz + `CONFIG_PM_ENABLE` + tickless idle + `WIFI_PS_MAX_MODEM`，
且每轮查询后 **`esp_wifi_stop()` 断射频**，睡 `poll_min` 分钟再唤醒（射频在线约 1.3%）。
断射频触发的 DISCONNECT 事件被 `s_radio_off` 屏蔽，不会触发重试/配网。

## 配网与多网络

- 触发：首次开机 / 所有已知网络失败 / 长按 BOOT 3s → AP `AI-Mate-Setup` (密码 `aimate123`)，`http://192.168.4.1`
- 最多记 4 组 WiFi（NVS `nnet` `s0..s3` `p0..p3` `last`）；开机扫描择优；连不上先重试 3 次再换网，全失败才进配网
- 唤醒重连走快速路径（不扫描）
- 配网页动态生成（含附近热点 `datalist`、已保存网络列表、字段回填）

## 项目结构

```
main/
├── cc_mate.c/h        # 入口、主循环、页面轮播、电池 ADC、BOOT 键
├── config_store.c/h   # NVS 配置(多网络/密钥/天气/电池)
├── wifi_mgr.c/h       # STA 多网络择优 + AP 配网门户 + 射频开关
├── net_query.c/h      # 智谱/DeepSeek、SNTP、消费历史、通用 HTTPS GET、gzip(puff) 解码
├── weather.c/h        # 双数据源天气 + 预警 + 分钟级降水 + IP/城市定位 + 图标映射
└── ui/                # ui.c/h + font_cjk_16 + font_wx_icon_36/24 + font_wx_num_36
components/rlcd_display/  # ST7305 驱动
components/puff/          # DEFLATE 解压 (Mark Adler, 公共领域)
```

## UI (4 页：额度 / 消费柱状图 / 配网 / 天气)

天气页布局要点（**按导出字体真实度量排布**）：
- 字体行高：`font_cjk_16`=18（≠16!）, `font_wx_icon_36`=36, `font_wx_num_36`=28（≠36!）, Montserrat20=22
- 图标与温度**视觉中心对齐**（同为 44）：icon y=26、temp y=30
- 预警横幅**动态占位**：有预警时 100..120（黑底白字反白），无预警时下方内容上移 26px、柱高上限 20→46px
- **所有动态文本标签必须设 `LV_LABEL_LONG_DOT`**，否则超长会换行压住下一行（已踩坑：峰值标签）

## 构建 / 烧录

```bat
subst X: D:\ESP\监控            :: 路径含中文, IDF 部分工具会乱码
X:\cc_mate\build.bat clean
X:\cc_mate\build.bat
X:\cc_mate\build.bat flash COM3
```
或 esptool 直烧（**工作目录必须是 build/**，否则 `@flash_args` 相对路径找不到）：
```
python <IDF>/components/esptool_py/esptool/esptool.py --chip esp32s3 -p COM3 write_flash @X:/cc_mate/build/flash_args
```
ESP-IDF v5.5.4 @ `C:\esp\v5.5.4\esp-idf`，工具链 `C:\Espressif`，Python 环境 `~/.espressif/python_env/idf5.5_py3.14_env`。

## 踩坑记录（重要）

1. **非 ASCII 路径**：`监控` 会让 objdump/ldgen 乱码 → 必须 `subst` 成盘符构建
2. **PowerShell 写源码**：本机 pwsh 默认 ANSI/GBK，用 `Get-Content|Set-Content` 改源码会把中文注释双重编码搞坏
   （曾毁掉 net_query.c 与 sdkconfig.defaults）→ **只用 edit/write 工具改源码**；必须用 pwsh 时走
   `[System.IO.File]::WriteAllText($p,$t,(New-Object System.Text.UTF8Encoding($false)))`
3. **字体声明区间 ≠ 有字形**：simhei 缺 ☀☁☂❄⚡，`lv_font_conv` 会静默跳过 → 用 `--symbols` 从 `seguisym.ttf` 补齐
4. **`esp_wifi_connect()` 必须在 `STA_START` 事件里调用**，在 `esp_wifi_start()` 后立即调用会静默失败
5. **不要在 esp_event 回调里做阻塞操作**（配网切换/扫描）→ 只置标志，主任务轮询执行
6. **main 任务栈默认 3584 太小**：24 个 `wifi_ap_record_t`(≈110B) 放栈上会爆 → 已提到 6144 并改用静态缓冲
7. **和风回 gzip**：需自行解压（puff），且 Key 走请求头而非 URL（避免错误日志泄漏）
