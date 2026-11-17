# AGENTS.md

## 项目速览

ESP32-S3 + 2.9" SSD1680 墨水屏天气面板，ESP-IDF/C。源码 `main/`，主机回归 `tools/`。两个页面：按天（5 列）和按小时（10 列，每小时一列），按键切换。接口获取 5 天 + 21 条逐时数据；逐时页只显示前 10 条，不要把请求条数和显示列数混用。

产品同时要支持**小智 AI 语音对话**：语音问答时间、天气和温湿度，墨水屏作为常显与语音状态的反馈面。语音对话仅由 WakeNet 检测「小鹿小鹿」触发。协议层用乐鑫的 `espressif/esp_xiaozhi` 组件，服务信息地址（OTA）由 SETUP-WIFI 页面逐台填写，固件没有内置默认服务器。接线和运行前置条件见「语音对话（小智 AI）」。时间、天气和本地温湿度 MCP 工具已接入，屏上激活码与对话状态仍待接入。

本文描述当前实现；修改行为时同步更新相关测试与说明。`README.md`、`ABLATION.md` 中部分内容是历史记录，尤其是“不保留旧天气快照”和“切页强制全刷”，不能直接当作当前约束。

## 通用能力复用原则

- 通用能力优先使用 ESP-IDF 内置组件或 Component Manager 注册表中的成熟组件，不重复实现已有的解析器、编码器、协议处理、数据结构或其他基础设施；例如 JSON 使用自动管理的 `espressif/cjson`，HTTP(S)、DNS、TLS 和网络协议优先使用 ESP-IDF API。
- 使用 Component Manager 组件时，在 `idf_component.yml` 中声明直接依赖并用 `dependencies.lock` 锁定版本；由组件管理器解析的传递依赖也应保持自动下载，不要把同一份第三方源码复制到 `components/`，也不要为了适配组件名额外维护重复包装层。`managed_components/` 是生成的下载目录，不提交到仓库。
- 不要自行实现通用 DNS 协议、报文编解码、查询解析或网络基础设施；优先复用 ESP-IDF/lwIP、Component Manager 组件或成熟库。`portal_dns` 只有在现有 API 无法覆盖配网热点的设备特定强制解析行为时，才允许保留最小适配代码。
- 自己实现的代码只负责业务规则、设备特定适配和库无法覆盖的边界；天气字段范围、日期连续性、刷新策略等业务校验仍由项目维护。若自动管理的组件缺少项目需要的严格校验，应在业务层补充最小必要规则，不修改下载目录中的第三方源码。
- 准备新增通用工具前，先检查 ESP-IDF 内置组件、Component Manager 注册表和现有依赖。只有在没有合适组件，或组件的资源、许可证、平台兼容性不满足要求时，才在 `components/` 中实现最小必要本地组件；使用修改版第三方代码时记录上游版本、修改原因、许可证和测试范围。
- 新增或调整依赖时同步更新 `idf_component.yml`、`dependencies.lock`、`CMakeLists.txt`、主机回归 harness、许可证和相关说明；固件与主机测试应尽量共用组件管理器下载的同一份源码。当前 `components/esp_xiaozhi/` 是基于上游 0.1.4 的单份本地修改版，Component Manager 使用 `override_path`；不要同时保留或修改 `managed_components/espressif__esp_xiaozhi/`。

## 常用命令

在项目根目录运行；主机回归和预览需要 Python 标准库及本机 C 编译器。运行天气/应用回归前，项目必须已由 ESP-IDF Component Manager 下载依赖（执行 `idf.py reconfigure` 或 `idf.py build`）。

```bash
python3 tools/test_app.py                 # 应用、解析、按键、UI 全量回归
python3 tools/test_portal.py              # 配网页模板、转义及 HTTP 分块发送回归
python3 tools/test_device_mcp.py          # 设备工具、快照、不可用状态、分配失败与并发回归
python3 tools/test_voice_lifecycle.py     # 抽取真实状态函数，检查停止失败重试、挂起唤醒与重配网恢复
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
    PATH="/Users/sharp/.espressif/tools/python/v6.0.2/venv/bin:$PATH" \
    /Users/sharp/.espressif/tools/python/v6.0.2/venv/bin/python \
    /Users/sharp/.espressif/v6.0.2/esp-idf/tools/idf.py build
```

- 不要用 `bash -lc '. .../export.sh'`：登录 shell 可能先找到系统 Python 3.9，导致 IDF 拒绝激活。
- 烧录前确认实际串口，再将末尾的 `build` 改为 `-p 实际串口 flash`；不要把历史设备路径当作固定值。`idf.py monitor` 在交互终端运行。
- 目标和默认配置见 `sdkconfig.defaults`：ESP32-S3、8 MB Flash、USB Serial/JTAG、主任务栈 10240 字节。已有 `sdkconfig` 时修改 defaults 不一定改变当前配置，需核对两者。

## 字体与字模生成

仓库提交的是生成后的 `main/fonts_aa.c`、`main/font_clock.c` 和 `main/font_icons.c`，固件构建不需要字体文件、Pillow、Swift、联网或 `.cache/`。只有修改屏幕文案、字号、文字字体、Weather Icons 映射或字形布局时才需要重新生成；生成后必须运行字模/布局回归。不要把下载的 TTF、CSS 或预览 PNG 提交到仓库。

### 文字字模：`fonts_aa.c`

`tools/genfont.py` 使用 Pillow 的 FreeType `mode='1'` 单色栅格器生成 1bpp、MSB-first、逐行字节补齐的 C 数组，输出为 `main/fonts_aa.c` 和独立时钟字模 `main/font_clock.c`；`--only clock` 只生成后者。输入字体只在生成时使用：

- 拉丁字母、数字和 `°` 使用 Google Fonts 的 Roboto Variable。原有 `fonts_aa.c` 的来源说明为 Apache License 2.0；新增时钟使用 Roboto 3.015（SIL OFL 1.1），固定提交、SHA256 与许可见 `third_party/roboto-clock/`。
- 中文使用 Google Fonts 的 Noto Sans SC Variable，SIL Open Font License 1.1；只有 `font_mid`（16px）带中文。
- 每种字号始终包含 ASCII `U+0020..U+007F` 和度符号 `U+00B0`；中文字符由 `main/main.c`、`main/ui.c`、`main/weather.c` 中的 C 字符串自动扫描，网页 `portal_web.c` 的文案不进入字模。
- 当前输出为 `font_huge` 46px Bold、`font_mid` 16px Medium、`font_temp` 14px Medium、`font_small` 12px Regular。右上角时钟另由 `tools/genfont.py --only clock` 生成 `font_clock`，为 12px Bold、仅拉丁字符；字号、字重、是否带中文在生成器中维护。

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

路径含 `[`、`]` 时必须加引号；脚本会从项目根目录读取上述 `main/*.c`，所以命令也必须在项目根目录运行。若只改了屏幕文案，通常无需重新下载字体，保留原 TTF 后直接重新运行最后三行即可。生成后检查对应 `.c` 的头部和缺字情况。

若只改右上角时钟字体，可只生成独立字模，不需要 CJK 字体：

```bash
FONT_TTF='.cache/fonts/Roboto[wdth,wght].ttf' \
python3 tools/genfont.py --only clock
```

生成后运行：

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
| `managed_components/espressif__cjson/` | Component Manager 自动下载的 cJSON 源码，固件和主机回归共用 |
| `main/main.c` | 启动、配网、重试、30 分钟更新、当前页与成功天气快照、刷新调度 |
| `main/button.c` / `.h` | GPIO4 低电平按键，内部上拉；2 次采样去抖，短按切页，长按 3s 重置 WiFi |
| `main/epd.c` / `.h` | SSD1680 SPI 驱动、4736 字节帧缓冲、绘图原语、全刷/局部刷新 |
| `main/ui.c` / `.h` | 五天页、10 列逐时页、传感器底栏、状态、重试与配网提示；只画帧缓冲 |
| `main/font_icons.c` / `.h` | Weather Icons 的 11 个 1bpp 图标子集；日页与逐时页使用两套固定尺寸字模 |
| `third_party/weather-icons/` | Weather Icons 来源、固定提交、SHA256 和 SIL OFL 1.1 许可说明 |
| `main/weather.c` / `.h` | 出口 IP 定位、Open-Meteo 请求、cJSON 数据校验、5 天 + 21 小时数据契约 |
- `main/weather_json.c` / `.h` | 使用 Component Manager 提供的 cJSON 完成严格整段解析、数值/整数/定长字符串/对象成员数组读取；不含天气业务校验
| `main/net.c` / `.h` | NVS 凭据、STA 连接、SNTP/时区、HTTP(S) GET |
| `main/audio.c` / `.h` | ES8311 + NS415B 的 I²C/I²S 初始化与 `audio_read`/`audio_write`/`audio_set_mute` 原语；上电静音 |
| `main/voice.c` / `.h` | WakeNet 本地唤醒、`esp_xiaozhi` 会话、Opus 编解码；采集与会话分别运行在独立任务 |
| `main/voice_settings.c` / `.h` | 小智官方/自建模式、信息地址和可选 WebSocket 地址的 NVS 保存及 URL 校验 |
| `main/voice_audio.c` / `.h` | 纯函数：交错立体声→左声道单声道、单声道→立体声；不含 SDK 依赖，主机可测 |
| `main/device_mcp.c` / `.h` | 发布屏幕同源的成功天气/传感器快照；用 managed MCP SDK 注册当前时间、5天+21小时预报、本地温湿度三个只读工具 |
| `main/net_portal.c` | SoftAP 配网生命周期：AP/IP、WiFi 模式、DNS/网页服务启停 |
| `main/portal_web.c` | 网页配网：热点扫描、模板渲染、WiFi 与小智服务设置 `/save` 保存与凭据事件 |
| `components/dns_server/` | 复用 ESP-IDF captive portal 示例的 DNS server 组件 |
| `main/portal_dns.c` / `.h` | 配网 DNS 的设备特定启动配置；协议处理由 `dns_server` 组件负责 |
| `main/web/` | 独立维护配网页 HTML 模板、CSS、JS；编译时嵌入，响应时内联 |
| `main/fonts_aa.c` / `.h` | 生成的 1bpp 字模：`font_huge` / `font_mid` / `font_temp` / `font_small`；右上角时间使用独立的 `main/font_clock.c` / `font_clock` 粗体字模 |
| `tools/test_app.py` | 编译真实应用/按键/解析/UI/绘图代码，stub 网络及硬件 I/O |
| `tools/preview_ui.py` | 用真实 UI/驱动渲染按天、逐时页 PNG，并检查布局与像素 |
| `tools/genicons.swift` | 从固定 Weather Icons TTF 生成两套 1bpp 图标字模；字体文件不进仓库 |
| `tools/test_portal.py` | 编译真实配网页渲染函数，stub 扫描和 HTTP；检查模板、转义与发送失败 |
| `tools/test_voice_settings.py` | 编译真实语音设置/URL 校验，使用 ESP-IDF `http_parser` 和 stub NVS 回归 |
| `tools/genfont.py` | 用 Pillow + TTF 生成文字字模；`--only clock` 只生成 `main/font_clock.c`，字体文件不进仓库 |

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

## 语音对话（小智 AI）

- 协议用 `espressif/esp_xiaozhi`（上游 0.1.4 的本地 override，依赖 `esp_websocket_client`、`mcp-c-sdk`）连接官方或自建 Xiaozhi 服务；示例 `ai/xiaozhi_chat`，API 文档见 <https://docs.espressif.com/projects/esp-iot-solution/zh_CN/latest/ai/xiaozhi.html>。本地修改包含运行时 URL 的 info 请求入口、有界 WebSocket 发送、MCP 发送错误传播与安全 stop/deinit；**不要在本项目重新实现 WebSocket、MQTT+UDP、hello/认证或 MCP**。来源、Apache-2.0 许可证和修改范围见 `components/esp_xiaozhi/LOCAL_PATCH.md`。
- 典型顺序：`esp_xiaozhi_chat_init`（传音频/事件回调）→ `esp_xiaozhi_chat_get_info`（POST 板卡信息拿激活码，把 MQTT/WS 配置写 NVS）→ `chat_start` → `chat_open_audio_channel`（不发 message 时自动生成 hello）→ `chat_send_audio_data` 推二进制音频帧 → `chat_stop` / `chat_deinit`。事件回调只报协议事实（TTS 起停、识别文本、emoji、system cmd），设备状态机、UI、执行命令由应用决定。
- 编解码用 `esphome/micro-opus`。本地检测使用 `espressif/esp-sr` 2.4.7 的 `wn9_xiaoluxiaolu_tts2`（「小鹿小鹿」）。构建把模型打包到 `model` 分区，首次烧录要包含模型分区。依赖和许可由 ESP-IDF 组件管理器维护，见 ESP-SR 组件的 `LICENSE`。
- `voice_init()` 加载模型并创建音频任务。联网后获取小智配置并初始化会话，首次激活码输出到串口。唤醒前仅在本地检测。命中后采集任务缓存单声道问句，供会话任务打开音频通道后用 60 ms Opus 帧上传。收到 TTS 开始或到达录音时限后停止上传；会话任务从队列解码播放，播放完成后恢复检测。
- 音频契约：hello 默认 `opus` / 16000 Hz / 1 声道 / 60 ms 帧。`main/audio.c` 现状是 16 kHz、16 bit、**双声道**（左声道为麦克风），需要在音频任务里做单声道转换和 60 ms 分帧；不要把双声道数据直接当单声道推给服务端。
- 音频接线以 `main/audio.h` 顶部 `#define` 为真值，改线只改那一处；下表供阅读，不是第二份配置：

| 信号 | GPIO | 说明 |
| --- | --- | --- |
| NS415B EN（功放） | 2 | 高电平使能，由 ES8311 驱动 `pa_pin` 控制 |
| I²S DOUT | 47 | ESP32 → ES8311 DIN/SDIN |
| I²S WS | 48 | LRCK / WS |
| I²S DIN | 38 | ES8311 DOUT → ESP32（麦克风输入） |
| I²S BCLK | 39 | BCLK |
| I²S MCLK | 40 | 按接线名为 MCN |
| I²C SCL | 41 | ES8311 控制时钟 |
| I²C SDA | 42 | ES8311 控制数据（接线名为 SCA） |

- 音频参数：`I2S_NUM_0` 全双工 + `I2C_NUM_0` 400 kHz，16 kHz / 16 bit / **双声道交错**，`MCLK_MULTIPLE_384`（ES8311 支持的整数时钟关系）。默认音量 60（0..100）、麦克风增益 18 dB；`audio_init()` 后 DAC 静音且**功放使能脚 PA_EN 拉低**（`esp_codec_dev_open()` 会短暂拉高，初始化末尾显式关掉；该脚可能同时接板载 LED），要播放先 `audio_set_mute(false)`（同时打开 DAC 与 PA）。注意 `esp_codec_dev` 的音量是线性 dB 映射（0=−50 dB、100=0 dB），低音量值实际非常轻，做通路自检时不要用 `volume 30` 级别的设置。
- 当前板卡 ESP32-S3-WROOM-1-N16R8 使用 8 MB Octal PSRAM，占用 GPIO33–37。音频已迁移为功放 EN=`GPIO2`、I²S DOUT=`GPIO47`、WS=`GPIO48`，其余 I²S/I²C 保持 GPIO38–42；GPIO47/48 在模块引脚区域相邻，适合缩短 PCB 走线。GPIO43/44 留作外接 UART，GPIO19/20 保留 USB-JTAG。`sdkconfig.defaults` 已启用 `CONFIG_SPIRAM`、`CONFIG_SPIRAM_MODE_OCT` 和 80 MHz PSRAM；刷板前必须核对 PCB 实际连线。固件在没有可用 PSRAM 时跳过唤醒初始化。
- 栈与任务：语音跑独立任务，不要挤主任务（10 KB 栈已只为同步 HTTPS 留出）；若 keystore 的 NVS 操作在 PSRAM 栈上出问题，用 `esp_xiaozhi_nvs_ops_register` 把 NVS 转到 SRAM 任务。
- 隐私边界：接上后麦克风音频和识别结果会上传到 xiaozhi.me，属于联网上传，不可写成“本地处理”。只在唤醒或明确触发后开麦，空闲时保持 `audio_set_mute` 静音、不发送音频。
- 与天气的关系：`device_mcp_publish()` 发布已有的 `app->shown` 快照和 `sensor_reading_t`，由 managed SDK 提供 `self.get_current_time`、`self.get_weather_forecast`、`self.get_sensor_reading` 三个无参数只读工具。不要为一次问答重跑 `weather_update()`，也不要绕过“失败保留旧快照”的语义。天气包含5天+21小时、抓取时间、单调缓存年龄、陈旧与重试状态；未对时抓取时间为null。传感器未接入时必须available=false，不能冒用预报。当前时间仅在SNTP成功、时钟合理且天气UTC偏移已知时synchronized=true。配套Rust服务端用官方rmcp发现与调用工具，云模型须支持tool_calls。
- 等待预算：设备首音频等待90秒、播放中断流等待30秒；配套Rust文本回合正常首音频65秒、含失败提示75秒、总回合180秒（不包含ASR）。不要只改一端的超时。WS stop失败时保留chat/MCP/Opus，释放应用网络锁后由voice任务重试清理，禁止新会话；成功stop后deinit消费句柄。
- 屏上反馈：语音状态（连接、聆听、说话、失败）沿用现有状态角落与文案机制，且**新增中文文案必须重跑 `tools/genfont.py`**（中文只在 `font_mid`）。**M1 尚未加语音文案和激活码页**，配对码只能从串口看；补屏上反馈时要一次把状态词和配对页做完，别分两次重生成字模。
- 按键语义：短按切页，长按 3 秒重配网。
- 音频回调把下行包送入有界队列。采集任务和播放任务共用 ES8311，播放期间暂停唤醒检测和录音。

## 配网页维护

- 配网热点是开放的 `weather-XXXXXX`（MAC 后三字节），网页地址 `192.168.1.1`，支持扫描或手填 SSID；凭据经明文 HTTP 保存到 NVS，不先验证连接成功。不要写入编译时凭据、提交真实密码或声称这是加密配网。
- `portal_dns` 使用 ESP-IDF captive portal 示例的 `dns_server` 组件，将配网热点的所有 A 查询指向 `WIFI_AP_DEF` 地址；HTTP 重定向辅助触发系统配网页，不保证所有系统必弹窗。真实 WiFi、DNS、网页和 TLS 行为需真机验证。
- 页面源文件在 `main/web/`：`index.html` 为配网页，`result.html` 为保存结果页，`style.css`、`app.js` 为样式与交互。通过 `main/CMakeLists.txt` 的 `EMBED_TXTFILES` 嵌入固件，无需文件系统、npm 或 CDN；改资源后需重新构建、烧录。
- `portal_web.c` 的 `send_page()` 分块发送模板并替换 `{{TOKEN}}`，CSS/JS 在响应中内联，不另开静态资源路由。结果页必须自包含：保存后热点即将关闭，不能依赖后续请求加载样式。
- 保留服务端同步扫描、刷新页面重新扫描及 `POST /save` 普通表单；WiFi 字段为 `ssid` / `ssid2` / `pass`，手填 `ssid2` 优先；小智字段为 `voice_url`（必填） / `voice_ws`（可选）。服务设置与 WiFi 凭据在同一 `wifi` NVS 事务中保存，长按重配网只清 WiFi 凭据并保留服务设置。
- 模板替换不自动转义，也不递归解析插入值。SSID 先经 `html_escape()` 再生成选项；结果页标题/消息仅传应用自己的固定文案，不直接传用户输入。新增 token 同步修改替换表及主机测试。
- 改页面、模板发送或资源嵌入后运行 `python3 tools/test_portal.py` 和 ESP-IDF 构建。主机测试检查真实渲染函数、最大热点列表、特殊字符、空替换及发送错误；不执行 JS、不验证真实 HTTP 服务、WiFi/NVS、手机配网窗口或浏览器视觉效果。

## 驱动与刷新策略

- 可见横屏 296×128，面板 RAM 为 `PANEL_W=128`、`PANEL_H=296`，帧缓冲 **4736 字节**。`EPD_W=300` 只是逻辑画布，`epd_pixel()` 裁掉 `x>=296`；新内容不能放在被裁区域。
- 引脚见 `epd.h`：SCK 9、MOSI 10、RST 11、DC 12、CS 13、BUSY 14；屏幕供电 3.3V。SPI2、4 MHz，旋转映射只在 `epd_pixel()` 做。
- `0x01` 门线 `0x27,0x01,0x00`；`0x45` Y 结尾 `0x27,0x01`。不要改成 300 行的 `0x2B` 或 4800 字节：多出的 4 行会回绕，表现为右侧像素跑到左侧。
- 全刷：复位 → 全量配置 → `0x32` 写 `ws_20_30` 前 153B，并写 6B 电压参数 → `0x24` 写 4736B → `0x22 0xC7` → `0x20`。
- 局部：复位 → 基础寄存器 → `0x3C 0x80` → `0x32` 写 `ws_partial` 前 153B → `0x37` 写 10B → `0x22 0xC0` + `0x20` 预置 → `0x24` 写 4736B → `0x22 0x0F` + `0x20`。不能仅将全刷的 `0x22` 参数改为 `0xFF`。
- `epd_refresh()` 在启动占位全刷之后从局刷开始；普通刷新计数每达到 `FULL_REFRESH_EVERY` 的倍数时做一次维护性全刷，其余使用局部波形。**状态帧、重试帧和天气更新走普通刷新接口**，计数取决于这些普通刷新调用；切页单独计数，不推进普通刷新计数。局部刷新仍传输完整帧缓冲，不是只传矩形区域。
- **切页刷新规则：累计第 1–7 次切页只使用局部刷新；第 8 次切页执行一次全刷并清零切页计数。未满 8 次不得因切页主动全刷。** 第 8 次的全刷同时清理多次局刷积累的残影；若普通维护全刷已经到期，应在这次全刷中一并完成，避免紧接着再闪一次。普通维护全刷或 `epd_refresh_full()` 执行后也会清零切页计数。
- `epd_refresh_full()` 强制全刷但不消耗普通刷新计数，应用只用于启动占位、首次进入配网或长按重置配网等需要清理断电残影/整页状态的场景。第 8 次切页由切页刷新接口执行一次全刷；若长期局刷导致残影，切页全刷和驱动的周期性维护全刷负责恢复对比度。
- BUSY 高电平为忙，初始化等待 2s、刷新等待 15s；超时只打日志，不无限卡住。

## 排版与字模

- 按天页 5 个 52px 等宽列，`GRID_X=18`，中心 44/96/148/200/252；日期、图标、温度、概率均居中，不加行标签。
- 第 1 列 `今天` 用 `font_mid`，其余 `MM/DD` 用 `font_small`；无数据时五列都是 `--/--`。温度单行 `最低°-最高°`，上限 48px，先用 14px `font_temp`，超宽降 `font_small`，仍超宽或无效则 `--`。`12°-25°` 在 14px 下 47px，在 16px 下 53px，不能直接换成 `font_mid`。
- 按小时页 **10 列 × 28px**，`HOUR_GRID_X=8`，中心从 22 起每次加 28；索引 `hours[i*HOUR_STEP]`，`HOUR_STEP=1`，覆盖请求时当前小时至 +9 小时，标题为 `未来10小时`。
- 小时用两位数字和 `font_small`，无数据 `--`；Weather Icons 的 `font_icon` 固定 advance 26、line height 23，基线 69，墨迹限制在 y=46..68 和当前 28px 列内。温度带 `°`、宽度上限 **26px**，`font_temp` → `font_small` → `--`；概率用 `font_small`，超宽时去掉 `%`（如 `100%` → `100`），未知仍为 `--%`。
- 两页共用 `DATE_BASE=42`、`ICON_TOP=46`、`TEMP_BASE=89`、`PRECIP_BASE=103`；按天 Weather Icons 的 `font_icon_daily` 固定 advance 40、line height 27，基线 73，墨迹区为 y=46..72；逐时图标基线 69，墨迹区为 y=46..68。改行锚点或字模 cell 时同步核对两个页面。
- 按天页顶部建议只看 `days[0]`：无预报或概率非法先显示 `预报待更新`；雷雨/冰雹优先 `雷雨注意安全`；其余天气只有四舍五入后的概率 **≥51%** 才显示 `今日带伞`；已知天气低于阈值显示 `降水概率低`，未知码低概率显示 `预报待更新`。阈值和百分比显示用同一舍入结果，日最大概率不代表正在下雨。
- 底栏基线 124：温度标签 x=8、读数 x=46（上限 58px）；湿度标签 x=110、读数 x=146（上限 50px）；全部用 `font_mid`。非有限值、越界湿度或超宽读数用占位。
- 状态角落 x≥200、y≥110，文字右对齐到 x=290，上限 90px。五种可见文案为 `正在联网` / `正在取数` / `同步时间` / `已更新` / `重试 #n`，另有空状态 `UI_STATUS_NONE`。`set_status(..., true)` 在取数、首次对时前单独刷状态帧，该帧的帧缓冲改动只能在此角落内。
- 右上角 `HH:MM` 使用 12px Bold `font_clock`，基线 16、右对齐到 x=292；日期、概率和逐时标签仍用原来的 `font_small`。当前没有独立的每分钟时钟刷新，时间在页面重绘时更新；仅更改字重不改变应用刷新频率。
- 文字字模为 1bpp：46/16/14/12px；**中文只在 `font_mid` 中**，度符号在所有字号中。天气图标来自 `main/font_icons.c` 的 Weather Icons 1bpp 子集，未知 WMO 码仍显示 `?`；不要在 `ui.c` 恢复手绘图标。新增屏幕文案后用 `tools/genfont.py` 重新生成并跑缺字检查；扫描范围为 `main.c`、`ui.c`、`weather.c` 的字符串，不含网页文案。
- 文字字模生成额外依赖 Pillow、Roboto 与 Noto Sans SC TTF，路径可用 `FONT_TTF` / `FONT_CJK_TTF` 指定；天气图标字模生成需要 macOS Swift 的 CoreGraphics/CoreText/CryptoKit/ImageIO，以及 `.cache/weather-icons/` 中固定 SHA256 的 Weather Icons TTF。普通构建和测试直接使用已生成的 `fonts_aa.c` / `font_clock.c` / `font_icons.c`，不需要下载字体。

## 回归要求与覆盖边界

- 改驱动、布局或字模后至少运行 `python3 tools/test_app.py` 和 `python3 tools/preview_ui.py`。改 Weather Icons 映射、生成器或 `font_icons.c` 后还要核对 `third_party/weather-icons/README.md` 的许可/来源与预览。测试通过不代表实屏黑度、残影、刷新时序或网络可用性已验证。
- `preview_ui.py` 覆盖按天页并生成逐时页预览；逐时 UI 在 `test_app.py` 中检查，切页由 `expected_view`、`button_toggle`、`button_switch_full`、`button_retry` 等应用逻辑覆盖；按键另有去抖和 3 秒长按阈值测试。
- 改布局同步核对：`ui.c` 的日/小时列宽、网格起点和行锚点；`preview_ui.py` 的 `COL_W` / `GRID_X` / `DATE_BASE` / `ICON_TOP` / `DAILY_TEMP_BASE` / `PRECIP_BASE`、`icon_mask` / `expect_forecast`；`test_app.py` 的 `centers` / `boundaries` / `hour_centers`、图标字形、宽度及基线断言。不要只改断言以掩盖越界。
- 主机解析测试广泛覆盖日数据、作用域、日期、非法值、缺项及截断 JSON；逐时有缺失/短数组/首日错位等用例，但 `validate_weather()` 尚未逐字段比较成功响应的 `hours`。改逐时解析时补对应断言，不假设现有用例已穷尽 DST 和数值边界。
- 应用测试检查完整 SPI 帧、启动等待、退避、失败正文保留、状态角落隔离、首次对时和切页；网络为 stub，**不覆盖真实 `net.c` / `net_portal.c` / `portal_web.c` / `portal_dns.c` 的 NVS、WiFi、HTTP/TLS、DNS、网页、SNTP 或硬件阻塞时序**。
- 语音链路大部分不在主机覆盖内：`esp_xiaozhi` 的传输、激活、Opus 编解码、唤醒和真实 I²S 回流只能真机验证。主机覆盖 `voice_audio.c` 的声道转换、`button.c` 的短按/3 秒重配网动作；`test_device_mcp.py` 编译真实工具及managed cJSON，使用SDK公开头和stub注册API/时钟/RTOS，覆盖数据、失败快照保留、分配失败、并发，不验证MCP协议或真实SNTP。`test_voice_lifecycle.py` 抽取未经改写的网络状态/会话清理/voice主循环函数，stub RTOS与协议调用，覆盖停止失败资源保留、挂起唤醒、重配网后重新ARMED，不覆盖整个voice.c或真实SDK停止时序。`main.c` 里的 `voice_*` 和发布调用在 harness 中是 stub；发布断言检查与UI同源。改 `voice.c` 的时序或分帧仍需真机听感验证。
- `partitions.csv` 分别为固件和 ESP-SR 模型分配分区。调整模型或依赖后，用构建输出核对 app 和模型镜像各自的大小。
- `--baseline` 接受旧 `main/` 目录，用于比较请求、调用及完整帧；只有数据契约、布局和字模兼容的基线才有意义。`.cache/` 中历史快照不是运行依赖，不能直接套用旧的单日/旧切页结论。
- `ABLATION.md` 的体积、帧像素与实机观察属于当时版本的实验，不是当前布局的测量；不要将旧数字或未经复测的残影结论写成当前保证。
