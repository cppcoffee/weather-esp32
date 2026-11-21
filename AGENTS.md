# AGENTS.md

ESP32-S3（ESP-IDF v6.0.2 / C）墨水屏天气与语音面板。源码在 `main/`，本地组件补丁在 `components/esp_xiaozhi/`，字模生成脚本在 `tools/genfont.py`。硬件接线、烧录与设置见 [README.md](README.md)。

## 构建与规范

- **环境**：ESP-IDF v6.0.2，使用 `idf.py build` 编译验证；优先复用 ESP-IDF 与 Component Registry 托管组件，避免手写底层驱动或重复造轮子。
- **配置**：`sdkconfig.defaults` 不会自动同步到已有 `sdkconfig`；主任务栈保持 ≥10 KB（同步 HTTPS）。
- **代码**：按 `.clang-format` 排版；增删源码同步更新 `main/CMakeLists.txt`；沿用 `components/esp_xiaozhi` 本地 override（差异见 `LOCAL_PATCH.md`）。

## 核心不变量

- **显示与锁**（`main.c` / `ui.c` / `epd.c`）：`ui.c` 只画帧缓冲，`epd.c` 处理旋转裁切（可见区 296×128，RAM 保持 296 行）；绘图与刷屏共用显示锁，HTTP 与配网等待期间必须释放。
- **刷新策略**：切页仅用 `epd_refresh_page_switch()`（每第 8 次全刷，其余局刷）；同页更新用 `epd_refresh()`（同帧跳过，局刷后同步新旧 RAM 窗口，失败重建参考）。
- **页面与字模**：开机先占位，天气失败保留上次成功的 `app->shown`（首轮失败用占位），天气与连网各自退避；按键由 `espressif/button` 驱动，长按 5 秒仅清 Wi-Fi 保留语音地址；配网页隐藏时钟和语音状态；逐时保留原顺序。字模用 `tools/genfont.py` 从 `third_party/` 生成，改中文后需核查缺字。
- **天气与传感器**（`weather.c` / `sensor.c` / `device_mcp.c`）：天气需同时有完整 5 天和 21 个逐时点才发布，UI 与 MCP 共用同源成功快照（含时效，单次问答沿用）；传感器固定地址由 `k0i05/esp_ahtxx` + `k0i05/esp_bmp280` 驱动，读数与 MCP 共用，未接入或无效标为不可用；时间工具要求 SNTP 同步且时区已知。
- **配网**（`net_portal.c` / `portal_web.c`）：Wi-Fi 与语音地址存 NVS；SSID 通过 `cJSON` 下发、前端 DOM 安全渲染；真实凭据仅存本地。
- **语音与音频**（`voice.c` / `voice_capture.c` / `audio.c` / `voice_internal.h`）：I²S 16 kHz 双声道（左声道麦克风），上行 Opus 16 kHz 单声道 60 ms 帧；`voice_capture.c` 通过 ESP-SR `AFE_SR` 统一运行 AEC + WakeNet + VAD（含 `vad_cache`）；保留固定麦克风增益与播放尾帧，参数以 `voice_internal.h` 为准。会话任务独占连接与清理，断网重连前先清旧会话，stop 失败由原会话重试；AEC 以实际写出的播放 PCM 作参考；涉及协议或回合时限时核对 `xiaozhi-server-rs`。

## 验证

改动后执行 `idf.py build` 验证编译链接；面板残影、传感器读数、配网页、联网/断线时序、音频质量、唤醒与打断需烧录真机验证。
