# weather-esp32

ESP32-S3 + GYSDISPLAY 2.9" 墨水屏（物理 296×128，面板 RAM 128×296，SSD1680；固件逻辑画布 300×128，最右 4 列不显示）天气面板。纯 ESP-IDF，C，无第三方库。

## 接线

| 屏幕 | ESP32-S3 |
|------|----------|
| SCK        | GPIO9  |
| SDA(MOSI)  | GPIO10 |
| RST        | GPIO11 |
| DC         | GPIO12 |
| CS1        | GPIO13 |
| BUSY       | GPIO14 |
| VCC        | 3V3（不要 5V） |
| GND        | GND    |

### ES8311 + NS415B 音频

当前固件使用 ESP-IDF 官方 `esp_codec_dev` 的 ES8311 驱动，I²S 以 ESP32-S3 为主机，默认 16 kHz、16 bit、双声道 Philips 格式；麦克风数据通过 `audio_read()` 获取，播放数据通过 `audio_write()` 发送。上电初始化后 DAC 静音，并显式把功放使能脚 PA_EN（GPIO2）拉低；调用 `audio_set_mute(false)` 后才同时打开 DAC 和功放。若板载 LED 或指示灯接在 PA_EN 同一网络上，空闲时不会点亮。

| 音频信号 | ESP32-S3 | 说明 |
|------|----------|------|
| NS415B EN | GPIO2 | 高电平使能功放，由 ES8311 驱动控制 |
| ES8311 DO | GPIO47 | ESP32 → ES8311 DIN/SDIN（I²S 数据输出） |
| ES8311 WS | GPIO48 | LRCK / WS |
| ES8311 DI | GPIO38 | ES8311 DOUT/SDOUT → ESP32（麦克风输入） |
| ES8311 BCK | GPIO39 | BCLK |
| ES8311 MCN | GPIO40 | 按接线名称理解为 MCLK |
| ES8311 SCL | GPIO41 | I²C 时钟 |
| ES8311 SCA | GPIO42 | 按接线名称理解为 SDA/CDATA |
| 音频 VCC | 依模块规格 | ES8311 逻辑电源通常为 3.3V；NS415B 功放电源按模块规格供电 |
| 音频 GND | GND | 必须与 ESP32-S3 共地 |
### 小智 AI 语音唤醒

启用语音后，说「小鹿小鹿」触发一轮对话。固件通过 ESP-SR WakeNet 在本地识别唤醒词，命中后才向小智服务上传问句。服务信息地址（OTA）必须逐台在配网页填写，固件没有内置默认地址。按键用于短按切页和长按 3 秒重新配网。

ESP32-S3-WROOM-1-N16R8 的 8 MB Octal PSRAM 使用 GPIO33–37。音频已移到 `GPIO2`、`GPIO47` 和 `GPIO48`；其中 I²S 的两根迁移线在模块引脚区域相邻，便于 PCB 布线。GPIO43/44 继续留给外接 UART 调试，GPIO19/20 保留 USB-JTAG。默认配置已启用 Octal PSRAM（80 MHz），首次启用前仍需确认 PCB 实际连线与模块型号一致。

`idf.py build` 生成应用和 WakeNet 模型镜像。首次烧录用 `idf.py -p 实际串口 flash` 写入 `model` 分区。屏上对话状态和时间天气 MCP 工具仍待接入。

### 使用小智 AI

1. 确认音频硬件按上面的接线表连接，尤其是 ES8311 麦克风输入、NS415B 功放和 `GPIO2`/`GPIO47`/`GPIO48`。Octal PSRAM 必须可用，WakeNet 模型必须写入 `model` 分区。
2. 编译并完整烧录固件。烧录时使用实际串口，`flash` 会同时写入应用、分区表、bootloader 和 WakeNet 模型：

   ```bash
   idf.py build
   idf.py -p 实际串口 flash
   idf.py -p 实际串口 monitor
   ```

   也可以使用 USB Serial/JTAG 观察日志；不要用脚本反复读取 `/dev/cu.usbmodem*`，它可能在打开端口时复位设备。需要稳定的串口日志时，可使用 GPIO43/44 外接 USB-TTL。
3. 首次启动时连接设备热点 `weather-XXXXXX`，打开 `192.168.1.1`，保存 WiFi，并在同一页的“小智语音”里填写**服务信息地址（OTA）**，例如自建 Rust 服务的 `http://PC局域网IP:8003/xiaozhi/ota/`。这一项必填，留空设备不会连接任何语音服务。设备先完成第一次天气请求，随后才启动小智会话配置。
4. 在串口日志中等待类似下面的内容：

   ```text
   chat ready
   ```

   如果信息地址指向需要绑定/激活的服务（例如官方 xiaozhi.me 或上游 Python 智控台），日志还会先打印 `activation code: ......`，按提示在对应控制台完成激活后设备会自动重试。自建 `xiaozhi-server-rs` 不需要激活，不会打印激活码。
5. 当日志显示 `chat ready` 后，在设备附近说 **“小鹿小鹿”**。本地 WakeNet 检测到唤醒词后，设备会打开麦克风，把单声道 Opus 音频上传给小智；听到回答后通过 ES8311 和 NS415B 播放。
6. 每次唤醒对应一轮问答。当前单轮最多录音 15 秒；收到回答后自动停止播放并恢复等待下一次“小鹿小鹿”。空闲时不会上传麦克风音频。

常见日志含义：

- `voice unavailable`：语音任务没有启动，先检查 ES8311 初始化、PSRAM 和 `model` 分区。
- `WakeNet requires PSRAM`：固件没有检测到可用 PSRAM，确认 `CONFIG_SPIRAM`、`CONFIG_SPIRAM_MODE_OCT` 和模块型号/焊接。
- `service info URL not configured`：配网页没有填服务信息地址，固件不会连接任何语音服务。
- `server config unavailable`：填了地址但当前网络访问不到，或信息接口没有下发 WebSocket/激活信息；约 30 秒后重试。
- `wake word detected`：本地已经检测到“小鹿小鹿”，随后应看到 `listening`。
- `user:` / `bot:`：串口收到识别文本或回答文本。

当前版本的语音状态和激活码没有显示在墨水屏上；设备端 MCP 目前也没有注册天气、时间和现场温湿度工具，因此小智不能读取本项目的本地天气快照或传感器数据。短按按键仍然切换天气页面，长按 3 秒会清除 WiFi 凭据并进入配网，这会中断语音会话。

在 PC 自建管理控制台和语音服务，见 [本地小智部署指南](LOCAL_XIAOZHI.md)，包含 Apple Silicon 镜像构建、服务配置、设备绑定和本固件的兼容性边界。使用本地服务时，直接在 `SETUP-WIFI` 页面填写信息地址；不需要重新编译固件。


改引脚只动 `main/epd.h` 顶部的 `#define`。

画图坐标系是**逻辑横屏 300×128**（`EPD_W`×`EPD_H`），驱动在 `epd_pixel()` 里旋转 90° 映射到面板竖屏 RAM（`128×296`）。SSD1680 最多 296 条 gate，所以逻辑 `x >= 296` 会被裁掉；实际可见宽度是 296。画面上下颠倒就把 `EPD_ROT_CW` 改成 0。

## 编译烧录

```bash
. ~/.espressif/tools/activate_idf_v6.0.2.sh   # 本机 IDF v6.0.2（eim 安装）
idf.py build
idf.py -p /dev/cu.usbmodem* flash
```

`idf.py monitor` 要在真正的终端里跑；用脚本读 `/dev/cu.usbmodem*` 会一开端口就复位芯片，拿不到 app 日志（只有 ROM 几行）。要稳定看日志就接 USB-TTL 到 GPIO43/44。

`CONFIG_ESP_MAIN_TASK_STACK_SIZE` 是 **10 KB**（写在 `sdkconfig.defaults` 里）：出口 IP 定位和天气请求的 HTTPS 都在主任务里同步跑，4 KB 会在第一次 TLS 握手时栈溢出，症状是开机后十几秒 panic 重启、屏幕只停在启动时刷的空白页，串口只剩 `rst:0xc (RTC_SW_CPU_RST)`。

## 配置 WiFi（网页配网，不用改代码）

第一次上电（NVS 里还没有凭据时）板子会自己开热点：

1. 手机连 WiFi **`weather-XXXXXX`**（开放热点，无密码；后缀是这块板子 MAC 的后三字节，同时上电的几块板子不会撞名。系统会提示「网络不安全」，忽略即可）
2. 一般系统会自己弹出配网页（板子劫持了 DNS 并把探测请求 302 到配网页）；没弹就浏览器打开 **`192.168.1.1`**
3. 页面打开时会扫一次周边热点，WiFi 名称是个下拉框（也支持手动输入），选一个填密码 → **保存并连接**
4. 凭据存进 NVS，跳出配网页直接连；开机先画占位页并全刷（清掉上次断电留下的画面，同时让屏幕有内容可看），等 5 秒再连网，连上后 25 秒内取数据、对时、刷屏

屏幕在配网模式会显示这条提示。换网络：清一次 NVS（`idf.py erase-flash` 会连固件一起清掉，之后重新烧）或用同样的方法重新配。

已保存的凭据可以在**长按按键 3 秒**后清除（屏幕同时切换到配网提示页），再重新配网。WiFi 凭据仅通过网页配置并保存在 NVS 中，不再提供编译时默认值。未保存凭据时进入网页配网流程；已保存凭据但暂时连不上路由器时，按 5、10、20、40、60 秒的阶梯间隔重试（最多 1 分钟 1 次），不会自动开热点。已有的 NVS 配置不受固件更新影响。

## 数据从哪来

1. `http://ip-api.com/json/?fields=status,city,lat,lon` —— 用**出口 IP** 拿经纬度和城市，无需 API key。
2. `https://api.open-meteo.com/v1/forecast?latitude=..&longitude=..&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max&hourly=weather_code,temperature_2m,precipitation_probability&temperature_unit=celsius&timezone=auto&forecast_days=5&forecast_hours=21` —— 免费无 key，获取 5 天日预报和 21 条逐时数据；逐时页显示其中前 10 条，即当前当地小时到未来第 9 小时。统一使用摄氏度；`timezone=auto` 返回 `utc_offset_seconds`。不请求接口中的 `current` 温度或湿度。
3. 用这个偏移设置 POSIX `TZ`（`UTC+8` → `XXX-08:00`），再 SNTP 对时，`localtime_r()` 就是当地时间。
4. 正常每 30 分钟取一次；定位、天气请求或解析失败时，屏幕顶部显示 `无网络 5s后重试 #1`，之后按 5、10、20、40、60 秒阶梯递增（最高 1 分钟 1 次），连续失败时次数递增，成功后重置。已有天气则保留标题栏以下的旧数据；首次失败显示传感器和五天预报的占位布局。成功后清除重试提示并恢复 30 分钟间隔。不额外保存一份旧天气结构体；首次成功取得天气后才启动 SNTP 对时。

## 屏幕布局与传感器预留

- 顶部：城市、今天的出行提示与页面重绘时的当地时间；右上角时间使用独立的 **12px 粗体** 字模，保持原来的右对齐位置。城市超宽时安全省略，失败时整行替换为无网络重试提示，不继续展示旧的带伞建议。目前没有独立的每分钟时钟刷新，本次只调整字重，不改变刷新频率。
- 主体：五列等宽的日期轴、Weather Icons 天气图标、单行 `最低°-最高°` 区间和降水概率，例如 `12°-21°`；第一列固定显示 `今天`（16px，汉字只在 `font_mid` 里），其余列为 `MM/DD`。每列 52px，五列整体在 296px 可见宽度内左右各留 18px 居中排列，列内元素水平居中。温度单位为摄氏度并带度数符号，区间保持最低温在前；`最低°-最高°` 在 16px 下是 53px 放不进 52px 列，所以温度行用 14px 字模（47px），更宽的负温或三位数区间再自动缩小字号，仍无法容纳或数据无效时显示 `--`。图标根据每天的 WMO 天气码映射到 Weather Icons 的 1bpp 子集，覆盖晴、多云、阴、雾、毛毛雨、雨、雪、阵雨、阵雪、雷雨和冰雹；无预报或未知码显示 `?`，不冒充晴天。
- 底部：一行三块——传感器温度、湿度、右下角的当前状态。尚未接入硬件驱动时温湿度分别显示 `--.- °C` 和 `-- %`，不会用天气接口数值冒充现场读数，也不占用预报主体区域。状态是短文案：`正在联网` / `正在取数` / `同步时间` / `已更新` / `重试 #n`；取数和对时这种阻塞步骤会先刷一次状态，让人看到设备在做事。
- `main/ui.h` 的 `sensor_reading_t` 预留 `temperature_c`、`humidity_percent` 和 `valid`。后续驱动可填入实际采样值，当前入口保持 `valid=false`。未知、非有限值或越界湿度使用占位；数值固定使用底栏字号，无法容纳时显示占位。当前刷新节奏仍由天气更新驱动，接入传感器后可按实际采样需求再调整。

出门提示采用保守的全天参考规则，仅依据今天，不用明天的风险决定今天的建议：

- 四舍五入后的降水概率达到 **51%**：显示 `今日带伞`；即使天气图标是雨雪类，低于 51% 也不显示带伞提示。
- 雷雨或伴冰雹的雷雨：优先显示 `雷雨注意安全`，不把带伞当作雷电防护。
- 已知的晴/多云/阴/雾等天气且概率低：显示 `降水概率低`，不保证不用带伞。
- 没有有效预报、当天概率无效，或未知天气码且概率低：显示 `预报待更新`，未知概率显示 `--%`，不冒充 0%。

`precipitation_probability_max` 是**当天各时段中的最高降水概率**，不是“现在正在下雨”，也不是某个出门时刻的分钟级预测。百分比显示和 **51%** 阈值判断使用相同的四舍五入结果；阈值是本项目的提醒策略，不是天气服务给出的确定结论。

生成与固件使用相同绘图代码、字模和单色帧缓存的布局预览（只需 Python 标准库和本机 C 编译器）：

```bash
python3 tools/preview_ui.py
# 可选：启用 AddressSanitizer / UBSan
python3 tools/preview_ui.py --sanitize
```

默认输出 `build/preview-5day.png`（三倍最近邻放大）、`build/preview-5day-native.png`（296×128 原尺寸）和 `build/preview-5day-hourly.png`，另生成有效传感器、断网重试、低降水概率、预报待更新及雷雨提示等状态的图片。**预览中的日期与预报数值为演示数据，不是实时天气；默认温湿度保持未知占位。**

## 代码结构

保留 `main/`：这是 ESP-IDF 默认的应用源码目录。改成 `src/` 不会减少模块或依赖，反而需要显式注册目录，并修改字体生成工具的路径；本项目没有这层自定义配置的必要。

- `main/epd.c` / `main/epd.h` — 驱动。ESP-IDF 核心 API：`driver/spi_master.h`（SPI2，4MHz，CS 手动拉低跨命令）、`driver/gpio.h`（RST/DC/BUSY）；4736 字节单色帧缓存；绘图原语（像素/线/填充矩形/实心圆/字体/全屏/局部刷新）。
- `main/fonts_aa.c` + `main/fonts_aa.h` — **1bpp 单色文字字模**：拉丁字母来自 Roboto（Apache 2.0），界面上的汉字来自 Noto Sans SC（OFL），汉字只带 `main/main.c`、`main/ui.c` 和 `main/weather.c` 字符串里真用到的那些（`genfont.py` 自动扫，加文案不用改字表）；度符号 `°`（U+00B0）也由 `genfont.py` 补进所有字号。46px Bold / 16px Medium / 14px Medium（`font_temp`，五天温度行专用）/ 12px Regular。汉字只带在 16px 的 `font_mid` 里（12px 汉字掉笔画），所以中文一律用 `font_mid`。右上角时间使用独立的 12px Bold `font_clock`（`main/font_clock.c`），不改变其他 12px 文本。字模用 FreeType 的 mono 目标（`mode='1'`）栅格化，笔画网格对齐到整像素后再存 1bpp，屏上直接位图对位输出。
- `main/font_icons.c` + `main/font_icons.h` — **Weather Icons 1bpp 图标子集**：按天页使用 advance 40、cell 高 27 的 `font_icon_daily`，逐时页使用 advance 26、cell 高 23 的 `font_icon`；共 11 类天气图标，运行时不缩放，不在 `ui.c` 手绘图形。字体和生成位图使用 SIL OFL 1.1，来源、固定提交、SHA256 和完整许可见 `third_party/weather-icons/README.md`。
- `tools/genfont.py` — 用 Pillow 从 TTF 重新生成 `fonts_aa.c`；`--only clock` 单独生成 `main/font_clock.c`，不需要 CJK 字体。生成输入文件不进仓库；Roboto 的许可证见 `third_party/roboto-clock/OFL.txt`。

  ```bash
  curl -LO 'https://github.com/google/fonts/raw/main/ofl/roboto/Roboto%5Bwdth,wght%5D.ttf'
  curl -LO 'https://github.com/google/fonts/raw/main/ofl/notosanssc/NotoSansSC%5Bwght%5D.ttf'
  FONT_TTF='Roboto[wdth,wght].ttf' FONT_CJK_TTF='NotoSansSC[wght].ttf' python3 tools/genfont.py
  ```

  只重新生成时钟（Roboto 3.015、SIL OFL 1.1；来源与固定版本见 `third_party/roboto-clock/README.md`）：

  ```bash
  FONT_TTF='.cache/fonts/Roboto[wdth,wght].ttf' python3 tools/genfont.py --only clock
  ```

- `tools/genicons.swift` — 用固定提交的 Weather Icons TTF 生成 `main/font_icons.c`，不在固件运行时加载字体。需要 macOS Swift 的 CoreGraphics、CoreText、CryptoKit 和 ImageIO；字体文件放在 `.cache/weather-icons/`，来源、许可、SHA256 和图标映射见 `third_party/weather-icons/README.md`：

  ```bash
  mkdir -p .cache/weather-icons
  curl -fL 'https://raw.githubusercontent.com/erikflowers/weather-icons/bb80982bf1f43f2d57f9dd753e7413bf88beb9ed/font/weathericons-regular-webfont.ttf' \
      -o .cache/weather-icons/weathericons-regular-webfont.ttf
  swift tools/genicons.swift .cache/weather-icons/weathericons-regular-webfont.ttf main/font_icons.c \
      .cache/weather-icons/preview.png
  ```

- `managed_components/espressif__cjson/` — 由 ESP-IDF Component Manager 自动下载的 cJSON 组件（1.7.x）；固件和主机回归共用这份源码。首次运行主机解析回归前，先执行 `idf.py reconfigure` 或 `idf.py build`。
- `main/main.c` — 应用入口：初始化、配网、取天气、对时和定时刷新，不包含绘图细节。
- `main/audio.c` / `main/audio.h` — ES8311 + NS415B 音频初始化：GPIO2 功放使能、GPIO47/48/38/39/40 I²S、GPIO41/42 I²C；提供麦克风读取、扬声器播放、音量/增益和静音接口，使用官方 `esp_codec_dev` 组件。
- `main/ui.c` / `main/ui.h` — 以五天天气图标、高低温及降水概率为主体，图标由 Weather Icons 字模按 WMO 码映射，顶部提供今日出行提示，传感器读数/占位放在单行底栏，另含配网和无网络重试提示；重试只覆盖顶部标题栏，保留旧正文。只修改帧缓冲，由入口决定何时刷新墨水屏。
- `main/net.c` — WiFi 连接、SNTP、时区、HTTP(S) GET（`esp_crt_bundle` 校验证书）。
- `main/net_portal.c` — SoftAP 配网生命周期：AP/IP 配置、WiFi 模式、DNS/HTTP 服务启停。
- `main/portal_web.c` / `main/portal_web.h` — 配网页 HTTP 服务：热点扫描、凭据保存、模板渲染和保存事件同步。
- `main/web/` — 配网页独立源文件：`index.html`、`result.html`、`style.css`、`app.js`。通过 `main/CMakeLists.txt` 的 `EMBED_TXTFILES` 编译进固件，不需要文件系统或前端构建工具。服务端替换 `{{TOKEN}}` 并分块发送，CSS/JS 仍内联在 HTML 响应中，避免保存后热点关闭导致资源加载失败。热点扫描、SSID 转义与普通表单提交仍由 C 处理；重新扫描仍刷新页面，手填 SSID 优先。修改网页后重新构建和烧录即可，不需要生成墨水屏字模。
- `main/button.c` / `main/button.h` — 单个低电平按键：2 次采样去抖，短按切页，长按 3 秒清掉 NVS 里的 WiFi 凭据并回到配网页。
- `components/dns_server/` — 复用 ESP-IDF captive portal 示例的 DNS server 组件，负责 DNS 协议处理和将 A 查询指向 SoftAP。
- `main/portal_dns.c` / `main/portal_dns.h` — 配网 DNS 组件的设备特定启动配置，不自行实现 DNS 协议。
- `main/weather.c` / `main/weather.h` — IP 定位 + 五天天气预报，使用 Component Manager 提供的 `espressif/cjson` 解析 JSON。只从 `daily.time`、`daily.weather_code`、`daily.temperature_2m_max`、`daily.temperature_2m_min` 和 `daily.precipitation_probability_max` 读取五天数据。逐日验证日期、数组长度、0–99 整数天气码、0–100% 有限降水概率、温度类型及高低温关系；必需数据缺失、为 `null`、日期不连续或响应截断时视为失败，不用后续字段冒充预报数据。
- `main/weather_json.c` / `main/weather_json.h` — 天气解析使用的通用 cJSON 辅助函数：严格整段解析（拒绝尾部内容）、有限数值/整数、定长字符串、对象成员字符串与固定长度数组读取；日期连续性和值域等天气数据契约校验仍在 `weather.c`。

## 驱动要点（踩过的坑）

- 面板是**竖屏 128×296**：SSD1680 最多 296 条 gate，`0x01` 写 `0x27 0x01 0x00`，每行 16 字节，X 窗口 `0x00..0x0F`，Y 窗口 `0x00..0x27 0x01`，帧缓存 4736 字节。**不要写 300 行（`0x2B`/4800 字节）**：多出的 4 行会回绕，表现为右侧像素跑到左侧，局部刷新时尤其明显。
- B/W 面板要**主机下发 159 字节波形表**（`0x32` 写前 153 字节，再 `0x3F/0x03/0x04/0x2C` 写 6 个电压值）；不写 LUT 画面出不来。
- BUSY **高电平 = 忙**；所有等待都有超时（初始化 2s、刷新 15s），超时只打日志不卡死。
- 刷新序列：`0x24` 写 4736 字节 → `0x22 0xC7`（全刷）或 `0x20` → 等 BUSY。局部刷新按 Waveshare V2 官方流程：硬件复位 + `0x32` 下发 153 字节 partial LUT + `0x37`/`0x3C 0x80` + `0x22 0xC0` 预置 → 写图 → `0x22 0x0F` 显示。
- 旋转只在 `epd_pixel()` 做一次，所有绘图原语、文字、图标都自动跟着横屏。
- 上电先画占位页（`无数据` / `预报待更新` / `--`）并用 `epd_refresh_full()` 全刷：墨水屏断电后仍保留上次的画面，而第一帧天气要等连网和请求完成，所以不能留白屏。这一步不消耗普通刷新计数，后续取数、状态和重试走局部刷新；按键切页前 7 次使用局部刷新，第 8 次切页做一次全刷去残影并清零切页计数。驱动每 `FULL_REFRESH_EVERY` 次普通刷新另做一次维护性全刷，其余使用官方 partial LUT，不整屏黑白翻转，日常更新基本不闪。正常每 30 分钟取数并刷新；失败时刷新无网络提示和下一次重试序号，按 5、10、20、40、60 秒阶梯再尝试，每次尝试结束只刷新一帧，不做每秒倒计时。等待时间不包含 HTTP 请求、对时和刷屏耗时。恢复成功后重画正常界面，时间也随成功刷新更新；没有“数值相同就跳过”的判断。

## 回归验证

```bash
python3 tools/test_app.py
python3 tools/test_portal.py
```

主机回归只需要 Python 标准库和本机 C 编译器，但天气解析和应用回归会直接编译 Component Manager 已下载的 cJSON 源码；首次运行前先执行 `idf.py reconfigure` 或 `idf.py build`。测试真实编译应用入口、UI、天气解析、绘图代码和 Weather Icons 字模，模拟网络与硬件输入。应用场景检查配网、开机 5 秒等待、连接阶梯退避（5/10/20/40/60 秒）、持续失败与恢复、重试次数、首次成功对时、完整帧及旧正文保留。直接解析测试覆盖五天全部日期/高低温、单位与作用域、跨月年/闰年、数组长度、无效数值与截断 JSON；UI 测试检查每列日期、Weather Icons 字形、WMO 映射、图标边界/隔离、高低温/降水概率、未知天气与概率、带伞建议和舍入阈值、雷雨优先级、今日与明日隔离、传感器底栏与预报主体隔离、字模和布局边界。可用 `--only weather`、`--only ui` 或 `--only app` 单独运行一组，生成物均放在系统临时目录。

可用 `--baseline` 指定旧 `main/` 的源码目录，对两个版本的请求、时区调用和完整帧缓冲逐字节比较。比较时应保持五日数据契约、界面与字模一致；旧的单日/current 数据结构或旧重试行为不兼容当前回归。直接解析测试只针对当前版本运行。测试不覆盖真实 `net.c` / `net_portal.c` / `portal_web.c` / `portal_dns.c` 的 WiFi、网页、DNS、事件同步及硬件时序，这些仍需真机回归。

`tools/test_portal.py` 单独编译 `portal_web.c` 中真实的网页渲染函数，并以真实网页文件提供嵌入内容；检查热点选项、特殊字符转义、模板替换、内联资源、结果页及分块发送错误传播。扫描和 HTTP 为 stub，不执行浏览器 JS，也不覆盖凭据保存、真实网络和硬件行为；资源嵌入及链接需另跑 `idf.py build`，配网交互仍需手机实测。

本轮精简的依据与消融结果见 [ABLATION.md](ABLATION.md)。
