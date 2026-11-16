# 在本地 PC 运行小智智控台和语音服务

本指南使用社区项目 [xinnan-tech/xiaozhi-esp32-server](https://github.com/xinnan-tech/xiaozhi-esp32-server)，与本仓库的 `espressif/esp_xiaozhi` 客户端通过小智协议连接。它是可自建的服务端，不是把 xiaozhi.me 的官方后台复制到本机。

## 运行哪些服务

完整智控台需要以下模块；只运行 Python Server 不包含完整管理网页。

| 模块 | 用途 | Docker 全模块部署的访问地址 |
| --- | --- | --- |
| manager-web + manager-api | 登录、智能体、设备绑定、模型配置、信息接口 | PC 浏览器访问 `http://localhost:8002` |
| xiaozhi-server | WebSocket、语音识别、大模型调用、语音合成 | ESP32 访问 `ws://PC局域网IP:8000/xiaozhi/v1/` |
| MySQL | 用户、设备、模型和系统配置 | Compose 内部网络 |
| Redis | 缓存、激活和运行状态 | Compose 内部网络 |
| ASR / LLM / TTS | 语音转文字、生成回答、文字转语音 | 由智控台选择本地模型或云 API |

推荐先使用 WebSocket，暂不部署额外的 MQTT/UDP 网关。ESP32 与 PC 接同一个可互访的局域网，PC 保持开机、服务运行且不休眠。

“服务端运行在本地”不等于“模型全部离线”。例如本地 FunASR + 云 LLM + EdgeTTS 仍会把识别文本、合成文本发给对应服务商。云 ASR 还会上传语音。要完全本地处理，必须同时选择本地 ASR、LLM、TTS；天气面板本身仍会访问天气接口。

## 1. 准备上游源码和运行目录

将服务端作为独立项目放在 PC 上，不要复制到本仓库的 `components/` 或 `managed_components/`。

```bash
git clone --branch v0.9.8 --depth 1 https://github.com/xinnan-tech/xiaozhi-esp32-server.git
cd xiaozhi-esp32-server
```

后续镜像构建命令在这个上游项目根目录执行。

上游全模块部署文档注明，自 0.8.2 起预制全模块镜像只支持 x86。Intel/AMD PC 可以按上游 Docker 全模块教程使用发行镜像；Apple Silicon 优先尝试原生 ARM64 构建。ARM64 是否能构建成功，还取决于基础镜像和 Python 依赖的 ARM64 支持，不能仅给 x86 镜像加 `platform: linux/arm64`。

## 2. Apple Silicon 构建 ARM64 镜像

上游 `Dockerfile-server` 依赖预制的 `server-base`，因此先构建本地基础镜像，再让应用镜像使用它：

```bash
docker build --platform linux/arm64 -f Dockerfile-server-base -t xiaozhi-server-base:0.9.8-arm64 .
sed 's|^FROM .*|FROM xiaozhi-server-base:0.9.8-arm64|' Dockerfile-server > Dockerfile-server.local
docker build --platform linux/arm64 -f Dockerfile-server.local -t xiaozhi-server:0.9.8-arm64 .
docker build --platform linux/arm64 -f Dockerfile-web -t xiaozhi-web:0.9.8-arm64 .
```

首次构建会下载基础镜像、Python 包、npm 包和 Maven 依赖，可能耗时较长，并占用数 GB 磁盘。若遇到 `no matching manifest for linux/arm64` 或 Python 包没有 ARM64 wheel，需要针对报错的基础镜像/依赖适配；另一条上游支持的路线是本地源码运行：Python 3.10 + libopus + ffmpeg、JDK 21 + Maven、Node.js、MySQL、Redis。不要混用 ESP-IDF 的 Python 环境安装服务端依赖。

## 3. 准备 Compose、配置和 ASR 模型

进入上游的服务端目录：

```bash
cd main/xiaozhi-server
mkdir -p data models/SenseVoiceSmall
cp config_from_api.yaml data/.config.yaml
curl -fL --retry 3 https://modelscope.cn/models/iic/SenseVoiceSmall/resolve/master/model.pt -o models/SenseVoiceSmall/model.pt
```

默认本地语音识别使用 SenseVoiceSmall，模型文件较大。这个 PC 端 ASR 模型与 ESP32 的 WakeNet 模型是两份不同的模型，设备端仍须烧录自己的 `model` 分区。

在当前目录创建 `docker-compose.local.yml`，用本地构建镜像覆盖上游两个应用镜像，并统一数据库密码：

```yaml
services:
  xiaozhi-esp32-server:
    image: xiaozhi-server:0.9.8-arm64
  xiaozhi-esp32-server-web:
    image: xiaozhi-web:0.9.8-arm64
    environment:
      SPRING_DATASOURCE_DRUID_PASSWORD: ${XIAOZHI_DB_PASSWORD:?请在.env中设置数据库密码}
  xiaozhi-esp32-server-db:
    image: mysql:8.4
    environment:
      MYSQL_ROOT_PASSWORD: ${XIAOZHI_DB_PASSWORD:?请在.env中设置数据库密码}
```

在同一目录创建不提交的 `.env`，填入自己生成的数据库密码：

```dotenv
XIAOZHI_DB_PASSWORD=填入你生成的数据库密码
```

仅在创建全新数据库时设置初始密码；已有 MySQL 数据目录时，修改 `.env` 不会自动修改数据库中的密码。

先检查 Compose，再只启动智控台及其数据库依赖：

```bash
docker compose -f docker-compose_all.yml -f docker-compose.local.yml config --quiet
docker compose -f docker-compose_all.yml -f docker-compose.local.yml up -d xiaozhi-esp32-server-web
docker compose -f docker-compose_all.yml -f docker-compose.local.yml logs --tail 100 xiaozhi-esp32-server-web
```

不要把真实 API Key、数据库密码或 `server.secret` 提交到 Git。上游 Compose 的数据库和 Redis 没有发布到宿主机端口，保持这一默认即可。

## 4. 初始化智控台

PC 浏览器打开 `http://localhost:8002`。按上游流程注册第一个用户，该用户是管理员；然后配置：

| 位置 | 配置内容 |
| --- | --- |
| 参数管理 → `server.secret` | 复制自动生成的值，供 Python Server 调用管理 API |
| 参数管理 → `server.websocket` | `ws://PC局域网IP:8000/xiaozhi/v1/` |
| 参数管理 → `server.ota` | `http://PC局域网IP:8002/xiaozhi/ota/` |
| 参数管理 → `server.mqtt_gateway` | 此方案保持未配置，不下发 MQTT |
| 模型配置 | 选择 ASR、LLM、TTS，并填写所选云服务所需的密钥或本地服务地址 |
| 智能体管理 | 创建/配置智能体、提示词、音色和模型，供设备绑定 |

在 macOS 的“系统设置 → 网络 → 当前连接 → 详细信息 → TCP/IP”中查看 PC 的局域网 IPv4；建议在路由器中做 DHCP 地址保留。

例如 PC 的地址为 `192.168.1.100` 时，两个设备端接口为：

```text
http://192.168.1.100:8002/xiaozhi/ota/
ws://192.168.1.100:8000/xiaozhi/v1/
```

这是示例 IP，必须替换成实际地址。设备端不要填 `localhost`、`127.0.0.1`、`0.0.0.0` 或 Docker 内部地址。ESP32 连家庭 WiFi 后使用家庭局域网，`192.168.1.1` 配网热点地址不是 PC 服务地址。

将 `data/.config.yaml` 的 `manager-api` 改成以下内容；`secret` 填入控制台生成的真实值，保留模板中其余服务设置：

```yaml
manager-api:
  url: http://xiaozhi-esp32-server-web:8002/xiaozhi
  secret: "填入控制台生成的server.secret"
```

这里的 Docker 服务名只供容器之间通信；它不能用在 ESP32 的接口配置中。启用 manager-api 模式后，模型设置应在智控台修改，不要仅修改 Python 默认 `config.yaml`。

## 5. 启动语音服务

```bash
docker compose -f docker-compose_all.yml -f docker-compose.local.yml up -d xiaozhi-esp32-server
docker compose -f docker-compose_all.yml -f docker-compose.local.yml ps
docker compose -f docker-compose_all.yml -f docker-compose.local.yml logs --tail 100 xiaozhi-esp32-server
```

先确认控制台能登录、模型配置有效、语音服务能取得管理 API 配置。防火墙允许 ESP32 所在局域网访问 TCP `8002` 和 `8000`，路由器/AP 不启用客户端隔离。局域网初次联调可用 HTTP/WS，这两个协议不加密；公网部署另行配置 HTTPS/WSS。

## 6. 让本项目固件连接 PC

不需要重新编译来切换服务地址。烧录当前固件后，设备进入 `SETUP-WIFI` 页面，在“小智语音”中选择“官方服务”或“自建服务”。选择自建服务时填写：

- 全模块服务的信息地址：`http://PC局域网IP:8002/xiaozhi/ota/`
- 仅语音 Server 的信息地址：`http://PC局域网IP:8003/xiaozhi/ota/`
- 可选 WebSocket 覆盖：`ws://PC局域网IP:8000/xiaozhi/v1/`

WebSocket 留空时，设备使用信息接口返回的地址；设备不会把模型 API Key 或 `server.secret` 保存到 NVS。设备不能填写 `localhost`，必须填写 PC 在家庭局域网中的地址。

官方服务模式仍使用编译时的 `CONFIG_XIAOZHI_OTA_URL` 默认值。项目自身用 SNTP 和 `TZ` 管理时间，组件的 `XIAOZHI_SYNC_SYSTEM_TIME_FROM_SERVER` 保持默认也不会改变天气页面的时区设置；如果需要让项目 SNTP 作为唯一时间源，可以在 `menuconfig` 中关闭它。然后完整构建、烧录一次即可：

```bash
idf.py build
idf.py -p 实际串口 flash
idf.py -p 实际串口 monitor
```

当前固件通过信息接口获取激活信息和协议配置；自建模式下会使用配网页保存的信息地址，并强制选择 WebSocket。可选 WebSocket 地址会覆盖服务端下发的地址，令牌和协议版本仍由信息接口保存/下发。若使用本指南的 WebSocket 部署，不要在控制台配置 MQTT 网关。

## 7. 绑定设备和验证问答

1. ESP32 配好 WiFi 后会向本地信息接口发送设备 MAC、UUID 和板卡信息。
2. 串口出现 `activation code: ...` 后，在本地智控台的智能体/设备页面输入该码完成绑定；这里使用的是自建控制台，不再去 xiaozhi.me 输入。
3. 绑定后重启设备，再观察串口的 `chat ready`。自建服务可能在未绑定时也返回 WebSocket 配置，因此 `chat ready` 本身不能证明设备已经绑定。
4. 在设备附近说“小鹿小鹿”，检查 `wake word detected`、`listening`、`user:`、`bot:` 和扬声器播放。自建服务的 `wakeup_words` 配置也应包含 `小鹿小鹿`。先用一句简短问题测试。

当前固件在收到 TTS 开始事件时停止本轮录音；上游可能先对唤醒词发送问候 TTS，因此需要检查是否只播放问候、未处理后续问句。简单把 `enable_greeting` 设成 false 也不能保证兼容：上游可能仍发送 TTS stop，当前应用同样会把它当成本轮结束。遇到这个问题，需要针对双方的唤醒/聆听时序适配并真机验证，不能仅凭服务启动或绑定成功宣称已能完整对话。

固件的音频基准是 Opus / 16000 Hz / 单声道 / 60 ms 帧；核对服务端实际音频参数，尤其是其默认配置中出现的 24000 Hz。Opus 解码器可按输出采样率解码，但服务端的分帧、重采样及声明参数仍需与这条链路一起验证。当前也未做播放时语音打断。

本项目尚未注册设备端天气、时间、温湿度 MCP 工具；PC 上开启天气插件不会自动读取 ESP32 的 `shown` 快照或本地传感器。

## 选择模型

| 目标 | 组合建议 | 网络与资源 |
| --- | --- | --- |
| 先验证系统 | 本地 FunASR + 云 LLM + EdgeTTS/云 TTS | PC 不需本地跑大模型，但仍依赖互联网，云服务可能收费 |
| 大模型在 PC | 本地 FunASR + Ollama/LM Studio + 云 TTS | 文本推理在 PC，TTS 仍联网 |
| 全部语音链路本地 | 本地 ASR + Ollama/LM Studio + 本地 TTS | 各模型需独立安装、下载和配置，资源消耗取决于模型 |

Docker 内的模型适配器访问宿主机 Ollama 时，通常使用 `http://host.docker.internal:11434`；OpenAI 兼容接口通常使用带 `/v1` 的 Base URL，按所选适配器要求填写。Ollama 需允许来自 Docker 的连接，不能仅监听容器无法访问的回环接口。原生 macOS 运行 Ollama/LM Studio 可利用 Apple GPU；不要假设 Docker 容器能直接使用 Apple GPU。

作为单设备实验的起点，可给 Docker 预留约 6–8 GB 内存；这不是性能保证，也不含本地 LLM/TTS 的需求。全本地模型建议在明确 PC 内存和目标模型后再定规模，较大的模型更适合 16–32 GB 或以上内存。

## 排查顺序

- 网页打不开：检查 Docker Desktop、`ps`、MySQL/Redis health 和 web 日志。
- 管理 API 鉴权失败：核对 `server.secret` 和容器内部的 `manager-api.url`。
- ESP32 仍访问官方地址：核对实际生效的 `sdkconfig`、重新构建和烧录。
- 浏览器打开 OTA 地址能返回内容，但设备失败：查看真正的设备 POST 响应和服务器日志；浏览器 GET 不等于设备注册成功。上游 v0.9.8 的 GET 健康提示还会检查 MQTT 参数，单用该提示不足以判定 WebSocket 方案失败。
- 绑定码看不到/过期：先看串口；当前激活码不显示在墨水屏。重启设备重新获取，不擦除整个 NVS。
- 能绑定但不能对话：分开检查模型密钥、唤醒问候时序、ASR 识别、TTS 和音频参数。
- 改完控制台配置仍用旧值：重启 Python Server 和设备，让配置重新加载。

## 上游参考

- [全模块部署](https://github.com/xinnan-tech/xiaozhi-esp32-server/blob/v0.9.8/docs/Deployment_all.md)
- [Docker 镜像构建](https://github.com/xinnan-tech/xiaozhi-esp32-server/blob/v0.9.8/docs/docker-build.md)
- [仅运行语音 Server](https://github.com/xinnan-tech/xiaozhi-esp32-server/blob/v0.9.8/docs/Deployment.md)
