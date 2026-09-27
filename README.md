# AI-Mate — AI 额度与天气桌面摆件

ESP32-S3 + 4.2" 反射式墨水屏（400×300 纯黑白），**WiFi 直连**查询 **智谱 GLM Coding Plan 额度**、**DeepSeek 余额** 与 **本地天气**，用余额快照推算每日消费并绘制 30 天柱状图。

完全独立运行：插上 USB 电源即可，无需电脑、无需蓝牙、无需账号登录（全部走 API Key）。

> 由 Claude Code 监控伴侣 [cc_mate](https://github.com/vincezhaojie-lang/cc_mate) 改造而来：BLE/PC 桥接 → WiFi 独立运行，并新增额度/消费/天气三大功能。

---

## 功能

- **智谱 GLM Coding Plan**：5 小时滚动窗 + 周窗的已用百分比、剩余积分、重置倒计时
- **DeepSeek**：总余额 / 赠金余额 / 充值余额，以及今日、本月消费
- **消费统计**：余额快照推算每日消费，30 天柱状图 + 日均/最高
- **天气**：当前天气 + 4 日预报（Open-Meteo，免 API Key），支持 **IP 自动定位**
- **室内温湿度**：读板载 SHTC3，室内温度显示在室外温度同一行的右端（对比一目了然）
- **免电脑配网**：手机连热点填表即可，配置存 NVS；换 WiFi/换 Key 长按 BOOT 重配
- **省电**：80MHz + tickless idle + WiFi 查询后断射频（duty-cycle），平均电流约 15–25mA
- **1-bit 屏优化**：描边进度条、虚线网格、专用图标字体

---

## 硬件

| 项目 | 规格 |
|------|------|
| 主控 | ESP32-S3（Waveshare ESP32-S3-RLCD-4.2） |
| 屏幕 | 400×300 横屏，1-bit 黑白，ST7305 反射式 LCD（不刷新时零功耗保持画面） |
| 通信 | WiFi STA（查数据）+ SoftAP（配网门户） |
| 按键 | BOOT / GPIO0，长按 3 秒重新配网 |
| 电池 | ADC GPIO2（可选） |
| 室内温湿度 | 板载 SHTC3（I2C 0x70，SDA=GPIO13 / SCL=GPIO14） |

屏幕引脚：`MOSI=12 SCLK=11 CS=40 DC=5 RST=41 TE=6`

> 板载但本项目**尚未使用**：ES8311 codec + ES7210 双麦 + 喇叭座（I2S 8/9/10/16/45）、
> PCF85063 RTC（同一条 I2C 总线）、TF 卡槽、KEY 键（GPIO18）、8MB PSRAM、约 11MB 空闲 Flash。

---

## 三个页面（每 15 秒轮播）

### 第 1 页 · 额度总览

```
┌ 智谱 GLM Coding Plan ────────────────┐
│ 5h 窗  ████████░░░░░░░░░░░░░   15%   │
│ 周 窗  ██████░░░░░░░░░░░░░░░   17%   │
│ 剩余积分   5h 10181/12000  周 49373/60000 │
│ 重置倒计时  5h 2h41m   周 4d3h        │
├ DeepSeek 余额 ───────────────────────┤
│ 115.88 元              ← 20px 大号    │
│ 赠金余额 0.00 元   充值余额 115.88 元 │
│ 今日消费 1.23 元   本月消费 8.36 元   │
│ 数据已更新                            │
└──────────────────────────────────────┘
```

余额不足 10 元时右侧出现「余额偏低」，账户异常时出现「账户停用」。

### 第 2 页 · 30 天消费柱状图

```
│ DeepSeek 每日消费 (近30天)     满格 20 元 │
│  ┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈  75% │
│  ┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈  50% │
│  ▁▁▃▂▂▁▁▂▁▁▂▁▅▂▁▁▃▂▁▁▂▁▁▃▁▁▂▁▁▁▂▁▁  25% │
│ ▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔      │
│ 8/24        9/07        今天           │
│ 今日 1.23 元   本月 8.36 元   30天 38.50 元 │
│ 日均 1.28 元   最高 4.30 元 (9/12)     │
```

### 第 3 页 · 天气

**有天气预警时**（横幅占据一行，内容下移）：

```
┌──────────────────────────────────────────────────┐
│ 无锡市              和风天气 · 更新 2 分钟前        │
│    ☁          23.1 度                             │
│   (36px)      阴                    湿度 97%      │
│ ──────────────────────────────────────────────── │
│ ▌! 大风蓝色预警  (+1 条)▐   ← 反白横幅              │
│ ☔ 间歇性降雨还将持续50分钟       峰值 0.20mm/5m   │
│    今天       明天       周三       周四           │
│     ☁         ☂         ☀         ☀             │
│   30/22     28/20     31/21     32/22            │
│    ▂▄█▆▃▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁  ← 未来2小时降水      │
│  现在          +1小时          +2小时             │
└──────────────────────────────────────────────────┘
```

**无预警时**（当前常态，内容整体上移 26px，柱状图更高）：

```
┌──────────────────────────────────────────────────┐
│ 无锡市              和风天气 · 更新 2 分钟前        │
│    ☁          23.5 度                             │
│   (36px)      阴                    湿度 95%      │
│ ──────────────────────────────────────────────── │
│ ☔ 间歇性降雨还将持续60分钟       峰值 0.45mm/5m   │
│    今天       明天       周三       周四           │
│     ☁         ☂         ☀         ☀             │
│   30/22     28/20     31/21     32/22            │
│         ▃▅█▇▅▃▂▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁               │
│  现在          +1小时          +2小时             │
└──────────────────────────────────────────────────┘
```

**天气预警**（和风 `weatheralert`）：显示当前生效预警，多条时选最严重的一条并标注 `(+N 条)`。
橙色及以上预警前缀 `!` 强调。1-bit 屏无法用颜色，故用**黑底白字反白横幅**表达严重性。
无预警时不占位——下方内容上移、柱状图高度上限从 20px 增到 46px，避免留白。

**分钟级降水**（和风 `minutely/5m`）：未来 2 小时、每 5 分钟一格共 24 格柱状图，
并直接使用和风的自然语言摘要（如「间歇性降雨还将持续50分钟」）+ 峰值强度（mm/5min）。
无和风 Key 时，退化为逐小时预报推算的「N小时后有雨 (概率%)」文本，柱状图隐藏。

> 布局按**导出字体的真实度量**排布（`font_cjk_16` 行高 18、`font_wx_icon_36` 36、
> `font_wx_num_36` 28、Montserrat 20 行高 22），所有动态文本标签均设 `LV_LABEL_LONG_DOT`，
> 超长显示省略号而不是换行压住下一行。

### 配网页（AP 模式）

屏幕显示热点名与配置地址；浏览器打开 `http://192.168.4.1` 填表。

---

## 数据源

全部为 HTTPS + API Key，**不需要账号登录、不需要 userToken**（不会过期）。

| 数据 | 接口 |
|------|------|
| 智谱额度 | `GET https://bigmodel.cn/api/monitor/usage/quota/limit?type=1`，Header: `Authorization`(raw key) + `bigmodel-organization` + `bigmodel-project` |
| DeepSeek 余额 | `GET https://api.deepseek.com/user/balance`，Header: `Authorization: Bearer <key>` |
| 天气（默认） | `GET https://api.open-meteo.com/v1/forecast`（免 Key，含 current/daily/hourly） |
| 天气（可选） | `GET https://<你的API Host>/weather/v1/{current,daily,hourly}/{lat}/{lon}`、`/weatheralert/v1/current/{lat}/{lon}`、`/v7/minutely/5m?location=lon,lat`，Header: `X-QW-Api-Key`（和风天气） |
| 城市定位 | `GET https://geocoding-api.open-meteo.com/v1/search`（免 Key） |
| IP 定位 | `http://ip-api.com/json/`（主）/ `https://api.ip.sb/geoip`（备），免 Key |
| 时间 | SNTP `ntp.aliyun.com` / `pool.ntp.org` |

**智谱 org/project 获取方法**：浏览器登录 `bigmodel.cn/coding-plan` → F12 → Network → 找 `quota/limit` 请求 → 复制请求头的 `bigmodel-organization` 与 `bigmodel-project`。`type=1` 个人版，`type=2` 团队版。

**DeepSeek 字段语义**（[官方文档](https://api-docs.deepseek.com/api/get-user-balance)）：`总余额 = 赠金余额 + 充值余额`；`topped_up_balance` 是**剩余充值余额**，不是累计充值额（累计值该接口查不到）。

---

## 每日消费推算（无需 userToken）

```
每日消费 = 当日开盘余额 − 当日收盘余额 + 当日充值
充值检测 = 两次快照间隔 < 35min 且余额跳增 ≥ 8 元
设备离线跨多天 → 中间天记为无数据（柱状图留空）
```

> ⚠️ 这是**设备观测值**：当天设备首次轮询之前的消耗无法计入；「本月消费」从设备开始观测那天起累加。平台官方精确明细需要会过期的 userToken，故未采用。

---

## 天气与定位

**双数据源**（配网页填了和风参数就用和风，留空自动用 Open-Meteo）：

| 数据源 | 是否需要 Key | 特点 | 计费 |
|---|---|---|---|
| Open-Meteo（默认） | 不需要 | 全球覆盖，国内精度一般 | 免费无配额 |
| [和风天气](https://dev.qweather.com) | 需 API Host + API Key | 国内 1 公里分辨率、分钟级更新、中文天气现象 | 每自然月**前 5 万次免费**，之后 ¥0.0007/次 |

天气刷新与轮询**解耦**：默认 30 分钟一次（配网页可调 5–240 分钟）。和风路径每次查询 5 个接口（实时/每日/逐小时/预警/分钟级降水），按默认设置约 **7,200 次/月**，占和风免费额度的 14.4%；Open-Meteo 路径只需 1 次请求且免 Key。

> 数据来源标注：使用和风天气时，天气页页脚显示「数据源 和风天气」（这是和风[服务条款](https://dev.qweather.com/docs/terms/attribution/)的要求）。
>
> ⚠️ 和风天气会**无视 `Accept-Encoding: identity` 直接返回 gzip**（其所有文档示例都带 `--compressed`）。ESP-IDF 没有 zlib 组件，因此本项目内置了 `components/puff`（公共领域的 DEFLATE 解压实现）自动识别 `1F 8B` 魔数并解压，无需额外配置。

**定位：位置跟着 WiFi 网络走**

WiFi 是固定的 → 它所在的位置也是固定的，所以坐标**按 SSID 永久绑定**（不再每天重查 IP——重查只会
让 IP 库偶尔给个「邻居市」，城市名和天气跟着跳）。

优先级：**本网络手填坐标 > 本网络城市名解析 > 本网络 IP 自动定位 > 全局手填坐标**

| 配网页字段 | 行为 |
|---|---|
| **纬度 / 经度 都留空**（默认） | 首次在该网络下查询时 IP 自动定位（`ip-api.com` 主、`ip.sb` 备），结果**永久绑定到该 SSID** |
| 填写城市名（如 `上海`） | geocoding 解析一次，永久绑定；改了城市名会自动重新解析 |
| **手填纬度 / 经度** | 钉死坐标，精度到 ~100m（免费 IP 库只到城市级，误差 5~50km） |
| 勾「我所有已保存的 WiFi 都在同一地点」 | 上面填的坐标写入全部已保存网络，并对以后新加的网络生效 |
| 清空经纬度后保存 | 删除绑定 → 回落 IP 自动定位 |

坐标从哪来：手机地图（高德/百度/苹果）**长按落点 → 复制坐标**，粘进配网页即可。

> 为什么不让配网页自动读手机定位？配网页是 `http://192.168.4.1`，私有 IP 的 HTTP **不是 secure context**，
> 而 `navigator.geolocation` 从 Chrome 50 起只在安全上下文可用 → 会被浏览器直接拒绝。除非给门户上 HTTPS。

两源共用同一套经纬度，无需额外查询。

---

## 省电设计

| 措施 | 说明 |
|------|------|
| CPU 80MHz | `CONFIG_ESP32S3_DEFAULT_CPU_FREQ_80`（默认 160MHz） |
| Tickless idle | `CONFIG_PM_ENABLE` + `CONFIG_FREERTOS_USE_TICKLESS_IDLE`，空闲自动 light sleep |
| WiFi MAX modem sleep | `esp_wifi_set_ps(WIFI_PS_MAX_MODEM)`，信标间隙射频休眠 |
| **查询后断射频** | 每轮：唤醒 → 连接 → 查询 → `esp_wifi_stop()` → 睡 N 分钟。**射频在线时间约 1.3%**（5 分钟周期下约 4 秒） |
| UI 2 秒刷新 | 时钟为分钟级，无需更快 |
| RLCD 特性 | 不刷新时零功耗保画面 |

断射频期间时钟/UI/电量照常（light sleep 保持 RTC），顶栏状态显示「休眠」。实测唤醒重连约 1.1 秒。

估算平均电流约 **15–25mA**（未优化前常驻连接约 100mA）。

---

## 配网与多网络

### 连接策略（最多记住 4 个 WiFi）

| 场景 | 行为 |
|---|---|
| 开机 / 换环境 | **扫描附近热点，从已保存网络中选信号最好的接入** |
| 省电周期唤醒 | 直接用上次成功的 SSID 快速重连（不扫描，省 ~2s 射频时间） |
| 连不上 | 重试 3 次 → 扫描换用其他已保存网络 → 全失败才进配网门户 |
| 保存新 WiFi | 自动加入列表（同名则更新密码），旧的保留，回到旧环境也能连 |

状态栏会显示当前连接的 SSID，例如 `ASUS · 更新于 2 分钟前`。

### 配网步骤

1. 首次开机 / 所有已知网络都连不上 / **长按 BOOT 3 秒** → 进入 AP 配网模式
2. 手机连接热点 `AI-Mate-Setup`（密码 `aimate123`）
3. 浏览器打开 `http://192.168.4.1`，表单会：
   - 列出**已保存的 WiFi**（开机自动择优连接）
   - 提供**附近热点下拉候选**（`<datalist>`，带信号强度）
   - 回填现有的智谱 / DeepSeek / 天气配置（改单项不用重填）
4. 填写内容：
   - WiFi 名称 + 密码（**仅支持 2.4GHz**；留空 = 保持已保存网络不变）
   - 智谱 API Key、Organization ID、Project ID、套餐类型（1 个人 / 2 团队）
   - DeepSeek API Key
   - 城市名（可选，只用于显示；留空 = 用定位得到的城市名）
   - **纬度 / 经度**（可选，填了就固定用这个坐标，清空 = 按 IP 自动定位）
   - 「我所有已保存的 WiFi 都在同一地点」勾选框（多频段/多 SSID 只需填一次）
   - 和风 API Host / API Key（可选，留空 = 用免费 Open-Meteo）
   - 天气刷新间隔（分钟，5–240，默认 30）
   - 轮询间隔（分钟，1–60）
   - 电池分压比（默认 3.00，电量显示偏差时校准）
5. 保存 → 写入 NVS → 自动重启连接；勾选「清除所有已保存的 WiFi」可重置网络列表

> 设备重启后会自动迁移旧版单网络配置，无需重新配网。

---

## 构建

ESP-IDF **v5.5+**，目标 ESP32-S3。

```bat
:: 项目路径含非 ASCII 字符时，先映射成盘符（部分 IDF 工具对非 ASCII 路径敏感）
subst X: <项目路径>

X:\build.bat clean      :: 清理
X:\build.bat            :: 构建
X:\build.bat flash COM3 :: 烧录
X:\build.bat monitor COM3
```

或使用标准流程：`idf.py set-target esp32s3 && idf.py build flash monitor`

> 构建日志中的 `ESP_ROM_ELF_DIR environment variable is not defined`（gdbinit 警告）不影响产物，可忽略。
> 若用 esptool 手动烧录，`@flash_args` 的相对路径以 `build/` 为工作目录：
> `python <IDF>/components/esptool_py/esptool/esptool.py --chip esp32s3 -p COM3 write_flash @X:/<项目>/build/flash_args`

---

## 项目结构

```
main/
├── cc_mate.c/h          # 入口、主循环、页面轮播、电池 ADC、BOOT 键
├── config_store.c/h     # NVS 配置读写
├── wifi_mgr.c/h         # WiFi STA + AP 配网门户 + 射频开关（省电）
├── net_query.c/h        # GLM/DeepSeek 查询、SNTP、消费历史推算、通用 HTTPS GET、gzip 解压
├── weather.c/h          # 双数据源天气（和风/Open-Meteo）、IP/城市定位、图标映射
└── ui/
    ├── ui.c/h           # 四页 UI（额度/柱状图/配网/天气）
    ├── font_cjk_16.c    # 中文 + ASCII + 标点（含天气符号）
    ├── font_wx_icon_36.c / _24.c   # 天气图标（当前 / 预报）
    └── font_wx_num_36.c            # 大号温度数字
components/
├── rlcd_display/         # ST7305 驱动 + LVGL 桥接
└── puff/                 # DEFLATE 解压（zlib 作者 Mark Adler, 公共领域）
                          # ESP-IDF 无 zlib 组件；和风天气无视 identity 直接回 gzip，
                          # 故内置 puff 自行解析 gzip 容器后解压
```

### NVS 键

| 命名空间 | 键 |
|---|---|
| `ai_mate` | `gkey` `gorg` `gproj` `dkey` `gtype` `pmin` · 天气: `wcity` `qwhost` `qwkey` `wmin` · 电池: `bdiv` · WiFi: `nnet` `s0..s3` `p0..p3` `last`（+ 兼容旧字段 `ssid` `pass`） |
| `ai_hist` | `cents`(63 天消费) `base` `dbase` `rechg` `lbal` `lepo` `wxlat` `wxlon` `wxcity2` `wxepo` |

---

## 字体生成

字体用 [lv_font_conv](https://github.com/lvgl/lv_font_conv) 从 Windows 系统字体裁剪：

```bash
# 中文字体：simhei 提供 CJK/ASCII，SEGOE UI SYMBOL 补天气符号（simhei 缺 ☀☁☂ 等字形）
npx lv_font_conv --font C:/Windows/Fonts/simhei.ttf \
  --range 0x0020-0x007F,0x2000-0x206F,0x2580-0x259F,0x2600-0x27BF,0x3000-0x303F,0x4E00-0x9FFF,0xFF00-0xFF5F \
  --font C:/Windows/Fonts/seguisym.ttf --symbols "☀☁☂☰☔❄⚡·" \
  --size 16 --bpp 1 --format lvgl --no-compress -o font_cjk_16.c

# 天气图标（大/小）与大号数字
npx lv_font_conv --font C:/Windows/Fonts/seguisym.ttf --symbols "☀☁☂☔☰❄⚡" \
  --size 36 --bpp 1 --format lvgl --no-compress -o font_wx_icon_36.c
npx lv_font_conv --font C:/Windows/Fonts/seguisym.ttf --symbols "☀☁☂☔☰❄⚡" \
  --size 24 --bpp 1 --format lvgl --no-compress -o font_wx_icon_24.c
npx lv_font_conv --font C:/Windows/Fonts/simhei.ttf --symbols "0123456789./-" \
  --size 36 --bpp 1 --format lvgl --no-compress -o font_wx_num_36.c
```

两个注意点：
1. **声明了区间 ≠ 有字形**：simhei 不含 ☀☁☂❄⚡，lv_font_conv 会静默跳过 → 屏幕显示方块。必须用 `--symbols` 从含这些符号的字体补齐。
2. 生成的文件默认包含 `#include "lvgl/lvgl.h"`，需改为 `#include "lvgl.h"`（本工程组件布局）。

---

## 1-bit 黑白屏适配要点

驱动把 RGB565 按亮度二值化（`lum >= 80 → 白`，否则黑），因此：

- 任何"浅灰"都会变成白色而**不可见** → 进度条轨道用 **1px 黑描边**、图表参考线用 **黑色虚线**、网格用四分填充
- Montserrat 等拉丁字体**没有 CJK 字形** → 数字与「元/度」分属两个 label，用 `lv_obj_align_to` 动态贴合
- 不能靠灰度区分层次 → 用字号（36/24/20/16px）、描边、反白、占位密度来表达

---

## 已知限制

- 每日消费、本月消费、柱状图均为**设备观测推算值**，非平台官方数据
- IP 定位为城市级（运营商出口，误差 5~50km；移动/公司网络更差）。要更准只有两条路：
  配网页**手填经纬度**（~100m），或外挂 GPS 模块（但室内基本收不到信号）
- 配网页**无法自动读取手机定位**：`http://192.168.4.1` 不是 secure context，浏览器会拒绝 `navigator.geolocation`
- 位置与 WiFi 网络绑定：**换到新网络会重新定位一次**，之后永久记住；同一地点有多个 SSID 时勾「都在同一地点」
- ESP32 仅支持 **2.4GHz** WiFi
- 天气为 Open-Meteo 或和风天气；国内精度要求高请在配网页填和风 API Host + Key
- 使用和风天气时需保留页脚的数据来源标注（服务条款要求）
- 电量由开路电压查表估算，充电中读数会略偏乐观（充电电压高于静置电压）

## 致谢

- 原始硬件与驱动：[cc_mate](https://github.com/vincezhaojie-lang/cc_mate)
- 每日消费推算思路：[quote0-deepseek-balance](https://github.com/SamLinBIT/quote0-deepseek-balance)
- 智谱额度接口与 org/project 用法：[pi-glm-quota](https://github.com/focksor/pi-glm-quota)
- 天气数据：[Open-Meteo](https://open-meteo.com/)

## License

MIT
