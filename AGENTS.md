# AGENTS.md

ESP32-S3 天气面板，使用 SSD1680 墨水屏和 ESP-IDF/C。源码在 `main/`，主机回归在 `tools/`。接线、烧录和使用方法见 [README.md](README.md)。

## 构建与修改

- 使用 ESP-IDF v6.0.2，按 README 激活环境并执行 `idf.py build`。天气与应用回归需要先构建或 reconfigure，以取得 cJSON。
- 目标配置见 `sdkconfig.defaults`。已有 `sdkconfig` 不会自动继承 defaults 的修改。主任务需保留 10 KB 栈供同步 HTTPS 使用。
- 烧录时确认实际串口。首次烧录需写入 WakeNet 模型分区。常规监视用交互式 `idf.py monitor` 或 GPIO43/44 上的 USB-TTL。
- C 文件按 `.clang-format` 排版。新增源码同步更新 `main/CMakeLists.txt` 和对应测试。JSON 用 cJSON，网络和 TLS 用 ESP-IDF，依赖由 `idf_component.yml` 和 `dependencies.lock` 管理。

## 应用与显示

- `main.c` 管网络、天气更新、按钮和显示任务，`ui.c` 只画帧缓冲，`epd.c` 管面板。绘图和刷屏共用显示锁，HTTP 和配网等待期间释放该锁。
- 开机先显示占位页。天气失败时只用上次成功的 `app->shown`，首轮失败用占位。天气与连网各自退避。长按只清 WiFi 凭据，保留语音服务地址。
- `epd_refresh_page_switch()` 只用于按天和逐时页切换。第 8 次切页全刷，其他切页用局刷。同页时钟、天气、传感器和语音状态用 `epd_refresh()` 更新变化区域，同帧跳过。
- 面板可见区为 296×128，旋转和裁切由 `epd.c` 处理，RAM 高度保持 296 行。局刷后同步新旧 RAM 的对应窗口，失败后重建参考。
- 时钟用已同步的本地时间，逐时数据保留原顺序，配网页隐藏时钟和语音状态。修改布局时同步校验预览和像素断言。
- 字模用 `tools/genfont.py` 生成，字体来源见 `third_party/`。新增屏幕中文后检查缺字并运行预览。

## 数据、配网与语音

- `weather.c` 校验定位与 Open-Meteo 响应。天气缓存必须有 5 天和 21 个逐时点，整份数据有效才发布。MCP 使用同源成功快照并携带缓存时效，一次问答沿用该快照。
- `sensor.c` 提供本地温湿度和气压，界面和 MCP 共用读数。未接入或无效时标为不可用。时间工具要求 SNTP 同步且时区已知。
- `net_portal.c` 和 `portal_web.c` 管配网和表单。保留 WiFi 与语音地址的 NVS 保存行为，SSID 插入 HTML 前转义。真实凭据仅存本地。
- `voice.c` 管会话与播放，`voice_capture.c` 管采集、WakeNet、VAD 和 AEC。沿用 `components/esp_xiaozhi` 这个本地 override，协议差异见其 `LOCAL_PATCH.md`。
- I²S 为 16 kHz 双声道，左声道是麦克风。网络上行为 Opus、16 kHz、单声道、60 ms 一帧。保留本地唤醒、固定麦克风增益和播放尾帧处理，录音与等待参数以 `voice_internal.h` 为准。
- 会话任务负责连接和资源清理。断网重连前先清旧会话，停止失败时由原会话重试清理。AEC 使用实际写出的播放 PCM 作参考。涉及回合时限或协议时同时核对配套 Rust 服务端。

## 验证

以下脚本均在仓库根目录通过 `python3 tools/<脚本>` 运行。

| 改动 | 回归 |
| --- | --- |
| 应用、天气解析、按键 | `test_app.py` |
| 屏幕、时钟、布局和字模 | `test_app.py --only ui`、`test_clock.py --sanitize`、`preview_ui.py` |
| 语音会话与音频时序 | `test_voice_lifecycle.py`、`test_voice_session.py --sanitize` |
| 激活码、语音状态、地址设置 | `test_voice_feedback.py --sanitize`、`test_voice_settings.py` |
| 传感器、MCP 和音量 | `test_sensor.py`、`test_device_mcp.py` |
| 配网页、依赖、模型和分区 | `idf.py build` |

主机测试使用替身验证状态和数据。面板残影、联网与断线时序、音频质量、唤醒和打断效果需真机验证。
