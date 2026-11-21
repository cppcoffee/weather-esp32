# AGENTS.md

面向改代码的 AI：这里只写“改什么、改哪里、怎么验、不能违反什么”。用户文档在 `README.md`，进度与真机验收在 `TODO.md`，取舍历史在 `ABLATION.md` / `IMPROVEMENTS.md`（历史结论不等于当前约束）。

ESP32-S3 + 2.9" SSD1680 墨水屏天气面板（ESP-IDF/C，源码 `main/`，主机回归 `tools/`）。按天 5 列 + 逐时 10 列两页，按键切换；外加小智 AI 语音（WakeNet「小鹿小鹿」→ Opus 上传 → TTS 播放）。

## 构建与测试

```bash
# 本机 ESP-IDF v6.0.2（已激活环境时直接 idf.py build）
env IDF_PATH=$HOME/.espressif/v6.0.2/esp-idf \
    IDF_PYTHON_ENV_PATH=$HOME/.espressif/tools/python/v6.0.2/venv \
    ESP_IDF_VERSION=6.0.2 \
    PATH="$HOME/.espressif/tools/python/v6.0.2/venv/bin:$PATH" \
    $HOME/.espressif/tools/python/v6.0.2/venv/bin/python \
    $HOME/.espressif/v6.0.2/esp-idf/tools/idf.py build
```

```bash
python3 tools/test_app.py                  # 应用/解析/按键/UI（--only weather|ui|app）
python3 tools/test_clock.py --sanitize     # 时钟、阻塞取数、跨午夜、显示互斥
python3 tools/test_device_mcp.py           # 设备工具、音量、快照、并发
python3 tools/test_audio_volume.py         # 音量恢复/NVS 故障回滚
python3 tools/test_voice_feedback.py --sanitize  # 激活码、状态快照、状态绘图
python3 tools/test_voice_lifecycle.py      # 会话清理、断网恢复、重配网
python3 tools/test_voice_session.py --sanitize   # 录音、播放、追问、打断
python3 tools/test_voice_settings.py       # 语音设置与 URL 校验
python3 tools/preview_ui.py                # 布局预览 PNG（演示数据，非实时天气）
```

- 天气/应用回归会编译 `managed_components/espressif__cjson`，首次先 `idf.py build` 或 `reconfigure`。
- 不要用 `bash -lc '. .../export.sh'`（登录 shell 可能先找到系统 Python 3.9，IDF 拒绝激活）。
- `idf.py monitor` 需交互终端；脚本读串口会复位芯片。烧录用实际串口，路径不写死。
- 目标配置以 `sdkconfig.defaults` 为准（ESP32-S3、16 MB Flash、Octal PSRAM、主任务栈 10240）；已有 `sdkconfig` 时不随 defaults 自动更新。

## 模块

| 文件 | 职责 |
| --- | --- |
| `main/main.c` | 启动、配网、重试阶梯、30 分钟刷新、当前页/成功快照、刷新调度、`clock` 任务 |
| `main/ui.c` / `.h` | 五天页、逐时页、传感器底栏、状态/重试/配网提示；只改帧缓冲 |
| `main/epd.c` / `.h` | SSD1680 SPI 驱动、4736 B 帧缓冲、绘图原语、全刷/局刷 |
| `main/weather.c` / `.h` | IP 定位、Open-Meteo、5 天 + 21 小时数据契约校验 |
| `main/weather_json.c` / `.h` | cJSON 严格解析辅助（整段、有限数、定长串、数组成员） |
| `main/net.c` / `.h` | NVS 凭据、STA、SNTP/时区、HTTP(S) GET |
| `main/net_portal.c`、`main/portal_web.c`、`main/web/`、`components/dns_server/` | SoftAP 配网、网页渲染、DNS 劫持（不自行实现 DNS 协议） |
| `main/audio.c` / `.h` | ES8311 + NS415B：I²S/I²C 初始化、`audio_read/write`、增益/音量、静音 |
| `main/voice.c` / `.h` | 小智会话生命周期、单轮与追问、Opus 编解码、AEC 打断后重新聆听 |
| `main/voice_capture.c` / `.h` | 采集任务：I²S 读、AEC 前端、WakeNet、端点检测；只操作事件组 + 两个流缓冲 |
| `main/voice_internal.h` | 两语音任务共享的采样格式、缓冲尺寸、事件位契约 |
| `main/voice_settings.c` / `.h` | OTA 地址的 NVS 存取、URL 校验 |
| `main/voice_audio.c` / `.h` | 纯函数：立体声↔单声道、上传前限幅增益（无 SDK 依赖，主机可测） |
| `main/device_mcp.c` / `.h` | 发布屏幕同源快照，注册时间/天气/传感器/音量工具 |
| `main/fonts_aa.c`、`main/font_clock.c`、`main/font_icons.c` | 生成的 1bpp 字模，勿手改 |
| `tools/genfont.py`、`tools/genicons.swift` | 字模生成器（字体文件不进仓库） |
| `components/esp_xiaozhi/` | 上游 0.1.4 单份本地 override（`override_path`），勿另建副本 |

- 保留 `main/`，不改 `src/`；新增 C 文件同步 `main/CMakeLists.txt` 与主机 harness。

## C 代码风格

以人工阅读和审查为先。命名、文件组织和控制流可参考 [NGINX 开发指南](https://nginx.org/en/docs/dev/development_guide.html#code_style) 和 [Linux 内核编码风格](https://docs.kernel.org/process/coding-style.html)。

- 文件级变量用能说明用途的小写下划线名称，例如 `display_lock` 和 `sta_events`。局部变量在含义清楚时可以简短。
- 一起初始化、更新或清理的状态可归入同一结构体。私有类型定义在使用它的 `.c` 文件里，跨文件需要完整定义的类型放在对应头文件。
- 主路径按执行顺序展开，错误尽早处理。多条失败路径共用清理逻辑时，出口名称写清要释放的资源。注释说明约束、资源所有权或设计原因。
- 排版以 `.clang-format` 为准（4 空格、120 列、函数左花括号换行）。字模文件通过生成器更新。

## 通用复用

- JSON 用 `espressif/cjson`，HTTP/TLS/DNS/网络用 ESP-IDF；不自写协议栈。
- 新依赖写进 `idf_component.yml` 并由 `dependencies.lock` 锁定；`managed_components/` 不入库。
- 自己实现只限业务规则、设备适配和库缺口；业务校验（字段范围、日期连续性、刷新策略）留在本项目。

## 应用状态边界（违反会重复踩坑）

- 主循环管网络与刷新；`clock` 任务每秒检查分钟/日期/语音快照。二者共用显示互斥锁串行化绘图+刷新+SPI 等待；网络与配网等待期间不得持锁。`ui.c` 只画帧缓冲，不联网、不刷新。
- `app->w` = 本次请求结果（开始时被清空，失败可为脏数据）；`app->shown` = 最近成功快照。**失败重绘只读 `shown`**，不能靠 `w` 保留旧预报。
- 失败时 `present()` 用旧快照（首次失败用占位）重画，再覆盖顶部重试标题与底栏状态；正文像素不动。
- 首列显示 `今天` 仅当日期等于已同步当地日期。
- 底栏温湿度只取 `sensor_reading_t`；未接驱动时 `valid=false` → `--.- °C` / `-- %`，禁止用天气温湿度代替。接入后填 `temperature_c`、`humidity_percent`、`valid=true`。
- 已对时逐时页随当地小时滚动；未对时从缓存首条开始。MCP 始终返回全部 21 条。

## 启动、重试、按键

- 启动先画占位页并 `epd_refresh_full()`（清断电残影），再等 5 s 才连网；不能等天气成功才画首屏。
- 连接等待 25 s；有凭据连不上按 5→10→20→40→60 s 阶梯重试。无凭据才进配网。
- 天气失败同一阶梯，成功后清零并回 30 分钟；两者计数独立。等待不含请求/对时/刷屏耗时，无每秒倒计时。
- 拿到 IP 即后台 SNTP；每次天气成功后 `net_apply_tz()`（时区取天气的 UTC 偏移）。
- 长按 5 s：暂停语音 → 普通刷新显示「WiFi 重置中」→ 清 WiFi 凭据（保留服务设置）→ 约 1 s → 全刷配网页 → `net_portal()`。重置/配网画面隐藏时钟与语音反馈；长按只报一次，松开不触发短按。
- 按键任务每 20 ms 采样，8 项队列，长按优先；主任务在网络调用返回后执行重置。配网结束清空队列。
- 主任务栈保持 10 KB（HTTPS 同步跑在主任务；4 KB 会在 TLS 握手栈溢出重启）。

## 天气契约

- 顺序：`ip-api.com/json`（出口 IP 定位，无需 key）→ Open-Meteo HTTPS。不请求 `current`。
- 参数：`daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max`、`hourly=weather_code,temperature_2m,precipitation_probability`、`temperature_unit=celsius&timezone=auto&forecast_days=5&forecast_hours=21`。
- 日数组每个恰好 5 项、逐时每个恰好 21 项；缺失、`null`、数量错、截断或带非空白尾随内容 → 整次失败。只读对象直接成员，不要误读 `*_units` 或嵌套同名字段。
- 日日期合法且连续；温度有限且可存 `float`，最低 ≤ 最高；概率 `[0,100]`；天气码整数 `0..99`，未映射码 UI 画 `?`。
- 逐时格式 `YYYY-MM-DDTHH:00`，首条日期 == `days[0].date`；相邻本地时差允许 −1/0/+1/+2 h（DST）。每条保留当地日期（YYYYMMDD）+ 小时。MCP 日期输出 `YYYY-MM-DD`，重复/跳小时保持接口顺序。
- 响应缓冲静态 8192 B，达到边界即失败。改字段或数量时同时改结构体、解析器、缓冲和测试 URL。

## 语音（小智 AI）

- 协议层用 `espressif/esp_xiaozhi`（依赖 `esp_websocket_client`、`mcp-c-sdk`）；**不要重写 WebSocket/MQTT/hello/认证/MCP**。本地修改范围见 `components/esp_xiaozhi/LOCAL_PATCH.md`。
- 顺序：`chat_init`（回调）→ `get_info`（POST 板卡信息，拿激活码，MQTT/WS 配置写 NVS）→ `chat_start` → `open_audio_channel`（自动发 hello）→ `send_audio_data` → `stop`/`deinit`。事件回调只报协议事实，状态机/UI/命令由应用决定。
- 音频契约：hello = `opus`/16000 Hz/1 ch/60 ms。`audio.c` 是 16 kHz/16 bit/**双声道**（左声道为麦克风），语音任务必须转单声道并 60 ms 分帧，不要把双声道直接当单声道发。接线真值在 `main/audio.h` 顶部 `#define`（EN=15、DOUT=16、WS=17、DIN=18、BCLK=9、MCLK=10、SCL=11、SDA=12）；改线只改那里。
- 音频参数：`I2S_NUM_0` 全双工 + `I2C_NUM_0` 400 kHz，`MCLK_MULTIPLE_384`，默认音量 80（0..100，NVS 恢复），麦克风增益 18 dB。`audio_init()` 后 DAC 静音且 PA_EN 拉低（板载 WS2812 在 GPIO48，与 PA_EN 无关）；播放前 `audio_set_mute(false)`。`esp_codec_dev` 音量是 dB 线性映射（0=−50 dB），自检别用 30。
- `voice_init()` 载模型建任务；联网后取配置建会话；返回激活码时复制到反馈快照（1–16 个无空格可打印 ASCII）并上屏/串口，等待绑定后重试，不打开未激活会话。challenge 激活未实现。
- 唤醒：WakeNet（`esp-sr` 2.4.7 `wn9_xiaoluxiaolu_tts2`）只在本地；命中后才开麦上传。模型必须在 `model` 分区；无 PSRAM 时跳过语音。
- 建连：`open_session()` 在 `chat_start` 后等 `ESP_XIAOZHI_CHAT_EVENT_CONNECTED`（≤ `VOICE_CONNECT_TIMEOUT_MS`），超时按失败重试——`chat ready` 表示套接字已通。断网/STA 停止后停止接收并清会话；快速恢复 IP 也要先清旧会话；普通断线保留激活码，重配网清除。
- 追问（VAD 可用时自动启用）：播放结束静音后开 6 s 本地 VAD 窗口；确认开口带 ≤120 ms 前滚，复用同一通道 `listen start/stop`；窗口内取数最多等 4 s（网络锁）。沉默退回待唤醒。
- 端点：唤醒后的录音至少保留 600 ms，避免唤醒词尾音提前触发静默收尾。保护窗口过后，本地 VAD 约 300 ms 静默结束录音；追问无首句保护窗口。上行**不做逐帧增益归一**（会放大静音、让服务端能量 VAD 判不出停顿，且逐帧泵动）；音量只靠 codec 的麦克风增益。录音上限 15 s，随后发 `listen stop`；服务端收尾另行保留。
- 播放：正常结束等 I²S DMA 尾帧（`AUDIO_PLAYBACK_TAIL_MS`）再静音；打断/断线/出错立即停。
- AEC 打断：`voice_capture.c` 用 ESP-SR AFE（`afe_config_init("MR", ...)`，1 麦克风 + 1 播放参考），参考信号是 `play_response()` 实际写入 I²S 的 PCM（`playback_ref_stream`，按写入顺序，不加延迟）。WakeNet 在 AFE 之外，检测 AEC 输出；播放期间保持 `ARMED`，命中后置 `VOICE_BIT_INTERRUPT`，由 `run_turn()` 静音、丢音频、发 `abort`(wake_word_detected)、同通道重新 `listen start`。AFE 失败则回退为播放期不检测。
- 两任务只通过 `voice_internal.h` 的事件位 + 两个流缓冲通信；新增位同步更新该头与主机 harness。
- 预算（两端要一起改）：设备首音频 90 s、播放断流 30 s；Rust 正常首音频 65 s、含失败提示 75 s、整回合 180 s（不含 ASR）。
- WS stop 失败：保留 chat/MCP/Opus，释放网络锁后由语音任务重试清理，禁止开新会话；成功后 `deinit` 消费句柄。
- 屏上反馈：`voice_feedback.c/.h` 临界区发布/读取快照，状态变更通过观察者即时唤醒显示任务；连接重试/断网/错误保留待激活码，其他状态清空。显示任务合并更新：顶部激活码、右下角状态（纯局刷），保留天气正文/温湿度/时钟；配网页优先。新增中文文案必须重跑 `tools/genfont.py`。
- 隐私：接上后麦克风音频与识别文本会发到配置的服务端，不是本地处理。
- MCP：`device_mcp_publish()` 发布 `app->shown` + `sensor_reading_t`；工具 `self.get_current_time`、`self.get_weather_forecast`、`self.get_sensor_reading`（无参数只读）、`self.get_device_status`、`self.audio_speaker.set_volume`（0–100 写 NVS）。不要为一次问答重跑天气；未接入传感器必须 `available=false`；时间仅 SNTP 成功且时区已知时 `synchronized=true`；天气含抓取时间、单调时钟 age、`stale`/`retrying`，未对时抓取时间为 null。

## 配网页

- 热点 `weather-XXXXXX`（开放），`192.168.1.1`，扫描或手填 SSID；凭据明文 HTTP 存 NVS，不先验证连接。不写编译期凭据、不提交真实密码。
- 模板替换**不转义、不递归**；SSID 先 `html_escape()`。新增 token 时同步更新替换表。
- 表单字段：`ssid`/`ssid2`（手填优先）/`pass`/`voice_url`（必填）。服务设置与 WiFi 凭据同一 `wifi` NVS 事务；长按重配网只清 WiFi 部分。旧版 NVS 中的追问开关值不影响运行行为。
- `send_page()` 分块发送并内联 CSS/JS；结果页自包含（保存后热点即关闭）。改页面后运行 ESP-IDF 构建。

## 屏幕驱动与刷新

- 可见 296×128；面板 RAM `PANEL_W=128`/`PANEL_H=296`；帧缓冲 4736 B。`EPD_W=300` 是逻辑画布，`epd_pixel()` 裁掉 `x≥296`——新内容不能画在裁掉区。
- 引脚：SCK 38、MOSI 39、RST 40、DC 41、CS 42、BUSY 21；SPI2 4 MHz；旋转只在 `epd_pixel()` 做。
- `0x01` 门线 `0x27,0x01,0x00`，`0x45` Y 结尾 `0x27,0x01`。**不要改成 300 行 `0x2B`/4800 B**：多 4 行会回绕（右侧像素跑到左侧）。
- 全刷：复位 → 全配置 → `0x32` 前 153 B + 6 B 电压 → `0x24` 写 4736 B → `0x22 0xC7` → `0x20`。
- 局刷：复位 → 基础寄存器 → `0x3C 0x80` → `0x32` partial LUT 153 B → `0x37` 10 B → `0x22 0xC0`+`0x20` 预置 → `0x24` 图 → `0x22 0x0F`+`0x20`。不能只把全刷 `0x22` 改成 `0xFF`。
- `epd_refresh()` 从启动全刷后开始计普通刷新；每 `FULL_REFRESH_EVERY`（=5）次做一次维护全刷，其余局刷。时钟帧/重试帧/天气更新走普通刷新并推进计数；`epd_refresh_partial()` 仅做局刷、不推进计数，用于右下角状态单独更新；切页单独计数。
- 切页规则：第 1–7 次只局刷；第 8 次全刷并清零（若普通维护全刷同时到期，合并为一次）。`epd_refresh_full()` 强制全刷但不推进普通计数，只用于启动占位、首次进配网、长按重置；它也清零切页计数。
- BUSY 高电平为忙；初始化等 2 s、刷新等 15 s，超时只记日志。
- 局部刷新仍传整帧，不是只传矩形。

## 排版锚点

- 日页：5 × 52 px 列，`GRID_X=18`，中心 44/96/148/200/252。
- 逐时页：10 × 28 px 列，`HOUR_GRID_X=8`，中心从 22 每次 +28；标题 `未来10小时`。
- 共用 `DATE_BASE=42`、`ICON_TOP=46`、`TEMP_BASE=89`、`PRECIP_BASE=103`。
- 日图标 `font_icon_daily`：advance 40、line 27、基线 73、墨迹 y=46..72。逐时图标 `font_icon`：advance 26、line 23、基线 69、墨迹 y=46..68。
- 日页日期：今天→`font_mid`，其他→`font_small` `MM/DD`，无数据 `--/--`。温度单行 `最低°-最高°`，上限 48 px：`font_temp`→`font_small`→`--`（`12°-25°` 在 16px 是 53 px，装不进 52 px 列）。
- 逐时温度上限 26 px（`font_temp`→`font_small`→`--`）；概率超宽去 `%`（`100%`→`100`），未知 `--%`。
- 底栏基线 124：温度标签 x=8/读数 x=46（≤58 px）；湿度标签 x=110/读数 x=146（≤50 px）；全用 `font_mid`。
- 状态角落 x≥200、y≥110，右对齐到 290，≤90 px：网络/天气（`正在联网`/`正在取数`/`同步时间`/`已更新`/`重试 #n`，另有 `UI_STATUS_NONE`）与语音（`语音连接`/`等待激活`/`待唤醒`/`正在聆听`/`等待追问`/`等待回答`/`正在说话`/`无网络`/`设置语音`/`语音失败`/`语音不可用`）。语音活动态优先，其次正在联网/取数/重试，语音就绪在同步时间完成后显示 `待唤醒`。`set_status(..., true)` 的帧缓冲改动只能落在该角落，且只走局刷。
- 时钟：`font_clock` 12px Bold，基线 16，右对齐 292；对时且有天气时区时显示当地 `HH:MM`，否则显示 `--:--`。分钟更新只改 x=230..295、y=0..21；跨午夜或缓存过期可同时改日期标签/`预报待更新` 标题。重试文案限 x<230。配网页隐藏时钟，返回后恢复。
- 字模：1bpp，16/14/12 px（+ `font_clock` 12 px Bold）；**中文只在 `font_mid`**；`°` 在所有字号。图标来自 `font_icons.c`，未知 WMO 码画 `?`，不要在 `ui.c` 手绘图标。
- 日页顶部建议只看 `days[0]`：无数据/概率非法 → `预报待更新`；雷雨/冰雹 → `雷雨注意安全`；其余四舍五入后概率 ≥51% → `今日带伞`；已知天气低概率 → `降水概率低`；未知码低概率 → `预报待更新`。百分比显示与阈值用同一舍入。

## 字模再生成

仅当改屏幕文案、字号、字体、图标映射或行锚点时才需要；生成文件已入库，普通构建不需要字体/Pillow/Swift/联网。

```bash
# 文字（Pillow + Roboto + Noto Sans SC；路径含 [] 必须加引号）
FONT_TTF='.cache/fonts/Roboto[wdth,wght].ttf' \
FONT_CJK_TTF='.cache/fonts/NotoSansSC[wght].ttf' \
python3 tools/genfont.py                     # --only clock 只生成 font_clock.c

# 图标（macOS Swift；TTF 需与 third_party/weather-icons/README.md 的 SHA256 一致）
swift tools/genicons.swift .cache/weather-icons/weathericons-regular-webfont.ttf \
    main/font_icons.c .cache/weather-icons/preview.png
```

- 字体下载地址、固定提交、SHA256、许可证见 `third_party/roboto-clock/`、`third_party/weather-icons/`；TTF/CSS/预览 PNG 不入库。
- 中文扫描范围：`main.c`、`ui.c`、`weather.c` 的字符串（不含网页文案）。生成后检查头部与缺字。
- 生成后跑 `test_app.py --only ui`、`test_app.py`、`preview_ui.py`，并人工看预览的太阳/云/降水方向。只验证不覆盖时输出到 `.cache/` 后 `cmp`。

## 改动 → 必须跑的回归

| 改动 | 回归 |
| --- | --- |
| 驱动、布局、字模、图标 | `test_app.py` + `preview_ui.py`（图标另核对预览与 `third_party/` 说明） |
| 时钟任务、显示互斥、重试/配网页绘图 | `test_clock.py --sanitize` |
| 配网页、模板、嵌入资源 | ESP-IDF 构建 |
| 天气解析/数据契约 | `test_app.py --only weather`（含 `--only app`） |
| 设备工具、音量、快照 | `test_device_mcp.py` + `test_audio_volume.py` |
| `voice.c`/`voice_capture.c` 状态与时序 | `test_voice_lifecycle.py` + `test_voice_session.py --sanitize` |
| 语音设置、URL 校验 | `test_voice_settings.py` |
| 模型/依赖/分区 | 构建输出核对 app 与模型镜像大小 |

改布局时同步核对：`ui.c` 列宽/网格/锚点；`preview_ui.py` 的 `COL_W`/`GRID_X`/`DATE_BASE`/`ICON_TOP`/`DAILY_TEMP_BASE`/`PRECIP_BASE`/`icon_mask`/`expect_forecast`；`test_app.py` 的 `centers`/`boundaries`/`hour_centers` 与宽度、基线断言。不要只改断言掩盖越界。

## 覆盖边界（测试通过 ≠ 真机可用）

- 只有真机能验：真实 WiFi/HTTP(S)/TLS/DNS/NVS 时序、屏幕黑度/残影/刷新时序、WakeNet 与 VAD 识别率、AEC 实际抑制量、端到端打断延迟、音质与首句延迟。
- `test_app.py` 的 `main.c` 语音与发布调用是 stub；`test_voice_lifecycle.py` 抽取真实状态函数（不覆盖整个 `voice.c` 或 SDK 停止时序）；`test_voice_session.py` 编译真实会话/播放/追问函数，stub RTOS/Opus/VAD/I²S。
- `--baseline` 只在数据契约、布局、字模一致时有意义；`.cache/` 历史快照不是运行依赖。
- 不要把 `ABLATION.md` 的旧体积/像素/残影数字当成当前保证。
