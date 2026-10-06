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
| 室内温湿度 | 板载 SHTC3, I2C 0x70, SDA=GPIO13 SCL=GPIO14 |

> 板上还有一堆没用到的资源: ES8311 codec + ES7210 双麦 + 喇叭座 (I2S: 8/9/10/16/45, 功放使能 46)、
> PCF85063 RTC (同一 I2C 总线)、TF 卡槽、KEY 键 (GPIO18)、8MB PSRAM、11MB 空闲 Flash。
> 官方引脚表: <https://devices.esphome.io/devices/waveshare-esp32-s3-rlcd-42/>

屏幕引脚: MOSI=12 SCLK=11 CS=40 DC=5 RST=41 TE=6

## 数据源

| 数据 | 接口 | 认证 |
|------|------|------|
| 智谱额度 | `GET https://bigmodel.cn/api/monitor/usage/quota/limit?type=N` | Header: `Authorization`(raw key) + `bigmodel-organization` + `bigmodel-project` |
| DeepSeek 余额 | `GET https://api.deepseek.com/user/balance` | `Authorization: Bearer <key>` |
| 天气/预警/分钟级 | 和风 `https://<API Host>/weather/v1/{current,daily,hourly}/..`、`/weatheralert/v1/current/..`、`/v7/minutely/5m`、`/airquality/v1/current/..` | `X-QW-Api-Key: <key>` |
| 天气(备选,免Key) | `https://api.open-meteo.com/v1/forecast`（含 current/daily/hourly） | — |
| 城市定位 | `https://geocoding-api.open-meteo.com/v1/search` | — |
| IP 定位 | `http://ip-api.com/json/` (主) / `https://api.ip.sb/geoip` (备) | — |
| 时间 | SNTP ntp.aliyun.com / pool.ntp.org | — |
| 每日一图 | `https://www.bing.com/HPImageArchive.aspx?format=js&idx=0&n=1&mkt=zh-CN` + `https://www.bing.com/th?id=...&w=400&h=248&rs=1&c=4` | — |

- 智谱 `limits[]`: `unit=3`→5小时窗, `unit=6`→周窗; 字段 `percentage/usage/currentValue/remaining/nextResetTime`; type=1 个人, 2 团队
- org/project: 浏览器登录 bigmodel.cn/coding-plan → F12 → Network → `quota/limit` 请求头
- **和风返回 gzip（无视 `Accept-Encoding: identity`）**，ESP-IDF 无 zlib → 内置 `components/puff` 解压
- 和风 `type=1`…见上；预警 `alerts[].eventType.name` + `color.code`；分钟级 `summary` + `minutely[24].precip`(mm/5min)
- **和风 v1 响应字段**（`/weather/v1/current` 一次拿全, 加显示项不用加请求）：
  `condition.{text,code}` / `temperature.value` / `feelsLike.value` / `humidity`(0~1 小数) /
  `wind.direction.compass` + `wind.scale` / `windGust` / `precipitation.{amount,intensity,type}` /
  `pressure` / `visibility`(m) / `dewPoint` / `cloudCover`(0~1) / `uvIndex`
- 空气质量 `/airquality/v1/current/{lat}/{lon}`：`indexes[]` 里 `code=chn` 是中国标准（另有 `qaqi`/`us-epa`），
  取 `aqi` + `category`（跟随 `lang=zh` 返回"优/良/轻度污染…"）。**每个查询周期多 1 次请求**
- Open-Meteo 无空气质量；体感用 `current=apparent_temperature`（同一请求里加参数，不额外请求）

## 每日消费推算 (无 userToken)

```
每日消费 = 当日开盘余额 − 当日收盘余额 + 当日充值
充值检测: 两次快照间隔 <35min 且余额跳增 ≥8元 → 记为充值
设备离线跨多天 → 中间天记 -1 (无数据)
```
NVS `ai_hist`: `cents[63]`(分) `base` `dbase` `rechg` `lbal` `lepo`（坐标键见"定位与坐标"）
> 是设备观测值；当天首次轮询前的消耗无法计入。日期换算用 Howard Hinnant 算法（正反必须成对）。

## 定位与坐标（位置跟着 WiFi 网络走）

**位置按 SSID 永久绑定**，不再每日重查 IP（重查只会让 IP 库偶尔给个"邻居市"，城市名和天气跟着跳）。

优先级：**本网络手填坐标 > 本网络城市名解析 > 本网络 IP 自动定位 > 全局手填坐标**

- NVS `ai_hist`：键 `a{l,o,c,s}` + `fnv1a(ssid)` 8 位 hex（10 字符，NVS 键上限 15）；
  全局兜底 `gml/gmo/gmc`（给以后新加的网络用）。旧键 `wxlat/wxlon/wxcity2/wxepo` 已废弃（残留无害）
- 配网页（P1）可手填**纬度/经度**把位置钉死；勾"我所有已保存的 WiFi 都在同一地点"则写入全部网络 + 全局
- 清空经纬度保存 = 删除绑定 → 回落 IP 自动定位
- 改城市名会自动重新 geocode（比对绑定里存的城市名）
- **`navigator.geolocation` 拿不到**：配网页是 `http://192.168.4.1`，私有 IP 的 HTTP **不是 secure context**
  （Chrome 50 起 geolocation 只在安全上下文可用）→ 只能手动粘贴手机地图的坐标，或给门户上 HTTPS
- 精度上限：免费 IP 库只到城市级（5~50km，移动/公司网络更差）；和风 current/daily/hourly 是 1~3km
  网格插值，城市级够用；**只有分钟级降水吃精度**。要更准只能手填坐标（或外挂 GPS，室内基本无信号）

## 电池电量

- 采样：16 次、丢弃前 4 次、**只在射频关闭时采**（WiFi 发射瞬时压降 0.1~0.2V 会污染 OCV）
- 换算：22 点开路电压(OCV)曲线 + 线性插值；**不要用线性映射**（锂电平台区误差可达 30~50%）
- 充电识别（三选一）：≥4150mV / 10 分钟涨 ≥15mV / **端电压顶在满电参考-15mV（恒压平台）**
- **充电电压补偿**：充电中端电压 = OCV + 电流×内阻，CC 段直接查表虚高 15~30%。
  按距平台距离线性补偿（差 120mV+ 补 60mV，平台处 0），只作用于 SoC 查表，不影响满电学习
- **显示限速 + 100% 锁存**：放电每 30s 最多 -1%（弛豫允许 +1%）、充电最多 +2%；
  到平台直接 100%，拔电后保持 20 分钟（或跌破 full-60mV）——消除"拔 USB 立刻掉 9x%"
- 满电学习：电压 ≥4.0V 稳定 30 分钟 + 最近 1 小时见过上升（防放电平台误学），
  **只升不降**（各次平台有 ±几十 mV 差异，往低学 = 电量虚高，实测曾 4095→4011）；
  NVS `bfull_learned`，配网页 `bfull` 可手动指定（手动优先）
- **空电锚点学习（0%）**：查表曲线底部 3300mV 但设备 ~3.4V 带载就断电 → 濒死时还显示 5~8%。
  记录见过的最低 EMA（<3450mV 临终区才采纳，防浅放用户被误锚定），NVS `bempty_learned`，
  按 ≥25mV 台阶写不伤 flash。**只有真正深放过一次电才生效**
- **SoC 查表 = 两端锚点仿射重标定**：把 [空电锚点..满电锚点] 映射到查表区间 [3300..4200]，
  0% = 设备真正断电电压、100% = 学到的满电平台。旧"只缩放顶部"平台区偏乐观 ~5-8%
- 分压比可在配网页校准（`bdiv`，默认 300 = 3.00）
- ⚠️ **中段曲线形状学不了**：无电流计就没有"流过多少电"的基准，库仑计数无从谈起。
  充电曲线倒是能按 CC 段时间学, 但充放电 OCV 有 20~50mV 迟滞, 学了反而错。
  根治方案 = 加 Fuel Gauge 芯片 (MAX17048, I2C, ModelGauge 自动学电池)
- ⚠️ 中段 CC 充电电压涨得慢（平台区 ~1.2mV/min < 15mV/10min 阈值），趋势检测可能漏判
  → 补偿暂不生效；该区段 dV/dSoC 小，误差有限。无电流计的已知妥协

## 每日一图 (Page 4, Bing 壁纸)

每天一次拉 Bing 当日壁纸 → 设备端 JPEG 解码 → 抖动 → **400×248 全屏 1-bit**。**零成本、无需 Key**。

```
1. GET https://www.bing.com/HPImageArchive.aspx?format=js&idx=0&n=1&mkt=zh-CN
   → images[0].urlbase + title ("深海夜花园")
2. GET https://www.bing.com{urlbase}_1920x1080.jpg&w=400&h=248&rs=1&c=4&pid=hp
   ★ Bing 服务端直接裁成 400×248, 只有 20KB (原图 340KB) —— 省流量也省解码
3. esp_new_jpeg 解码 (RGB565_LE) → 亮度 (0.299/0.587/0.114)
4. 自动色阶 + 平坦区保护抖动 → 1-bit
5. 存到 storage 分区 (裸分区, 非文件系统), 开机读回
```

- **抖动算法是关键**（详见 `art_dither()`）：
  - 直接 Floyd–Steinberg → 大片暗部/亮部长满麻点，照片认不出
  - 固定阈值 → 暗调照片（如星空）整幅变黑
  - ✅ **先按直方图 10%/90% 分位定黑白场**（自动色阶），再**只对中间调抖动**，
    极暗/极亮直接定死（`FLAT_LO=45` / `FLAT_HI=210`）→ 干净且保内容
- **格式约定（踩过坑）**：位图里 `bit=1` = **黑**。抖动时 `on=1` 表示"白"，写位要 **取反**
  （GDI+ 的 `Format1bppIndexed` 恰好相反: `bit=1`=白、`palette[0]`=黑，对拍时会骗人）
- **持久化用裸分区**：`storage` 分区（1MB，原本给 SPIFFS，本项目未用文件系统）
  → `esp_partition_erase_range/write/read`，头 `{magic,day,ver,title[40],crc}` + 12400B 位图 ≈ 12.5KB。
  **不能塞 NVS**：nvs 分区只有 24KB，且单条上限 ~4KB
- **渲染版本号 `ART_PROMPT_VER`**：改算法时 +1 → 已存的图作废重取一次（否则要等次日）
- 失败重试 3 次/天；计数按 (日+版本) 双键存 NVS，防重启后重复拉取
- PSRAM 缓冲: JPEG 192KB + RGB565 198KB + int16 亮度 198KB ≈ 590KB（8MB 里随便用）
- 成品用**密度渐变**（` .:-=+*#%@`）打到串口 —— 抖动图用 OR 降采样会糊成一片 `#`，必须按墨点密度

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
├── shtc3.c/h          # 板载 SHTC3 室内温湿度 (I2C 0x70, 用新版 i2c_master API)
├── art.c/h            # 每日一图 (Bing 壁纸 → JPEG 解码 → 1-bit 抖动, 全屏)
└── ui/                # ui.c/h + font_cjk_16 + font_qw_36/24/16 (和风图标) + font_wx_num_36
    └── icons/         # QWeather Icons 字体源 (ttf/json/LICENSE, 用于重新生成)
components/rlcd_display/  # ST7305 驱动
components/puff/          # DEFLATE 解压 (Mark Adler, 公共领域)
components/esp_new_jpeg/  # 软件 JPEG 解码 (收编的预编译库, 原因见踩坑 #11)
```

## UI (5 页：额度 / 消费柱状图 / 配网 / 天气 / 每日一图)

轮播顺序 `order[] = {0, 1, 3, 4}`（配网页 2 不参与），每页 15 秒。

**天气图标 = QWeather Icons 字体**（<https://icons.qweather.com>，MIT）：
- 源文件在 `main/ui/icons/`（ttf/json/LICENSE），生成命令（在 main/ui 下）：
  `npx lv_font_conv --font icons/qweather-icons.ttf --range 0xF101-0xF146,0xF21A,0xF2E6 --size N --bpp 1 --format lvgl --no-compress -o font_qw_N.c`
- 已生成 `font_qw_36`（当前天气，行高 **37**）、`font_qw_24`（4 日预报）、`font_qw_16`（行内空气质量）
- **码点映射必须查表**：`weather.c` 的 `QW_ICON_MAP[]`（和风代码→码点）。
  100-104→0xF101-105、300-318→0xF10A-11C、399→0xF11F、400-410→0xF120-12A、499→0xF12D…
  ⚠️ 3xx/4xx/5xx 的码点**不是**按代码线性排的（350 夜间阵雨=0xF11D 插在 318 和 399 之间）
- 图标以 UTF-8 存进 `wx->icon`（3 字节，`icon_utf8()`）；旧方案（Unicode ☀☁☂❄⚡ + font_wx_icon_36/24）已删除

天气页布局要点（**按导出字体真实度量排布**）：
- 字体行高：`font_cjk_16`=18（≠16!）, `font_qw_36`=37, `font_qw_24`=25, `font_wx_num_36`=28（≠36!）, Montserrat20=22
- 图标与温度**视觉中心对齐**（同为 44）：icon y=26（行高 37 → 中心 44.5）、temp y=30
- **室内温度**（板载 SHTC3）在温度行右端：`wx_in` x=252 y=35 w=134 右对齐（行高 18 → 中心 44）
- **温湿度配对**：室外 `湿度 86%` 紧跟大温度（`lv_obj_align_to` 在"度"后面 +10px），
  室内 `室内 25.9度 70%` 右端对齐
- **第二行三个固定格位**（固定 x 而非动态对齐, 因为三者都可能很长）：
  `wx_desc` 68..172 | `wx_feel` 180..276 | AQI 图标 290..308 + `wx_aqi` 306..386 右对齐
- **空气质量已图标化**：`wx_aqi_icon`（font_qw_16, air-quality 图标）代替"空气"二字,
  文本只剩"类别 数值"（类别>2 字时只显示类别）
- 大温度 x 从 104 挪到 **68** 是为了给"湿度紧跟温度"腾位置：`font_wx_num_36` 每字符 18px,
  最坏 `-10.5` = 90px, 左侧组止于 252, 右侧组起于 258, 仍不重叠
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
8. **配网页 HTML 缓冲**：`PORTAL_HTML_MAX` 要 8192（附近热点 `datalist` 最多 24 条 × 最长 32 字节 SSID
   ≈ 1.2KB，静态文案 ≈ 3KB）。`append()` 溢出会**静默截断**，表单末尾被砍掉后浏览器仍能提交（自动补全标签），
   故障表现为"某些字段莫名丢失"→ 已加 `portal html: N/M bytes` 日志 + 截断时 `ESP_LOGE`
9. **`snprintf` + `%.4f` 触发 `-Werror=format-truncation`**：gcc 按 double 最坏情况（~316 字符）算，
   任何小于 700 字节的缓冲都会编译失败 → 坐标一律用整数格式化（`%d.%04d`，见 `fmt_x1e4()`）
10. **SHTC3 的 ID 不要严格比对**：本板实测 `id=0x0887`（CRC 正确），而 datasheet 标称 `0x0807`。
    严格比对会把一个完全正常的传感器判成"不在位"→ 只校验 **CRC 通过** 即采用，型号不符仅告警
11. **托管组件带预编译 .a 时，中文路径会让 ld 挂掉**：`esp_new_jpeg` 用
    `add_prebuilt_library("${CMAKE_CURRENT_SOURCE_DIR}/lib/...")`，托管组件路径被解析成
    `D:/ESP/监控/...`（Python 组件管理器会把 subst 盘符还原成真实路径），`ld.exe` 报
    "cannot find .../libesp32s3/libesp_new_jpeg.a"——**文件其实存在**。
    解法：收编为本地组件 `components/esp_new_jpeg/`（本地组件路径是 `X:/cc_mate/components/...`，ASCII）。
    这也是为什么不能只靠 subst
12. **1-bit 位图的 bit 语义**：本项目位图 `bit=1`=**黑**；而 GDI+ `Format1bppIndexed` 里
    `bit=1`=白（palette[0]=黑）。两边的渲染结果做对比时极易被反相骗过去——首版每日一图
    就因为写位没取反，整幅图黑白颠倒，靠"设备 vs 电脑"逐字符对比才抓出来
