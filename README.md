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

屏幕引脚：`MOSI=12 SCLK=11 CS=40 DC=5 RST=41 TE=6`

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

```
┌──────────────────────────────────────────┐
│ 无锡市                     更新于 2 分钟前 │
│    ☁          27.0 度                     │
│   (36px)      阴              湿度 64%    │
│ ──────────────────────────────────────── │
│    今天       明天       周三       周四   │
│     ☁         ☂         ☀         ☀     │
│   30/22     28/20     31/21     32/22    │
│ 数据源 Open-Meteo                         │
└──────────────────────────────────────────┘
```

### 配网页（AP 模式）

屏幕显示热点名与配置地址；浏览器打开 `http://192.168.4.1` 填表。

---

## 数据源

全部为 HTTPS + API Key，**不需要账号登录、不需要 userToken**（不会过期）。

| 数据 | 接口 | 认证 |
|------|------|------|
| 智谱额度 | `GET https://bigmodel.cn/api/monitor/usage/quota/limit?type=1` | `Authorization: <api key>`（raw，无 Bearer）+ `bigmodel-organization` + `bigmodel-project` |
| DeepSeek 余额 | `GET https://api.deepseek.com/user/balance` | `Authorization: Bearer <api key>` |
| 天气预报 | `GET https://api.open-meteo.com/v1/forecast` | 无需 Key |
| 城市定位 | `GET https://geocoding-api.open-meteo.com/v1/search` | 无需 Key |
| IP 定位 | `http://ip-api.com/json/`（主）/ `https://api.ip.sb/geoip`（备） | 无需 Key |
| 时间 | SNTP `ntp.aliyun.com` / `pool.ntp.org` | — |

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

| 配网页「城市」字段 | 行为 |
|---|---|
| **留空**（默认） | IP 自动定位（`ip-api.com` 主、`ip.sb` 备），坐标缓存 **24 小时**，换网络自动跟随 |
| 填写城市名（如 `上海`） | Open-Meteo geocoding 精确定位，坐标永久缓存 |

天气码（WMO）映射为中文描述与图标；当前天气 36px 图标、预报 24px 图标。

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

## 配网

1. 首次开机 / WiFi 连接失败（20 次重试）/ **长按 BOOT 3 秒** → 进入 AP 配网模式
2. 手机连接热点 `AI-Mate-Setup`（密码 `aimate123`）
3. 浏览器打开 `http://192.168.4.1`，填写：
   - WiFi 名称 / 密码（**仅支持 2.4GHz**）
   - 智谱 API Key、Organization ID、Project ID、套餐类型（1 个人 / 2 团队）
   - DeepSeek API Key
   - 城市（可选，留空 = IP 自动定位）
   - 轮询间隔（分钟，1–60）
4. 保存 → 写入 NVS → 自动重启连接

配置完整性校验失败会返回错误页而非误报成功。

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
├── net_query.c/h        # GLM/DeepSeek 查询、SNTP、消费历史推算、通用 HTTPS GET
├── weather.c/h          # Open-Meteo 查询、IP/城市定位、WMO 码→中文/图标
└── ui/
    ├── ui.c/h           # 四页 UI（额度/柱状图/配网/天气）
    ├── font_cjk_16.c    # 中文 + ASCII + 标点（含天气符号）
    ├── font_wx_icon_36.c / _24.c   # 天气图标（当前 / 预报）
    └── font_wx_num_36.c            # 大号温度数字
components/rlcd_display/  # ST7305 驱动 + LVGL 桥接
```

### NVS 键

| 命名空间 | 键 |
|---|---|
| `ai_mate` | `ssid` `pass` `gkey` `gorg` `gproj` `dkey` `wcity` `gtype` `pmin` |
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
- IP 定位为城市级（运营商出口），要精确到区请手填城市名
- ESP32 仅支持 **2.4GHz** WiFi
- 天气为 Open-Meteo 数据源，国内精度一般；如需更准可换高德/和风（需 Key）

## 致谢

- 原始硬件与驱动：[cc_mate](https://github.com/vincezhaojie-lang/cc_mate)
- 每日消费推算思路：[quote0-deepseek-balance](https://github.com/SamLinBIT/quote0-deepseek-balance)
- 智谱额度接口与 org/project 用法：[pi-glm-quota](https://github.com/focksor/pi-glm-quota)
- 天气数据：[Open-Meteo](https://open-meteo.com/)

## License

MIT
