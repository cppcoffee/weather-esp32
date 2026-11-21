# weather-esp32

ESP32-S3 + 2.9" 墨水屏（SSD1680，296×128）天气与 AI 语音面板（ESP-IDF / C）。支持 5 天与未来 10 小时天气预报、SNTP 时钟、本地温湿度气压监测、网页配网，以及本地唤醒（WakeNet）语音问答。

配套自建语音服务端见 [`xiaozhi-server-rs`](https://github.com/cppcoffee/xiaozhi-server-rs)。

## 硬件与接线

需要 **ESP32-S3（16 MB Flash + Octal PSRAM）**，所有外设与开发板共地：

| 模块 | 接线（GPIO） |
|---|---|
| 墨水屏（SSD1680） | `SCK=38` `SDA(MOSI)=39` `RST=40` `DC=41` `CS1=42` `BUSY=21` `VCC=3V3` |
| 音频（ES8311 + NS415B） | `PA_EN=15` `DOUT=16` `WS=17` `DIN=18` `BCLK=9` `MCLK=10` `SCL=11` `SDA=12` |
| 传感器（AHT20+BMP280，可选） | `SDA=5` `SCL=6` `VCC=3V3`（固定地址 `AHT20=0x38`、`BMP280=0x77`，改地址见 `main/sensor.h`） |
| 按键 | `GPIO4` 接微动开关到 `GND`（内部上拉） |

- 避开预留引脚：`GPIO33–37`（Octal PSRAM）、`GPIO19/20`（USB-JTAG）、`GPIO43/44`（外接 UART 日志）。
- 如需调整引脚，修改 `main/epd.h`、`main/audio.h`、`main/sensor.h`、`main/button.h` 顶部 `#define`。

## 编译与部署

环境要求：**ESP-IDF v6.0.2**。使用语音功能前，请先按 `xiaozhi-server-rs` 文档在局域网部署服务端并获取 OTA 地址。

```bash
. ~/.espressif/tools/activate_idf_v6.0.2.sh
idf.py build
idf.py -p <串口> flash      # 一并写入 app、分区表、bootloader 与 WakeNet 模型分区
idf.py -p <串口> monitor    # 需交互式终端；也可接 USB-TTL 到 GPIO43/44 查看日志
```

- 首次烧录会自动写入 `model` 分区（`build/srmodels/srmodels.bin`）；若语音模型异常，先执行 `idf.py -p <串口> erase-flash` 再重刷。
- 修改 `sdkconfig.defaults` 后，需删除已有 `sdkconfig` 再重新编译才会生效。

## 配网与设置

1. **首次配网**：上电后连接开放热点 `weather-XXXXXX`（无密码），浏览器打开 `http://192.168.1.1`。
2. **表单设置**：
   - **Wi-Fi**：从扫描列表选择或手动输入 SSID 与密码。
   - **服务信息地址（OTA）**：必填（无默认值），填写局域网服务端地址，如 `http://192.168.1.2:8000/xiaozhi/ota/`（不可填 `localhost`）。
3. **重新配网**：长按按键 **5 秒** 清除 Wi-Fi 凭据并重进配网（上次保存的语音服务地址会保留）。
4. **调节音量**：默认 `80`（范围 `0–100`），可在语音对话中让助手调整，自动写入 NVS 持久保存。

## 日常使用

- **按键**：短按切换「5 天预报 / 未来 10 小时」；长按 5 秒重新配网。
- **语音问答**：说 **“小鹿小鹿”** 唤醒；播完后约 6 秒内可直接追问，播放中再说唤醒词可打断；说“退下 / 再见 / 拜拜”或静默超时后结束会话并回到待机。
- **自动更新**：联网后按 IP 定位拉取天气（每 30 分钟）并 SNTP 校时；本地传感器每 10 分钟采样一次（未接入时显示 `--`）。语音助手支持查询时间、天气、本地温湿度气压、设备状态及调音量。

## 常见排查

| 日志 / 现象 | 排查方式 |
|---|---|
| `voice unavailable` / `WakeNet requires PSRAM` | 检查 ES8311 接线、Octal PSRAM 配置，或 `erase-flash` 后重刷 `model` 分区 |
| `service info URL not configured` | 重进配网页填写服务信息地址（OTA） |
| `server config unavailable` | 检查局域网服务端是否可达、OTA 接口是否返回 WebSocket 配置 |
| 屏幕显示激活码 | 按服务端提示输入激活码完成绑定 |

开发规范与主机回归测试见 [`AGENTS.md`](AGENTS.md)。
