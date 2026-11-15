# Weather Icons 字模来源与许可

本目录记录 `main/font_icons.c` 中两套 1bpp 位图子集的来源及许可。原始 TTF 不提交；正常固件构建只使用生成的 C，不运行 Swift、不下载字体，也不依赖 `.cache/`。`main/ui.c` 已使用这两套字模，`main/CMakeLists.txt` 已将 `font_icons.c` 编入固件。

## 固定来源与版权声明

- 项目：Erik Flowers 的 [Weather Icons](https://github.com/erikflowers/weather-icons)。
- 固定提交：`bb80982bf1f43f2d57f9dd753e7413bf88beb9ed`，不是浮动分支或标签。
- 字体：[font/weathericons-regular-webfont.ttf](https://raw.githubusercontent.com/erikflowers/weather-icons/bb80982bf1f43f2d57f9dd753e7413bf88beb9ed/font/weathericons-regular-webfont.ttf)，99,564 字节。
- 码点依据：[css/weather-icons.css](https://raw.githubusercontent.com/erikflowers/weather-icons/bb80982bf1f43f2d57f9dd753e7413bf88beb9ed/css/weather-icons.css)，30,727 字节。
- 作者及许可交叉依据：[上游 README 的 Credit / Licensing](https://github.com/erikflowers/weather-icons/blob/bb80982bf1f43f2d57f9dd753e7413bf88beb9ed/README.md)。初版图标设计来自 Lukas Bischoff；v1.1 起的图标艺术由 Erik Flowers 提供。

已读取原始 TTF 的 `name` 表，而非仅依据仓库代码许可：

| 字段 | 原文 |
| --- | --- |
| name ID 0，copyright | `Weather Icons licensed under SIL OFL 1.1 — Code licensed under MIT License — Documentation licensed under CC BY 3.0` |
| name ID 9，designer | `Erik Flowers, Lukas Bischoff (v1 Art)` |
| name ID 12，designer URL | `http://www.helloerik.com, http://www.artill.de` |
| name ID 6，PostScript name | `WeatherIcons-Regular` |
| name ID 5，version | `Version 1.100;PS 001.100;hotconv 1.0.70;makeotf.lib2.5.58329` |

**原字体及生成的位图子集使用 SIL Open Font License 1.1，不是 MIT。** 上游 CSS/代码为 MIT，文档为 CC BY 3.0；本项目只用 CSS 核对映射，不将 CSS 嵌入固件。TTF 的版本元数据与 CSS 注释中的 Weather Icons 2.0.10 是不同字段，不应混写。

[OFL.txt](OFL.txt) 保留上述字体版权字段、作者和网址，并附完整 OFL 1.1 英文正文。正文来源为 [SIL 官方许可文本](https://openfontlicense.org/open-font-license-official-text/)。已检查的 TTF `name` 表和上游 README 未给出版权年份或 Reserved Font Name 声明，因此这里不臆造年份、版权声明或保留名称。分发这些字模时应同时保留本来源说明与完整许可。

## SHA256

```text
176bda6661f213dde47c2114d76e476ec8ca9aae07dd54f9550d2d28fe02b4fd  weathericons-regular-webfont.ttf
c701c2e292c956680294db0dc15c954ffabb6cd9c7c6fcb0347913b5f31991f6  weather-icons.css
579da6eb064480938c83d2caeb786d912f927b74dfd544456b1a2e4d51f6634f  main/font_icons.c
```

生成器会强制检查 TTF SHA256，并直接从该文件的字节创建 CGFont，避免误用本机已安装的同名字体。C 文件的散列对应下述生成环境；CoreGraphics 版本变更后应重新检查输出和预览，而不是假设跨系统像素必然一致。

## 图标选择

Header 中的字符串均为 C UTF-8 十六进制转义，不含字面 PUA 字符。下表也是预览从左到右的顺序；生成的 C 码点表、glyph 表则严格按码点升序排列。

| 类别 / 宏 | 上游 CSS 官方名称 | 码点 | daily 墨迹 w×h | hourly 墨迹 w×h |
| --- | --- | --- | --- | --- |
| sun / `WI_SUN` | `wi-day-sunny` | U+F00D | 25×25 | 20×20 |
| partly_cloudy / `WI_PARTLY_CLOUDY` | `wi-day-cloudy` | U+F002 | 31×25 | 26×20 |
| cloud / `WI_CLOUD` | `wi-cloudy` | U+F013 | 26×17 | 21×14 |
| fog / `WI_FOG` | `wi-fog` | U+F014 | 29×21 | 24×17 |
| drizzle / `WI_DRIZZLE` | `wi-day-sprinkle` | U+F00B | 31×27 | 26×22 |
| rain / `WI_RAIN` | `wi-rain` | U+F019 | 24×23 | 20×19 |
| snow / `WI_SNOW` | `wi-snow` | U+F01B | 24×23 | 20×19 |
| showers / `WI_SHOWERS` | `wi-showers` | U+F01A | 24×24 | 20×20 |
| snow_showers / `WI_SNOW_SHOWERS` | `wi-day-snow` | U+F00A | 27×27 | 23×23 |
| thunder / `WI_THUNDER` | `wi-thunderstorm` | U+F01E | 24×24 | 20×20 |
| hail / `WI_HAIL` | `wi-hail` | U+F015 | 24×24 | 20×19 |

`U+F01D` 实际是 `wi-storm-showers`，不能当作阵雪。阵雪使用带太阳、云和雪的 `wi-day-snow`，与普通 `wi-snow` 区分；这是本项目的天气类别映射，并非上游另有名为 snow-showers 的字形。毛毛雨保留原有 `wi-day-sprinkle`。所有图形均来自原字体轮廓，没有手画补线或组合生成天气图标。

## 栅格化与接入契约

| aafont | 字号 | 固定 advance | cell / line_height | 位图数据 |
| --- | --- | --- | --- | --- |
| `font_icon_daily` | 22pt，day-snow 单独 19pt | 40 | 27 | 922 B |
| `font_icon` | 18pt，day-snow 单独 16pt | 26 | 23 | 681 B |

`wi-day-snow` 在 22pt 下墨迹高 31px，不能直接塞进 27px 单元格；只对该字形选择较小的生成字号，不裁切、不在运行时缩放。其余字形保持 22pt / 18pt。

- daily：`xoff=(40-w)/2`，`yoff=-27+(27-h)/2`；baseline 73 时单元格覆盖 y=46..72。
- hourly：`xoff=(26-w)/2`，`yoff=-23+(23-h)/2`；baseline 69 时单元格覆盖 y=46..68。
- 均为非负差值的整数除法。奇数留白多出的一个像素放右侧或底部。
- 位图为 1bpp、MSB 在左、每行补齐到整字节、由上至下，1 表示墨迹。CoreGraphics 抗锯齿关闭，并检查源像素只能为 0 或 255。
- 先在带留白的独立画布绘制完整字体轮廓，再裁去空白；检查墨迹未碰画布边缘、裁后尺寸不超 cell。生成过程不接受超限字形。
- 原实现对内存行做 `maxY-row` 反转，导致图标倒置。实际位图内存行已是图像自上而下顺序，改为 `minY+row`。不能仅凭“字体路径坐标 Y 向上”推断内存行方向。
- 两套各 11 个 glyph，均检查非空且栅格化结果互不相同。合计位图 1,603 B；另有 22 个 `aaglyph` 和 22 个 `uint16_t` 码点。在 `sizeof(aaglyph)=8` 时，位图与这两类表合计 1,823 B，不含两个 `aafont` 描述符及链接对齐开销。C 源文件为 10,564 B，不能当作固件占用。

## 重生成命令

在项目根目录运行。需要 macOS 的 Swift、CoreGraphics、CoreText、CryptoKit、ImageIO；不需要 Pillow 或其他第三方包。首次下载需要网络，后续生成本身离线。

目录通过编辑器代理的 `create_directory` 工具创建为 `.cache/weather-icons/`；手动运行时先保证该目录存在。TTF、CSS、预览及验证产物只放已被 `.gitignore` 排除的 `.cache/weather-icons/`。

```sh
curl -fL --max-time 60 https://raw.githubusercontent.com/erikflowers/weather-icons/bb80982bf1f43f2d57f9dd753e7413bf88beb9ed/font/weathericons-regular-webfont.ttf -o .cache/weather-icons/weathericons-regular-webfont.ttf
curl -fL --max-time 60 https://raw.githubusercontent.com/erikflowers/weather-icons/bb80982bf1f43f2d57f9dd753e7413bf88beb9ed/css/weather-icons.css -o .cache/weather-icons/weather-icons.css
shasum -a 256 .cache/weather-icons/weathericons-regular-webfont.ttf .cache/weather-icons/weather-icons.css

swift tools/genicons.swift .cache/weather-icons/weathericons-regular-webfont.ttf main/font_icons.c .cache/weather-icons/preview.png
shasum -a 256 main/font_icons.c
open .cache/weather-icons/preview.png
```

最后一个 PNG 参数可省略，保留原生成器的 `输入.ttf 输出.c` 调用方式。预览直接解码与 C 相同的打包字节及 glyph offset，不另用字体渲染一套图标。上排 daily，下排 hourly，列顺序见上表；灰色 cell 边框只用于检查，不写入字模，像素以最近邻方式放大 4 倍。

可不覆盖源文件地验证重生成一致性：

```sh
swift tools/genicons.swift .cache/weather-icons/weathericons-regular-webfont.ttf .cache/weather-icons/regenerated.c
cmp main/font_icons.c .cache/weather-icons/regenerated.c
cc -std=c11 -Wall -Wextra -Werror -Imain -c main/font_icons.c -o .cache/weather-icons/font_icons.o
```

## 已做的字模验证

生成环境：Apple Swift 6.4（swiftlang-6.4.0.34.1），arm64-apple-macosx26.0。

- 11 个宏的 UTF-8 解码值与固定提交 CSS 的官方名称/码点逐项核对。
- 两套共 22 个 glyph：码点有序、偏移连续、固定 advance、居中公式、cell 边界、行尾补位、非空及同套互不重复检查。
- `cc -std=c11 -Wall -Wextra -Werror` 编译真实 C/header，运行主机断言检查两套 `aafont` 和全部 11 个字符串宏。
- 可选 PNG 与从生成 C 数组独立解码的 `.cache/weather-icons/preview-from-c.png` 均已人工查看：太阳在云上方，降水/雪/冰雹位于云下方；未继续使用未经验证的 Y 轴反转。
- 同一环境重复生成，与提交用 C 文件逐字节一致。

这些检查不等价于墨水屏实机黑度、残影或阅读体验验证；字体映射、字形边界和主机帧缓冲检查已由 `tools/test_app.py` 与 `tools/preview_ui.py` 覆盖，实屏效果仍需硬件验证。
