# weather-esp32

ESP32-S3 + 2.9" 墨水屏（SSD1680，可见 296×128）天气面板，纯 ESP-IDF/C。五天日预报、逐时页、时钟、配网页，并支持小智 AI 语音问答（WakeNet 本地唤醒）。语音协议层由 `espressif/esp_xiaozhi` 提供，服务端见 [`xiaozhi-server-rs`](../xiaozhi-server-rs)。

## 接线

| 屏幕 | ESP32-S3 | | 音频（ES8311 + NS415B） | ESP32-S3 |
|---|---|---|---|---|
| SCK | GPIO38 | | 功放 EN | GPIO15 |
| SDA (MOSI) | GPIO39 | | I²S DOUT | GPIO16 |
| RST | GPIO40 | | I²S WS | GPIO17 |
| DC | GPIO41 | | I²S DIN（麦克风） | GPIO18 |
| CS1 | GPIO42 | | I²S BCLK | GPIO9 |
| BUSY | GPIO21 | | I²S MCLK | GPIO10 |
| VCC | 3V3 | | I²C SCL / SDA | GPIO11 / GPIO12 |

改引脚只动 `main/epd.h` 和 `main/audio.h` 顶部的 `#define`。屏幕用 3.3V；ES8311 与 NS415B 电源按模块规格，必须与 ESP32 共地。Octal PSRAM 占用 GPIO33–37，GPIO43/44 留给外接 UART，GPIO19/20 保留 USB-JTAG。

## 编译与烧录

本机使用 ESP-IDF v6.0.2：

```bash
. ~/.espressif/tools/activate_idf_v6.0.2.sh
idf.py build
idf.py -p 实际串口 flash      # 同时写入 app、分区表、bootloader 和 WakeNet 模型
idf.py -p 实际串口 monitor    # 需在真实终端运行
```

首次烧录必须包含 `model` 分区（`build/srmodels/srmodels.bin`，`idf.py flash` 会自动写）；缺模型时日志报 `WakeNet requires PSRAM` 或 `voice unavailable`。整体烧录可用 `erase-flash` 后重刷。

`idf.py monitor` 用脚本读串口会复位芯片、拿不到应用日志；要稳定日志请接 USB-TTL 到 GPIO43/44。

## 首次配置

1. 上电后连热点 `weather-XXXXXX`（开放、无密码），打开 `192.168.1.1`。
2. 选 WiFi 并填密码，同一页“小智语音”里填**服务信息地址（OTA）**，必填，固件没有内置默认值。自建 Rust 服务示例：`http://PC局域网IP:8000/xiaozhi/ota/`。
3. 保存后设备连网，先取一次天气，随后建立小智会话。设备不能填 `localhost`。WebSocket 地址由 OTA 响应下发（Rust 服务默认 `ws://PC局域网IP:26003/xiaozhi/v1/`），设备侧无需填写。

换网络：**长按按键 5 秒**清 WiFi 凭据并重新配网（保留小智服务设置）。已保存凭据但连不上路由器时按 5、10、20、40、60 秒阶梯重试，不会自动开热点。

## 使用

- **短按**切页（按天 / 未来 10 小时）。
- 说 **“小鹿小鹿”** 唤醒：本地 WakeNet 命中后才开麦上传，空闲时不上传音频。
- 一轮问答：唤醒后至少录 600 毫秒，之后约 300 毫秒静默即结束录音，最长 15 秒。
- **免唤醒追问默认开启**：回复播完后约 6 秒内可直接说下一句，设备先在本地确认开口再上传；窗口内不说话则回到待唤醒。
- 播放中说“小鹿小鹿”可打断（需要 AEC 初始化成功）。
- 说 **“退下”/“退下吧”/“再见”/“拜拜”** 结束会话：服务端下发 `goodbye` 并断开，设备释放 WebSocket/MCP/Opus，只留本地唤醒；下次唤醒重新走 OTA 建连。

屏幕右下角显示语音状态（连接、聆听、等待回答、说话、失败）。空闲时显示天气状态，激活码显示在顶部。

## 语音能问什么（MCP）

设备通过 `mcp-c-sdk` 注册工具，由服务端模型调用：

| 工具 | 内容 |
|---|---|
| `self.get_current_time` | 设备时钟 + 天气返回的 UTC 偏移；仅在 SNTP 成功且时区已知时 `synchronized=true` |
| `self.get_weather_forecast` | 与屏幕同源的最近成功快照（5 天 + 21 小时），含抓取时间、缓存年龄、`stale`/`retrying` |
| `self.get_sensor_reading` | 本地温湿度；**当前未接传感器驱动**，返回 `available=false` |
| `self.get_device_status` | 扬声器当前音量 |
| `self.audio_speaker.set_volume` | 音量 0–100，写入 NVS，重启恢复（首次默认 80） |

**没有接入温湿度传感器**，被问到时应答“没有可用的传感器读数”，不要报模拟值。语音识别文本与回答会发到所配云服务，不是纯本地处理。

## 数据与刷新

1. `http://ip-api.com/json/` 用出口 IP 取城市与经纬度（无需 key）。
2. Open-Meteo 取 5 天日预报 + 21 条逐时数据，摄氏度、`timezone=auto`。要求数组长度、日期连续、天气码 0–99、概率 0–100，任一不符整次失败，不用旧字段冒充。
3. STA 拿到 IP 后后台 SNTP 校时，天气成功后按返回的 UTC 偏移设置时区。
4. 正常 30 分钟刷新；失败保留最近成功快照，顶部显示 `无网络 5s后重试 #n`，按 5、10、20、40、60 秒阶梯重试。

底部温湿度只来自本地传感器，未接入时显示 `--.- °C` / `-- %`。出门提示只依据今天：四舍五入后降水概率 ≥51% 显示 `今日带伞`，雷雨/冰雹优先 `雷雨注意安全`，低概率已知天气显示 `降水概率低`；日最大概率不代表正在下雨。

## 常见日志

| 日志 | 含义 |
|---|---|
| `voice unavailable` | 语音未启动：查 ES8311、PSRAM、`model` 分区 |
| `WakeNet requires PSRAM` | 没有可用 Octal PSRAM |
| `service info URL not configured` | 配网页未填 OTA 地址 |
| `server config unavailable` | 信息接口访问不到或未下发 WebSocket 配置，约 30 秒后重试 |
| `wake word detected` / `listening` | 本地命中唤醒词，开始上传 |
| `user:` / `bot:` | 识别文本与回答文本 |
| `chat ready` | WebSocket 已连通，可以唤醒 |

## 开发与回归

内部实现、任务/事件位契约、字模生成、刷新策略、回归覆盖边界见 [`AGENTS.md`](AGENTS.md)；进度与真机验收项见 [`TODO.md`](TODO.md)；自建服务端见 [`LOCAL_XIAOZHI.md`](LOCAL_XIAOZHI.md)；设计取舍见 [`ABLATION.md`](ABLATION.md)、[`IMPROVEMENTS.md`](IMPROVEMENTS.md)。

```bash
python3 tools/test_app.py                 # 应用/解析/UI
python3 tools/test_clock.py --sanitize    # 时钟与显示互斥
python3 tools/test_device_mcp.py          # 设备工具与音量
python3 tools/test_voice_lifecycle.py     # 会话清理、断网恢复、重配网
python3 tools/test_voice_session.py --sanitize  # 录音、播放、追问、打断
python3 tools/preview_ui.py               # 布局预览 PNG（演示数据）
```

主机回归需要 Python 标准库和本机 C 编译器；天气/应用回归会编译 Component Manager 已下载的 cJSON，首次先跑 `idf.py build` 或 `reconfigure`。主机测试不覆盖真实 WiFi、HTTP/TLS、DNS、I²S 时序、WakeNet 识别率、AEC 抑制量和听感，这些只能真机验收。
