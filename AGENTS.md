# AGENTS.md

## 项目速览

ESP32-S3 + 2.9" SSD1680 墨水屏天气面板，ESP-IDF/C。源码 `main/`，主机回归 `tools/`。两个页面：按天（5 列）和按小时（10 列，每小时一列），按键切换。接口获取 5 天 + 21 条逐时数据；逐时页只显示前 10 条，不要把请求条数和显示列数混用。

本文描述当前实现；修改行为时同步更新相关测试与说明。`README.md`、`ABLATION.md` 中部分内容是历史记录，尤其是“不保留旧天气快照”和“切页强制全刷”，不能直接当作当前约束。

## 通用能力复用原则

- 优先使用项目已有、ESP-IDF 提供或成熟稳定的第三方库处理通用能力，不重复实现已有的解析器、编码器、协议处理、数据结构或其他基础设施；例如 JSON 使用 `components/cjson`，HTTP(S)、DNS、TLS 和网络协议优先使用 ESP-IDF API。
- 不要自行实现通用 DNS 协议、报文编解码、查询解析或网络基础设施；优先复用 ESP-IDF/lwIP 或成熟库。`portal_dns` 只有在现有 API 无法覆盖配网热点的设备特定强制解析行为时，才允许保留最小适配代码。
- 自己实现的代码只负责业务规则、设备特定适配和库无法覆盖的边界；天气字段范围、日期连续性、刷新策略等业务校验仍由项目维护。
- 准备新增通用工具前，先检查现有依赖和 ESP-IDF 组件。确实没有合适库，或引入库的资源、许可证、平台兼容性不满足要求时，才实现最小必要逻辑，并在说明和测试中记录原因。
- 新增库时同步更新组件目录、`CMakeLists.txt`、依赖清单、主机回归 harness、许可证和相关说明；固件与主机测试应尽量共用同一份库源码。

## 常用命令

在项目根目录运行；主机回归和预览只需 Python 标准库及本机 C 编译器，无需 ESP-IDF 或联网。

```bash
python3 tools/test_app.py                 # 应用、解析、按键、UI 全量回归
python3 tools/test_portal.py              # 配网页模板、转义及 HTTP 分块发送回归
python3 tools/test_app.py --only weather  # 仅天气解析
python3 tools/test_app.py --only ui       # 仅 UI
python3 tools/test_app.py --only app      # 仅应用流程
python3 tools/preview_ui.py              # 按天页预览及像素断言
python3 tools/preview_ui.py --sanitize    # 可选：AddressSanitizer / UBSan
```

预览输出 `build/preview-5day.png`（3 倍放大）、`build/preview-5day-native.png`（296×128）及各状态图片，均为演示数据，不是实时天气。`test_app.py` 的生成物放在系统临时目录。

本机使用 ESP-IDF v6.0.2。已激活环境时运行 `idf.py build`；否则可直接用 IDF 管理的 Python 环境：

```bash
env IDF_PATH=/Users/sharp/.espressif/v6.0.2/esp-idf \
    IDF_PYTHON_ENV_PATH=/Users/sharp/.espressif/tools/python/v6.0.2/venv \
    ESP_IDF_VERSION=6.0.2 \
    /Users/sharp/.espressif/tools/python/v6.0.2/venv/bin/python \
    /Users/sharp/.espressif/v6.0.2/esp-idf/tools/idf.py build
```

- 不要用 `bash -lc '. .../export.sh'`：登录 shell 可能先找到系统 Python 3.9，导致 IDF 拒绝激活。
- 烧录前确认实际串口，再将末尾的 `build` 改为 `-p 实际串口 flash`；不要把历史设备路径当作固定值。`idf.py monitor` 在交互终端运行。
- 目标和默认配置见 `sdkconfig.defaults`：ESP32-S3、8 MB Flash、USB Serial/JTAG、主任务栈 10240 字节。已有 `sdkconfig` 时修改 defaults 不一定改变当前配置，需核对两者。

## 字体与字模生成

仓库提交的是生成后的 `main/fonts_aa.c` 和 `main/font_icons.c`，固件构建不需要字体文件、Pillow、Swift、联网或 `.cache/`。只有修改屏幕文案、字号、文字字体、Weather Icons 映射或字形布局时才需要重新生成；生成后必须运行字模/布局回归。不要把下载的 TTF、CSS 或预览 PNG 提交到仓库。

### 文字字模：`fonts_aa.c`

`tools/genfont.py` 使用 Pillow 的 FreeType `mode='1'` 单色栅格器生成 1bpp、MSB-first、逐行字节补齐的 C 数组，输出固定为 `main/fonts_aa.c`。输入字体只在生成时使用：

- 拉丁字母、数字和 `°` 使用 Google Fonts 的 Roboto Variable，Apache License 2.0。
- 中文使用 Google Fonts 的 Noto Sans SC Variable，SIL Open Font License 1.1；只有 `font_mid`（16px）带中文。
- 每种字号始终包含 ASCII `U+0020..U+007F` 和度符号 `U+00B0`；中文字符由 `main/main.c`、`main/ui.c`、`main/weather.c` 中的 C 字符串自动扫描，网页 `portal_web.c` 的文案不进入字模。
- 当前输出为 `font_huge` 46px Bold、`font_mid` 16px Medium、`font_temp` 14px Medium、`font_small` 12px Regular。字号、字重、是否带中文在 `tools/genfont.py` 的 `SPECS` 中维护。

输入路径由环境变量覆盖，默认值是当前目录下的 `Roboto[wdth,wght].ttf` 和 `NotoSansSC[wght].ttf`：

```bash
mkdir -p .cache/fonts
curl -fL 'https://github.com/google/fonts/raw/main/ofl/roboto/Roboto%5Bwdth,wght%5D.ttf' \
    -o '.cache/fonts/Roboto[wdth,wght].ttf'
curl -fL 'https://github.com/google/fonts/raw/main/ofl/notosanssc/NotoSansSC%5Bwght%5D.ttf' \
    -o '.cache/fonts/NotoSansSC[wght].ttf'
python3 -m pip install Pillow  # 若当前 Python 环境尚未安装 Pillow
FONT_TTF='.cache/fonts/Roboto[wdth,wght].ttf' \
FONT_CJK_TTF='.cache/fonts/NotoSansSC[wght].ttf' \
python3 tools/genfont.py
```

路径含 `[`、`]` 时必须加引号；脚本会从项目根目录读取上述 `main/*.c`，所以命令也必须在项目根目录运行。若只改了屏幕文案，通常无需重新下载字体，保留原 TTF 后直接重新运行最后三行即可。生成后检查 `main/fonts_aa.c` 的头部和缺字情况，并运行：

```bash
python3 tools/test_app.py --only ui
python3 tools/preview_ui.py
```

### Weather Icons 字模：`font_icons.c`

`tools/genicons.swift` 在 macOS 上使用 CoreGraphics、CoreText、CryptoKit、ImageIO，从固定版本的 Weather Icons TTF 轮廓生成两套关闭抗锯齿的 1bpp 子集：`font_icon_daily`（advance 40、cell 高 27）和 `font_icon`（advance 26、cell 高 23）。来源、映射、版权和完整 SIL OFL 1.1 许可见 `third_party/weather-icons/README.md`、`OFL.txt`。生成器会校验 TTF SHA256，不能用系统里同名但内容不同的字体：

- 上游提交：`bb80982bf1f43f2d57f9dd753e7413bf88beb9ed`
- TTF SHA256：`176bda6661f213dde47c2114d76e476ec8ca9aae07dd54f9550d2d28fe02b4fd`
- TTF 和预览只放在被 `.gitignore` 排除的 `.cache/weather-icons/`；CSS 仅用于核对官方名称和码点，生成器本身不读取 CSS。

```bash
mkdir -p .cache/weather-icons
curl -fL --max-time 60 \
    'https://raw.githubusercontent.com/erikflowers/weather-icons/bb80982bf1f43f2d57f9dd753e7413bf88beb9ed/font/weathericons-regular-webfont.ttf' \
    -o .cache/weather-icons/weathericons-regular-webfont.ttf
curl -fL --max-time 60 \
    'https://raw.githubusercontent.com/erikflowers/weather-icons/bb80982bf1f43f2d57f9dd753e7413bf88beb9ed/css/weather-icons.css' \
    -o .cache/weather-icons/weather-icons.css
shasum -a 256 .cache/weather-icons/weathericons-regular-webfont.ttf .cache/weather-icons/weather-icons.css
swift tools/genicons.swift \
    .cache/weather-icons/weathericons-regular-webfont.ttf \
    main/font_icons.c \
    .cache/weather-icons/preview.png
shasum -a 256 main/font_icons.c
open .cache/weather-icons/preview.png
```

如果只想验证重生成而不覆盖已提交文件，把输出改成缓存文件并逐字节比较：

```bash
swift tools/genicons.swift \
    .cache/weather-icons/weathericons-regular-webfont.ttf \
    .cache/weather-icons/font_icons.regenerated.c
cmp main/font_icons.c .cache/weather-icons/font_icons.regenerated.c
```

图标生成后至少运行 `python3 tools/test_app.py` 和 `python3 tools/preview_ui.py`，并人工查看预览中的太阳/云/降水方向；若改变 `tools/genicons.swift`、图标映射或源字体，还要核对 `third_party/weather-icons/README.md` 的许可、SHA256 和生成结果。生成的 `.c` 文件仍需通过 `main/CMakeLists.txt` 编译进固件。

## 模块一览

| 文件 | 作用 |
| --- | --- |
| `components/cjson/` | 固件和主机回归共用的 cJSON 组件 |
| `main/main.c` | 启动、配网、重试、30 分钟更新、当前页与成功天气快照、刷新调度 |
| `main/button.c` / `.h` | GPIO4 低电平按键，内部上拉；2 次采样去抖，短按切页，长按 3s 重置 WiFi |
| `main/epd.c` / `.h` | SSD1680 SPI 驱动、4736 字节帧缓冲、绘图原语、全刷/局部刷新 |
| `main/ui.c` / `.h` | 五天页、10 列逐时页、传感器底栏、状态、重试与配网提示；只画帧缓冲 |
| `main/font_icons.c` / `.h` | Weather Icons 的 11 个 1bpp 图标子集；日页与逐时页使用两套固定尺寸字模 |
| `third_party/weather-icons/` | Weather Icons 来源、固定提交、SHA256 和 SIL OFL 1.1 许可说明 |
| `components/cjson/` | 固件和主机回归共用的 cJSON 组件 |
| `main/weather.c` / `.h` | 出口 IP 定位、Open-Meteo 请求、cJSON 数据校验、5 天 + 21 小时数据契约 |
| `main/weather_json.c` / `.h` | 天气解析共用的通用 cJSON 辅助：严格整段解析、数值/整数/定长字符串/对象成员数组读取；不含天气业务校验 |
| `main/net.c` / `.h` | NVS 凭据、STA 连接、SNTP/时区、HTTP(S) GET |
| `main/net_portal.c` | SoftAP 配网生命周期：AP/IP、WiFi 模式、DNS/网页服务启停 |
| `main/portal_web.c` / `.h` | 网页配网：热点扫描、模板渲染、`/save` 保存与凭据事件 |
| `components/dns_server/` | 复用 ESP-IDF captive portal 示例的 DNS server 组件 |
| `main/portal_dns.c` / `.h` | 配网 DNS 的设备特定启动配置；协议处理由 `dns_server` 组件负责 |
| `main/web/` | 独立维护配网页 HTML 模板、CSS、JS；编译时嵌入，响应时内联 |
| `main/fonts_aa.c` / `.h` | 生成的 1bpp 字模：`font_huge` / `font_mid` / `font_temp` / `font_small` |
| `tools/test_app.py` | 编译真实应用/按键/解析/UI/绘图代码，stub 网络及硬件 I/O |
| `tools/preview_ui.py` | 用真实 UI/驱动渲染按天、逐时页 PNG，并检查布局与像素 |
| `tools/genicons.swift` | 从固定 Weather Icons TTF 生成两套 1bpp 图标字模；字体文件不进仓库 |
| `tools/test_portal.py` | 编译真实配网页渲染函数，stub 扫描和 HTTP；检查模板、转义与发送失败 |
| `tools/genfont.py` | 用 Pillow + TTF 重新生成 `main/fonts_aa.c`，字体文件不进仓库 |

保留 ESP-IDF 的 `main/` 约定，不为分层增加空接口或改成 `src/`。新增 C 源文件需同步 `main/CMakeLists.txt` 及相关主机编译 harness。C 格式遵循 `.clang-format`（4 空格、120 列、函数左花括号换行）；不要为局部修改重排整个字模文件。

## 应用状态与数据边界

- 单一主循环负责网络和刷新；`ui.c` 只修改帧缓冲，不能自行联网或调用刷新。
- `app_t.w` 是本次请求结果，`app_t.shown` 是最近成功的完整天气快照。`weather_update()` 开始会清空输出，失败时 `ok=false`，且可能已写入部分数据；**不能依靠 `w` 保留旧预报**。`current_weather()` 在有成功数据后选 `shown`，支持失败期间切页与重画。
- `app_t.view` 默认按天；短按只切页并 `present()`，不重新取数，到点更新保持当前页。逐时条目不会随等待自动滚动，显示的是最近一次成功请求的前 10 条。
- 天气更新失败时，`present()` 用旧快照（首次失败用占位）重画当前页，再覆盖顶部重试提示和底栏状态。正文视觉保持不变，不是解析器保留旧值，也不是应用只调用一次标题绘制。
- **底栏温湿度只取本地传感器**（`sensor_reading_t`），不用天气接口温湿度冒充现场读数。目前未接入传感器驱动，入口传 `{0}`（`valid=false`），显示 `--.- °C` / `-- %`；接入后填 `temperature_c`、`humidity_percent` 并置 `valid=true`。

## 启动、重试与按键

- 初始化屏幕、网络、按键后，先画占位页并 `epd_refresh_full()` 清掉断电残留；然后 `wait_serviced(BOOT_WAIT_MS)` 等 5 秒再尝试连接。不能等天气请求成功才画首屏。
- `net_try_stored(CONNECT_TIMEOUT_MS)` 的连接等待为 25 秒。有凭据但连不上时按 5 → 10 → 20 → 40 → 60 → 60… 秒重试，并重画顶部连接失败提示；只有无凭据才进入网页配网。
- 天气请求失败采用相同阶梯，成功后清零天气重试计数，恢复 30 分钟等待。连接与天气阶段使用各自的重试计数；等待不包含请求、对时、刷屏耗时，不做每秒倒计时。
- 每次天气成功后 `net_apply_tz()`；首次天气成功才 `net_time_sync()`。时区来自数值 UTC 偏移，不是带 DST 规则的地区时区。
- 长按 3 秒：清 NVS → 配网提示页并全刷 → `net_portal()` → 重新连接。长按事件只报告一次，松开不再触发短按。
- **仅应用自己的等待**通过 `wait_serviced()` 每 20ms 轮询按键。连接、HTTP、SNTP、配网、屏幕 BUSY 等阻塞调用期间不轮询按键，不能承诺任意时刻长按都能立即响应。
- 保留 10 KB 主任务栈：HTTPS 在主任务同步运行，历史实机记录中 4 KB 会在 TLS 握手时栈溢出并反复重启。

## 天气与网络契约

- 每次 `weather_update()` 先请求 `http://ip-api.com/json/?fields=status,city,lat,lon` 获取出口 IP 位置，再请求 Open-Meteo HTTPS；无需 API key，不是 GPS 定位。
- 天气请求包含 daily 的天气码/最高最低温/最大降水概率，以及 hourly 的天气码/温度/降水概率，使用 `temperature_unit=celsius&timezone=auto&forecast_days=5&forecast_hours=21`，不请求 `current`。
- 日数组必须各恰好 5 项，逐时数组必须各恰好 21 项；任一必需数据缺失、`null`、数量错误或非法，整次更新失败。只按对象直接成员读取，不能误读 `daily_units`、`hourly_units` 或嵌套同名字段。截断或带非空白尾随内容的 JSON 不接受。
- 日日期要求合法且连续；温度要求有限且可存入 `float`，最低温 ≤ 最高温；概率在 `[0,100]`；天气码接受整数 `0..99`，UI 未映射的码仍画 `?`，不冒充晴天。
- 逐时格式为 `YYYY-MM-DDTHH:00`，首条日期须等于 `days[0].date`；相邻本地时间差允许 −1/0/+1/+2 小时以容忍 DST。起点为当前当地小时是 API 请求约定，不是与本机时钟比对后的保证。结构体仅保存小时数，不保留逐时日期。
- 响应缓冲为静态 8192 字节，达到容量边界视为失败；改数据数量或字段时同时检查响应容量、结构体、解析器和测试 URL。
- 配网热点是开放的 `weather-XXXXXX`（MAC 后三字节），网页地址 `192.168.1.1`，支持扫描或手填 SSID；凭据经明文 HTTP 保存到 NVS，不先验证连接成功。不要写入编译时凭据、提交真实密码或声称这是加密配网。
- `portal_dns` 使用 ESP-IDF captive portal 示例的 `dns_server` 组件，将配网热点的所有 A 查询指向 `WIFI_AP_DEF` 地址；HTTP 重定向辅助触发系统配网页，不保证所有系统必弹窗。真实 WiFi、DNS、网页和 TLS 行为需真机验证。

## 配网页维护

- 页面源文件在 `main/web/`：`index.html` 为配网页，`result.html` 为保存结果页，`style.css`、`app.js` 为样式与交互。通过 `main/CMakeLists.txt` 的 `EMBED_TXTFILES` 嵌入固件，无需文件系统、npm 或 CDN；改资源后需重新构建、烧录。
- `portal_web.c` 的 `send_page()` 分块发送模板并替换 `{{TOKEN}}`，CSS/JS 在响应中内联，不另开静态资源路由。结果页必须自包含：保存后热点即将关闭，不能依赖后续请求加载样式。
- 保留服务端同步扫描、刷新页面重新扫描及 `POST /save` 普通表单；字段 `ssid` / `ssid2` / `pass`，手填 `ssid2` 优先。此次提取不改变扫描阻塞、凭据保存或配网生命周期。
- 模板替换不自动转义，也不递归解析插入值。SSID 先经 `html_escape()` 再生成选项；结果页标题/消息仅传应用自己的固定文案，不直接传用户输入。新增 token 同步修改替换表及主机测试。
- 改页面、模板发送或资源嵌入后运行 `python3 tools/test_portal.py` 和 ESP-IDF 构建。主机测试检查真实渲染函数、最大热点列表、特殊字符、空替换及发送错误；不执行 JS、不验证真实 HTTP 服务、WiFi/NVS、手机配网窗口或浏览器视觉效果。

## 驱动与刷新策略

- 可见横屏 296×128，面板 RAM 为 `PANEL_W=128`、`PANEL_H=296`，帧缓冲 **4736 字节**。`EPD_W=300` 只是逻辑画布，`epd_pixel()` 裁掉 `x>=296`；新内容不能放在被裁区域。
- 引脚见 `epd.h`：SCK 9、MOSI 10、RST 11、DC 12、CS 13、BUSY 14；屏幕供电 3.3V。SPI2、4 MHz，旋转映射只在 `epd_pixel()` 做。
- `0x01` 门线 `0x27,0x01,0x00`；`0x45` Y 结尾 `0x27,0x01`。不要改成 300 行的 `0x2B` 或 4800 字节：多出的 4 行会回绕，表现为右侧像素跑到左侧。
- 全刷：复位 → 全量配置 → `0x32` 写 `ws_20_30` 前 153B，并写 6B 电压参数 → `0x24` 写 4736B → `0x22 0xC7` → `0x20`。
- 局部：复位 → 基础寄存器 → `0x3C 0x80` → `0x32` 写 `ws_partial` 前 153B → `0x37` 写 10B → `0x22 0xC0` + `0x20` 预置 → `0x24` 写 4736B → `0x22 0x0F` + `0x20`。不能仅将全刷的 `0x22` 参数改为 `0xFF`。
- `epd_refresh()` 在计数为 0 或 5 的倍数时全刷，否则局部；调用后计数加一，即第 1、6、11… 次普通刷新全刷。**状态帧、重试帧也计数，切页使用 `epd_refresh_full()` 且不推进计数**；周期取决于所有普通刷新调用，不是固定时间。局部刷新也传输完整帧缓冲，不是只传矩形区域。
- `epd_refresh_full()` 强制全刷但不推进、不重置周期计数，也不保证下一次普通刷新一定全刷。应用用于启动占位、配网页（首次配网或长按重置），以及 `present()` 中 `w->ok && !painted` 的首帧真实天气；首帧天气的全刷由 `painted` 显式保证，不依赖计数恰好为 0。
- **切页强制全刷**，因为按天页和逐时页是整屏内容替换，局部波形无法可靠清除上一页的字形和图标。切页会闪一下，但不推进普通刷新计数；主机切页场景检查全刷序列 `0xC7`，修改此策略必须同步测试并做实屏残影/闪烁验证。
- BUSY 高电平为忙，初始化等待 2s、刷新等待 15s；超时只打日志，不无限卡住。

## 排版与字模

- 按天页 5 个 52px 等宽列，`GRID_X=18`，中心 44/96/148/200/252；日期、图标、温度、概率均居中，不加行标签。
- 第 1 列 `今天` 用 `font_mid`，其余 `MM/DD` 用 `font_small`；无数据时五列都是 `--/--`。温度单行 `最低°-最高°`，上限 48px，先用 14px `font_temp`，超宽降 `font_small`，仍超宽或无效则 `--`。`12°-25°` 在 14px 下 47px，在 16px 下 53px，不能直接换成 `font_mid`。
- 按小时页 **10 列 × 28px**，`HOUR_GRID_X=8`，中心从 22 起每次加 28；索引 `hours[i*HOUR_STEP]`，`HOUR_STEP=1`，覆盖请求时当前小时至 +9 小时，标题为 `未来10小时`。
- 小时用两位数字和 `font_small`，无数据 `--`；Weather Icons 的 `font_icon` 固定 advance 26、line height 23，基线 69，墨迹限制在 y=46..68 和当前 28px 列内。温度带 `°`、宽度上限 **26px**，`font_temp` → `font_small` → `--`；概率用 `font_small`，超宽时去掉 `%`（如 `100%` → `100`），未知仍为 `--%`。
- 两页共用 `DATE_BASE=42`、`ICON_TOP=46`、`TEMP_BASE=89`、`PRECIP_BASE=103`；按天 Weather Icons 的 `font_icon_daily` 固定 advance 40、line height 27，基线 73，墨迹区为 y=46..72；逐时图标基线 69，墨迹区为 y=46..68。改行锚点或字模 cell 时同步核对两个页面。
- 按天页顶部建议只看 `days[0]`：无预报或概率非法先显示 `预报待更新`；其余雷雨/冰雹优先 `雷雨注意安全`，雨雪类 `今日带伞`，再看四舍五入后的概率 **≥50%** 则 `今日带伞`；已知晴好/雾类低概率显示 `降水概率低`，未知码低概率显示 `预报待更新`。阈值和百分比显示用同一舍入结果，日最大概率不代表正在下雨。
- 底栏基线 124：温度标签 x=8、读数 x=46（上限 58px）；湿度标签 x=110、读数 x=146（上限 50px）；全部用 `font_mid`。非有限值、越界湿度或超宽读数用占位。
- 状态角落 x≥200、y≥110，文字右对齐到 x=290，上限 90px。五种可见文案为 `正在联网` / `正在取数` / `同步时间` / `已更新` / `重试 #n`，另有空状态 `UI_STATUS_NONE`。`set_status(..., true)` 在取数、首次对时前单独刷状态帧，该帧的帧缓冲改动只能在此角落内。
- 文字字模为 1bpp：46/16/14/12px；**中文只在 `font_mid` 中**，度符号在所有字号中。天气图标来自 `main/font_icons.c` 的 Weather Icons 1bpp 子集，未知 WMO 码仍显示 `?`；不要在 `ui.c` 恢复手绘图标。新增屏幕文案后用 `tools/genfont.py` 重新生成并跑缺字检查；扫描范围为 `main.c`、`ui.c`、`weather.c` 的字符串，不含网页文案。
- 文字字模生成额外依赖 Pillow、Roboto 与 Noto Sans SC TTF，路径可用 `FONT_TTF` / `FONT_CJK_TTF` 指定；天气图标字模生成需要 macOS Swift 的 CoreGraphics/CoreText/CryptoKit/ImageIO，以及 `.cache/weather-icons/` 中固定 SHA256 的 Weather Icons TTF。普通构建和测试直接使用已生成的 `fonts_aa.c` / `font_icons.c`，不需要下载字体。

## 回归要求与覆盖边界

- 改驱动、布局或字模后至少运行 `python3 tools/test_app.py` 和 `python3 tools/preview_ui.py`。改 Weather Icons 映射、生成器或 `font_icons.c` 后还要核对 `third_party/weather-icons/README.md` 的许可/来源与预览。测试通过不代表实屏黑度、残影、刷新时序或网络可用性已验证。
- `preview_ui.py` 覆盖按天页并生成逐时页预览；逐时 UI 在 `test_app.py` 中检查，切页由 `expected_view`、`button_toggle`、`button_retry` 等应用逻辑覆盖；按键另有去抖和 3 秒长按阈值测试。
- 改布局同步核对：`ui.c` 的日/小时列宽、网格起点和行锚点；`preview_ui.py` 的 `COL_W` / `GRID_X` / `DATE_BASE` / `ICON_TOP` / `DAILY_TEMP_BASE` / `PRECIP_BASE`、`icon_mask` / `expect_forecast`；`test_app.py` 的 `centers` / `boundaries` / `hour_centers`、图标字形、宽度及基线断言。不要只改断言以掩盖越界。
- 主机解析测试广泛覆盖日数据、作用域、日期、非法值、缺项及截断 JSON；逐时有缺失/短数组/首日错位等用例，但 `validate_weather()` 尚未逐字段比较成功响应的 `hours`。改逐时解析时补对应断言，不假设现有用例已穷尽 DST 和数值边界。
- 应用测试检查完整 SPI 帧、启动等待、退避、失败正文保留、状态角落隔离、首次对时和切页；网络为 stub，**不覆盖真实 `net.c` / `net_portal.c` / `portal_web.c` / `portal_dns.c` 的 NVS、WiFi、HTTP/TLS、DNS、网页、SNTP 或硬件阻塞时序**。
- `--baseline` 接受旧 `main/` 目录，用于比较请求、调用及完整帧；只有数据契约、布局和字模兼容的基线才有意义。`.cache/` 中历史快照不是运行依赖，不能直接套用旧的单日/旧切页结论。
- `ABLATION.md` 的体积、帧像素与实机观察属于当时版本的实验，不是当前布局的测量；不要将旧数字或未经复测的残影结论写成当前保证。
