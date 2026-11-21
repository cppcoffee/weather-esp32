# weather-esp32

ESP32-S3 + 2.9" 墨水屏（SSD1680，296×128）天气面板，纯 ESP-IDF/C。支持 5 天预报、未来 10 小时逐时页、SNTP 时钟、本地温湿度气压传感器、网页配网，以及基于 `espressif/esp_xiaozhi`（WakeNet 本地唤醒）的 AI 语音问答。自建服务端见 [`xiaozhi-server-rs`](https://github.com/cppcoffee/xiaozhi-server-rs)。

## 硬件接线

| 屏幕 (SSD1680) | GPIO | 音频 (ES8311 + NS415B) | GPIO | 传感器 (`I2C_NUM_1`) | GPIO |
|---|---|---|---|---|---|
| SCK | 38 | 功放 EN | 15 | SDA | 5 |
| SDA (MOSI) | 39 | I²S DOUT | 16 | SCL | 6 |
| RST | 40 | I²S WS | 17 | VCC | 3V3 |
| DC | 41 | I²S DIN (Mic) | 18 | | |
| CS1 | 42 | I²S BCLK | 9 | | |
| BUSY | 21 | I²S MCLK | 10 | | |
| VCC | 3V3 | I²C SCL / SDA | 11 / 12 | | |

- 修改引脚只改 `main/epd.h`、`main/audio.h`、`main/sensor.h` 顶部的 `#define`。
- GPIO33–37 被 Octal PSRAM 占用，GPIO43/44 留给外接 UART 日志，GPIO19/20 为 USB-JTAG；所有外设需与 ESP32 共地。
- 传感器自动识别 BME280、BME680/688、AHT20/SHT3x/SHT4x + BMP280/BMP388/390，每 60 秒采样并局部刷新底栏；未接入或读数异常时显示 `--.-°C` / `--.-%` / `---.-kPa`。

## 编译与烧录

本机使用 ESP-IDF v6.0.2：

```bash
. ~/.espressif/tools/activate_idf_v6.0.2.sh
idf.py build
idf.py -p <串口> flash       # 自动写入 app、分区表、bootloader 与 WakeNet 模型分区
idf.py -p <串口> monitor     # 需在真实终端运行；或接 USB-TTL 到 GPIO43/44 看稳定日志
```

> 首次烧录必须包含 `model` 分区（`build/srmodels/srmodels.bin`）；若语音异常可 `erase-flash` 后重刷。

## 配网与使用

1. **首次配网**：上电连接开放热点 `weather-XXXXXX`，浏览器打开 `192.168.1.1`。选择/输入 WiFi 并填写 **服务信息地址（OTA）**（必填，无默认值，例如 `http://PC局域网IP:8000/xiaozhi/ota/`，不可填 `localhost`）。
2. **按键**：短按切换「5 天预报 / 未来 10 小时」；**长按 5 秒**清除 WiFi 凭据并重进配网（保留服务设置）。
3. **语音问答**：
   - 说 **“小鹿小鹿”** 本地唤醒后开始上传（空闲时不上传音频）；录满至少 2.4 s 后静默约 900 ms 结束，最长 15 s。
   - **免唤醒追问默认开启**：播完后约 6 秒内可直接继续说话；播放中说“小鹿小鹿”可打断（需 AEC 正常）。
   - 说 **“退下 / 退下吧 / 再见 / 拜拜”** 结束会话并释放连接，仅保留本地唤醒。
4. **屏幕状态**：顶部显示重试提示或激活码；右下角局刷语音/联网状态；每切页 8 次全刷一次，其余为局刷。

## 天气、刷新与 MCP 工具

- **天气与时钟**：通过 `ip-api.com` 定位并请求 Open-Meteo（5 天日预报 + 21 小时逐时），每 30 分钟刷新；失败时保留旧快照并按 5→10→20→40→60 s 阶梯重试。连网后后台 SNTP 校时并应用天气返回的 UTC 时区。
- **出门提示**：仅看当日数据，雷雨/冰雹显示 `雷雨注意安全`，否则四舍五入降水概率 ≥51% 显示 `今日带伞`、低概率显示 `降水概率低`。
- **MCP 工具**：`self.get_current_time`、`self.get_weather_forecast`、`self.get_sensor_reading`、`self.get_device_status`、`self.audio_speaker.set_volume`（0–100，写 NVS 持久化，默认 80）。

## 常见日志

| 日志 | 含义 |
|---|---|
| `voice unavailable` / `WakeNet requires PSRAM` | 语音未启动（查 ES8311、Octal PSRAM 或 `model` 分区） |
| `service info URL not configured` | 配网页未填写 OTA 地址 |
| `server config unavailable` | OTA 接口不可达或未下发 WebSocket 配置（约 30 s 后重试） |
| `chat ready` | WebSocket 已连通，可唤醒 |
| `wake word detected` / `user:` / `bot:` | 唤醒命中、语音识别文本与模型回复 |

## 开发与回归

内部契约与代码规范见 [`AGENTS.md`](AGENTS.md)。

```bash
python3 tools/test_app.py                       # 应用/解析/UI
python3 tools/test_clock.py --sanitize          # 时钟与显示互斥
python3 tools/test_device_mcp.py                # MCP 工具与音量
python3 tools/test_sensor.py                    # I2C 温湿度气压传感器驱动
python3 tools/test_voice_lifecycle.py           # 会话清理与重配网
python3 tools/test_voice_session.py --sanitize  # 录音、播放、追问、打断
python3 tools/preview_ui.py                     # 布局预览 PNG
```

主机回归首次运行前需先执行 `idf.py build` 或 `reconfigure` 以下载 `cjson` 组件；真实 WiFi/TLS、I²S 时序、WakeNet/AEC 效果需真机验证。
