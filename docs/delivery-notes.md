# 双板 MP3 播放器 — 交付说明与验收记录

> 主控 STM32F407VET6（天空星） · 解码 ESP32-S3-N16R8 · 功放 MAX98357A · 屏 ST7789 2.4" 320x240
> 配套文档：[`docs/roadmap.md`](roadmap.md)（规划书）、[`hardware/wiring-diagram.html`](../hardware/wiring-diagram.html)（接线图）、
> [`hardware/tft-ui-demo.html`](../hardware/tft-ui-demo.html)（界面验收基线）
>
> 说明：下文提到的 `verify_*.py` / `render_*.py` / `cvjpeg.py` / `p2build.py` / `p2.map` / `out_*.png` 都是开发期在本机跑的验证脚本与产物，
> **未随本仓库发布**，列出来只为说明当时的验收方法与数据。

---

## 1. 交付内容

| 交付物 | 路径 | 说明 |
|---|---|---|
| STM32 工程 | [`stm32/`](../stm32/) | Keil MDK5 + ARMCC AC5；含 app/bsp/FatFS R0.16/libraries |
| ESP32 工程 | `esp32\` | PlatformIO(Arduino)；含 lib/mp3dec(Helix 软解)、lib/proto、lib/link、lib/player |
| 规划书 | [`docs/roadmap.md`](roadmap.md) | 方案、引脚、协议、阶段计划（含 3.2.1 实机结论） |
| 接线图 | [`hardware/wiring-diagram.html`](../hardware/wiring-diagram.html) | 最终接线 + SD/MODE 与 GAIN 真值表 + 背光时序 |
| 界面基线 | [`hardware/tft-ui-demo.html`](../hardware/tft-ui-demo.html) | 横版 UI 设计基线（本 UI 按此实现） |
| 中文字库 | `stm32\bsp\ui\ui_font_cn14.c/.h` | 14 px 点阵，**3973 字形**（P1 的 3599 + **GB2312 一级汉字全覆盖**，二进制 108455 B 位图 + 3973×12 B 描述符） |
| P2 新增模块 | `app/lyrics.c/.h`、`bsp/encoder/bsp_encoder.c/.h` | **.lrc 滚动歌词**（同目录同主名、GBK/UTF-8 自动识别、12.8 KB 静态缓冲）、**旋转编码器音量**（PC6/PC7 TIM8 编码器模式 + PD12 按下） |
| 本文件 | [`docs/delivery-notes.md`](delivery-notes.md) | 编译烧录、接线、操作、验收、排查 |

---

## 2. 系统分工

```
SD 卡 --SDIO--> STM32F407VET6 --USART2 1Mbps--> ESP32-S3 --I2S--> MAX98357A --> 喇叭
                 读卡/歌单/UI/按键                Helix 软解 MP3
                 只搬 MP3 原始字节                音量/软限幅/下混
```

- F407 没有 MP3 硬解，M4F 上跑软解会挤占 UI 与读卡实时性，所以把它交给带 PSRAM 的 ESP32-S3 双核。
- STM32 只做"文件系统 + 界面 + 流控"，把 MP3 帧原样搬过去；解码、音量、限幅都在 ESP32。
- 播放速率由 ESP32 按 MP3 头里的采样率/比特率**实测换算**后反馈给 STM32 的信用窗口，做到 1.00× 实时、不依赖 VBR 假设。

---

## 3. 最终接线（与接线图一致）

### 3.1 电源
| 网络 | 接法 |
|---|---|
| ESP32 / STM32 | 各自 5V / 3V3 供电，**共地**（必须） |
| MAX98357A VIN | 独立 5V，就近并 **100~470 µF + 0.1 µF** 到 GND |
| 喇叭 | OUT+ / OUT-，8 Ω / 1 W 以上，**OUT± 不得接地** |

### 3.2 STM32F407 侧
| 功能 | 引脚 | 备注 |
|---|---|---|
| SDIO | PC8(D0) PC9(D1) PC10(D2) PC11(D3) PC12(CK) PD2(CMD) + PD3(CD) | 4-bit，传输时钟 12 MHz |
| TFT SPI1 | PA5(SCK) PA7(MOSI) PA4(CS) PB0(DC) PB1(RST) PB10(BLK) | BLK 建议对 GND 加 10 kΩ 下拉 |
| 按键提示音 | PB8 | 3 脚有源蜂鸣器模块（低电平触发）：PB8 拉低 = 响、拉高 = 静音；固件把 PB8 配成推挽 + 内部上拉，复位期即静音 |
| 按键 K1~K5 | PC0 PC1 PC2 PC3 PC4 | 另一端接 GND，内部上拉 |
| 旋转编码器 | PC6(A 相) PC7(B 相) **PD12(按下)** | TIM8_CH1/CH2 AF3 硬件编码器模式（TI12、4 计数/档）；公共端接 GND，A/B/SW 内部上拉，无需外部电阻。**PC5 不接、PC8 不可用**（PC8 = SDIO_D0，见下） |
| 板间串口 USART2 | PA2(TX) → ESP32 GPIO17(RX)；PA3(RX) ← ESP32 GPIO18(TX) | 1 Mbps |
| 调试串口 USART1 | PA9(TX) PA10(RX) | 115200，2×5P 排针（非 Type-C） |
| 下载 | SWDIO PA13 / SWCLK PA14 / NRST / 3V3 / GND | |

### 3.3 ESP32-S3 与功放
| 功能 | GPIO | 备注 |
|---|---|---|
| I2S BCLK / LRCK / DIN | 5 / 6 / 7 | 短线上板，共地 |
| 功放 SD/MODE | **直接接 3V3** | >1.4 V ⇒ 常开，且**只出左声道**（固件已下混，不丢内容） |
| GPIO8 | **不接**（固件 `PIN_AMP_SD=-1`） | 实测经 560 kΩ 驱动 SD 会全程沙沙，见 §6.2 |
| 功放 GAIN | **悬空**（默认 9 dB） | 官方增益表：经 100 kΩ 接 GND=15 dB、直接接 GND=12 dB、悬空=9 dB、直接接 Vin=6 dB、经 100 kΩ 接 Vin=3 dB |
| 状态灯 LED | 48 | 绿=播放中 黄=缓冲 蓝=停止 |

---

## 4. 开发环境与编译烧录

### 4.1 STM32（Keil MDK5 + ARMCC AC5）
- 打开 `stm32\project\MDK(V5)\Project.uvprojx`，**Rebuild** 后下载。
- 必须项：**`--c99`**（FatFS R0.16 的 ff.h 只在 C99 分支 include <stdint.h>，AC5 默认 C90 会编译失败）；宏 `USE_STDPERIPH_DRIVER`, `STM32F40_41xxx`；启动文件栈 `Stack_Size = 0x1000`。
- 源码编码注意：**AC5 按 GBK 解析源文件**，中文注释没问题，但字符串字面量里的中文必须写成八进制转义（例如 `"\351\237\263"`），禁用 `\xNN`。
- 全量编译自检：`p2build.py`（armcc 逐文件编译 UVPROJ 里全部 .c + armlink 链接），最新一次（P11「死代码清理 + CCM 搬 RAM」）结果为 **66 个 .c，0 问题**；`Program Size: Code=55348 RO-data=332528 RW-data=564 ZI-data=122604`（Flash ≈ 388.4 KB / 512 KB、余量 ≈ 123.6 KB；RAM 总计 ≈ 120.3 KB，但其中 63.7 KB 在 **CCM**、主板 SRAM 只占 **≈56.6 KB / 128 KB（余量 ≈71.4 KB）**。封面子系统静态内存见 §9.6，本轮改造见 §9.8）。
- **内存布局（本轮）**：工程里设 `Scatter File = Project_ccm.sct` 并去掉 *Use Memory Layout from Target Dialog*（`Project.uvprojx`：`umfTarg=0 / useFile=1 / ScatterFile=Project_ccm.sct`）。`stm32\project\MDK(V5)\Project_ccm.sct` 里把封面解码缓冲所在的 `.ccmram` 段定点放到 **CCM 0x10000000（64 KB）**，主板 SRAM（0x20000000 / 128 KB）只留 `.ANY (+RW +ZI)`。细节与理由见 §9.8。
- 注：本机 `UV4.exe -b` 命令行构建会崩溃（0xC0000409），所以用 `p2build.py` 复刻 uVision 的 armcc/armlink 参数做等效构建；Keil GUI 里 Rebuild 同样有效。

### 4.2 ESP32-S3（PlatformIO）
```bash
cd esp32
pio run -e esp32s3 -t upload --upload-port COM8
pio device monitor -p COM8 -b 115200
```
- 环境：`platform = espressif32@6.9.0`、`board = esp32-s3-devkitc-1`、`qio_opi` + 16 MB Flash + 8 MB OPI PSRAM（`-DBOARD_HAS_PSRAM`）。
- 日志走 UART0(GPIO43/44) → 板载 CH340 的 USB-C 口（COM8），烧录和看日志同一个口。
- 开发期的两个调试开关（`-DPROBE_I2S` 测试音、`-DDBG_STATS` 统计）连同相关测试/统计代码，
  已在开源整理时删除；现在 `build_flags` 只剩 `-DBOARD_HAS_PSRAM` 与 `-DPIN_AMP_SD=-1`。
- 当前固件：RAM 7.3% (23900 B) / Flash 10.3% (344617 B)，`firmware.bin` 344976 B。

---

## 5. 使用说明

### 5.1 SD 卡
- 优先扫描 **`/MUSIC`** 目录，找不到则扫描根目录 `/`；只收 `.mp3`（大小写不敏感）；最多 **300** 首；显示名最长 59 字符（UTF-8 中文可）。
- 建议 FAT32、单分区；文件名用中文/英文均可（FatFS 配了 `FF_LFN_UNICODE=2` + `FF_CODE_PAGE=437`；中文文件名走 UTF-8 长名，与码页无关）。
- 卡内文件必须完整；曾遇到的"每首只有 6.8 秒"是卡上文件本身残缺（54432 B 的合法片段），不是固件问题。

### 5.2 按键
**P2 后的键位（音量交给旋钮，原音量键改为歌单/切换播放）**

| 键 | 短按 | 长按（≥0.6 s） |
|---|---|---|
| 旋转编码器 | 旋转 = 音量 ±5 | — |
| 编码器按下（PD12） | **歌词模式**（主界面 = 进入全屏歌词页；歌词页内 = 返回主界面） | — |
| K1 | **播放 / 暂停** | — |
| K2 | 上一曲 | — |
| K3 | 下一曲 | — |
| K4 | **歌单**（开/关歌单浮层） | — |
| K5 | **切换播放模式**（顺序 → 单曲循环 → 随机，UI 上显示「顺序 / 单曲 / 随机」） | — |

- 歌单模态：K2/K3 上下移动，K1 选中并播放，K4 或 K5 关闭（K4 是歌单键，再按一次即关闭）。
- **全屏歌词页**：旋钮按下 PD12 进出（主界面进入 / 页内返回，等同控制栏 [歌词] 按钮）；页内 **K1 仍是播放/暂停、K2/K3 仍是上一曲/下一曲，只有 PD12 能退出**。
- 音量 0~100 步进 5、默认 70，由旋转编码器调节（一格 ≈ 5 音量，转动时左侧音量条自动弹出 1.5 s）；模式 0=顺序（列表循环）1=单曲循环 2=随机（控制栏 [模式] 键上按此顺序显示「顺序 / 单曲 / 随机」三个字符串之一，与旁边的 [列表] 键不再重名）。
- 与原 P1 的差异：K4/K5 不再是音量键（改成 歌单 / 模式键），**K1 仍是最初的"播放/暂停"**（长按打开歌单的手势已取消）；歌单改由 K4 负责，播放/暂停由 K1 与旋钮按下负责。
- 旧键位（P1）：K1 短=播放/暂停 长=打开歌单、K2=上一曲、K3=下一曲、K4 短=音量+5 长=切模式、K5=音量−5。
- **按键提示音（本轮）**：⚠ **P11 起默认静音**（`BEEP_DEFAULT_EN = 0u`），要提示音时改 `bsp_beep.h` 里该宏为 `1u` 或运行期调 `beep_set_enable(1)` 即可，调用点不用动。PB8 接 3 脚**有源蜂鸣器**（低电平触发：PB8 拉低 = 响、拉高 = 静音，复位期靠内部上拉保持静音）。触发点：任意按键（K1~K5）与旋钮每格（`beep_key()`）→ **30 ms**（`BEEP_MS_KEY`）；进 / 出歌单浮层、进 / 出全屏歌词页（`beep_ms(BEEP_MS_ACT)`）→ **60 ms**。全程非阻塞：`beep_on(ms)` 只记 `s_until = tick_ms() + ms` 并拉低 PB8，主循环每轮 `beep_poll()` 用 `(int32_t)(tick_ms() - s_until) >= 0` 判到点后拉高静音，内部无任何 `delay_ms` ⇒ 连按 / 旋钮连续转动都不会卡；`beep_set_enable(0)` 可全局静音（立即静音并忽略后续请求）。

### 5.3 界面
320x240 横屏，无中间居中歌名、无装饰电平柱（按界面基线调整后的版本）：左侧歌曲信息/进度/时长，歌单浮层模态，音量与模式指示。

P2 变化：
- **歌词区**从写死的"暂无歌词 / 请欣赏纯音乐"占位改为真实 **4 行滚动歌词**（上一行 / **当前行高亮放大** / 下一行 / 再下一行），行变化时才做**脏刷新**（只重画变化的行）；无 .lrc 或前奏时保持占位文案。
- **控制栏**去掉 [−][+] 两个音量按钮，改为 `[⏮][▶][⏭] 音量 N | [歌词][模式][歌单]`（播放键实心高亮，最后一键的文案为"歌单"）；左侧音量条仍会在调音量时自动弹出 1.5 s。
- 歌单浮层脚注改为 `K2上 K3下 K1选 K4/K5返回`。
- 歌单翻动改为**脏刷新**：上下移动只重画"旧选中行 + 新选中行"两行，翻页（滚出视窗）时只重画 6 行条目，背景 / 表头 / 脚注不再跟着重画；到顶 / 到底一帧都不重画。
- **全屏歌词页**（控制栏 [歌词] / 旋钮按下 PD12）：居中 **5 行视窗**（上 2 行 + 当前行 + 下 2 行），**当前行 1.5 倍放大 + 高亮**（`bsp/ui/bsp_ui.c` 的缩放渲染器泛化为 `ui_text_scl(x,y,s,fg,bg,num,den)`，新增 `ui_text15()` 按 3/2 最近邻采样），随 `pos_ms` 自动滚动到当前行；顶部歌名 + 序号，底部进度条 / 时间 / 「旋钮按下 返回」；脏刷新（只有变化的那一行重画），无 .lrc 时显示「暂无歌词 / 请欣赏纯音乐」。
- **底部进度条防闪**：只补「新增的那一段」像素（前进补新段、回退只擦掉多出的段），秒数文本真的变了才擦 40×15 那一小块；轨道 / 边框 / 中间提示画好后不再重画（旧实现每整秒铺一次 320×32 底板，进度条区域每秒黑一下 ⇒ 闪频）。
- **主界面也全程增量刷新**：主界面进度条与全屏歌词页同款（`draw_progress_delta()` 只补/擦差额像素，左侧时间 `draw_progress_time()` 只在文本真的变了才擦那一小块）；音量读数四处都改成小块增量——状态栏数字（`draw_status_vol()`，只擦 21×15 的固定字段）、控制栏数字（`draw_ctrl_vol()`）、侧边音量条（`draw_side_vol_delta()`，弹出后只补差额）、**全屏歌词页页脚新增常显「音量 N」读数**（`ly_draw_vol()`，值变化时只重画那一小块并高亮 1.5 s，之后自动恢复常规色）。
- **切页也改成「局部恢复」**：歌单浮层关闭 / 全屏歌词页退出不再整屏清底重画。主界面四块各带一个有效位 `s_blk`（`BLK_STATUS` / `BLK_COVER` / `BLK_INFO` / `BLK_CTRL`，整块重画时自置位），浮层打开只把被它盖住的三块置为无效、歌词页打开四块全无效；关闭时 `restore_main()` 先用 `clear_main_bg()` 只擦四块底板铺不到的 5 条背景缝隙（共 10496 px），再逐块重画被覆盖的块。**歌单浮层只盖 y=26..239 ⇒ 状态栏不重画、退出时不闪**；若浮层期间状态变了（暂停 / 换曲 / 换模式）先整条补画状态栏，只调了音量就只补那一小块 21×15。`ui_player_full()` 原样保留，仍是启动期与兜底的整屏整块重画。
- **切页加了极短的背光淡入/淡出过渡**：背光从原来的 GPIO 二值开关升级为 **TIM2_CH3（PB10，AF1）10 kHz 硬件 PWM**（PSC=83 / ARR=99 ⇒ 100 档，`LCD_BL_FULL 100`），四类切页（进 / 出歌单浮层、进 / 出全屏歌词页）都走「**淡出 60 ms → 背光为 0 的那一帧再换画面 → 淡入 60 ms**」的非阻塞状态机（`page_fade_poll()` 挂在 `ui_player_update()` 最前面，过渡期间整帧更新体直接 return）；上电仍是「全部初始化 + 首帧画完」之后再用 60 ms PWM 淡入点亮。一次切页 120 ms，画面突变被藏在全黑那一帧 ⇒ 看不出割裂。
- **封面区改成真封面**：`app/cover.c` 新增自写的**迷你 baseline JPEG 解码器**（不引第三方库）。换曲时先找 **MP3 内嵌 ID3v2 图**（`PIC` v2.2 / `APIC` v2.3 / v2.4），找不到再找**同目录 sidecar**（`cover.jpg` / `cover.JPEG` / `folder.*` / `<同名>.jpg` 等 12 个候选，大小写各两种）；解出的 **144×144 RGB565** 直接 `lcd_blit` 到封面区（`bsp/ui/ui_player.c:346 draw_cover()` → `cover_ready()` / `cover_pixels()`）。支持灰度 / 4:4:4 / 4:2:2 / 4:2:0 / 4:1:1 + DRI 重启；**渐进 JPEG、CMYK、边长超 1600、坏码表一概拒绝**并保留原占位图标（不会卡住播放）。
- **切歌提速（本轮）**：封面解码由「换曲时同步解完才出声」改为**分片异步** —— `cover_begin(path)` 只定位源（ID3v2 APIC 或 sidecar）并解析到 SOS，剩下的 MCU 扫描交给主循环 `cover_poll(2u)`（每帧最多 2 ms）推进，解完再刷一次封面区；`set_track()` 顺序改为 **时长解析 → 先 `audio_srv_open()` 出声 → 歌词 → `cover_begin()`**。最坏情况（1600×1600 封面 ≈0.5 s）下，按 K3 到出声从 ≈500 ms 降到 **≈40 ms**，解码期间界面 / 按键全程流畅；断点落在 MCU 边界，解出的 144×144 RGB565 与原同步路径**逐像素完全相等**（§9.7）。`cover_load()` 保留为 `cover_begin()` + `cover_poll(0u)` 的同步包装，语义不变。

---

## 6. 实机验收记录

### 6.1 已通过
| 项目 | 结论 | 证据 |
|---|---|---|
| 全链路出声 | ✅ | 15 首歌全部正常播放 |
| 播放速率 | ✅ 1.00× 实时 | 输入 39.87 KB/s = 320 kbps 标称；解码 38.3 帧/s = 44100/1152 |
| 帧同步稳定 | ✅ | 重锁 `rs` 仅开机 1 次（skip=166 字节，落在合法 MPEG 头 `FFFBE264` 前），之后 `skipB` 不再增长；`nosync=0` `holes=0` `crc=0` `blen=0` |
| 数字 PCM 干净 | ✅ | `pcmq`：1 kHz 哔声 rms/peak=0.694、440 Hz 纯音 0.707、hf 0~1%；音乐 hf 15~25%，均为正常取值 |
| 进度/时长 | ✅ | 时长来自真实 MP3 帧解析（`mp3info.c`），进度由 TIM7 毫秒计时，实测每秒 +1000 ms |
| 喇叭杂音（沙沙） | ✅ 已消除 | 功放 SD/MODE 由 GPIO8 改接 3V3 后消失（机理见 6.2） |
| 大音量破音 | ✅ 明显改善 | 固件加 −6 dBFS 软限幅 + 单声道下混后，"大音量破音好很多" |
| 上电闪屏 | ✅ 已消除 | 背光延后到"全部初始化 + 首帧画完"再打开 |

### 6.2 沙沙声的根因（复盘，很有参考价值）
MAX98357A 的 **SD/MODE 不是单纯使能脚，而是同时选声道**：

| SD 电压 | 行为 |
|---|---|
| < 0.16 V | 关断 |
| 0.16 ~ 0.77 V | 输出 (L+R)/2 |
| 0.77 ~ 1.4 V | **只出右声道** |
| > 1.4 V | **只出左声道** |

原设计用 GPIO8 经 560 kΩ 驱动 SD。若模块带未断开的 1 MΩ SD→Vin 上拉，分压点为
` (3.3/560 + 5/1000) / (1/560 + 1/1000 + 1/100) ≈ 0.85 V `，正好贴住 0.77 V 分界线，
音频信号一抖就在"平均"与"右声道"两个模式间来回跳 ⇒ **随音量变化的沙沙 + 发闷 + 人声小**（暂停时无信号所以没有沙沙）。
把 SD 直接接 3V3 后模式稳定，杂音消失。

代价：3V3 那档只出左声道，因此固件在送出前做 **单声道下混 `m=(L+R)/2`，左右槽都写 m**，
这样无论功放工作在哪个档位都放出完整内容。

### 6.3 若还要进一步压榨音质
1. **GAIN 改 3 dB 档**：把 GAIN 经 100 kΩ 接 Vin（余量最大，专治大音量硬削波）。
2. **VIN 去耦**：100~470 µF 电解 + 0.1 µF 瓷片就近到 GND，独立 5V ≥1 A。
3. **喇叭**：8 Ω/1 W 以上，别用蜂鸣器；OUT± 不可接地。
4. 重新打开 `-DPROBE_I2S=1` 可对比 A1~A4 四种 I2S 格式（16bit/标准、16bit/左对齐、32bit 槽、APLL）听感差异（**该开关已在开源整理时删除，此处为开发期记录**）。

### 6.4 P2 新增功能（本机验证结论）
| 项目 | 本次结论 | 依据 |
|---|---|---|
| 旋转编码器驱动 | ✅ 编译通过、引脚无冲突 | PC6/PC7 = TIM8_CH1/CH2（TIM8 在 P1 里从未使用，不抢外设）；按下脚用 **PD12**（GPIOD 只用了 PD2/PD3）。**PC5 保持不配置**（恢复 P1 空置状态）；**PC8 不能用**——它是 SDIO_D0（`bsp_sdio.c:43/55` 配成 AF_SDIO，`bsp_sdio.c:395` 传输切 4-bit），接旋钮按下会与 SD 卡抢数据线 |
| 编码器计数逻辑 | ✅ 审查通过 | TIM8 硬件编码器模式 TI12（两相上升沿计数）、`TIM_ICFilter=10` 消抖动、`(int16_t)(now-last)` 回绕安全差分、余数累加不丢半格（4 计数/档） |
| .lrc 解析 | ✅ 审查通过 | 支持 `[mm:ss]` / `[mm:ss.x]` / `[mm:ss.xx]` / `[mm:ss.xxx]`、一行多时间标签（副歌）、`[offset:±N]`、UTF-8(BOM)/GBK 自动识别；上限 128 行 × 96 B |
| GBK 编码转换 | ✅ +16 KB Flash | `.lrc` 的 GBK 行由 `app/gbk2312_map.h`（自带 GB2312 表，7445 个非零项）查表转 Unicode —— FatFS 码页改为 437 后 `ff_oem2uni(..., 936)` 不再可用 |
| 字库扩充 | ✅ 独立脚本验证 | GB2312 一级 3755 字**全覆盖**；P1 的 3599 字形**逐字节 3599/3599 未变**；样张渲染 6 句歌词全部正常 |
| 歌词刷新方式 | ✅ 编译通过 | 歌词 4 行窗口逐行与上一帧比较，只有变化的行才重绘；滚动时不再整块重画信息区（封面/歌手/进度/时间不跟着闪） |
| 歌词/进度比声音慢 | ✅ 已修（审查，未上机） | 根因：ESP32 上报位置把环内待播量**扣了两遍**——`s_written_us`（`player.cpp:959-966`）只累加 `i2s_write` 实际接受的字节，`s_pcm` 环里还没送 DMA 的 PCM 本就不在其中，旧代码又减了一次 `pcm_lead` ⇒ 上报比真实出声慢 0~0.74 s（128 KB PCM 环 ÷ 176.4 KB/s）。现改为 `pm = s_written_us/1000 + s_base_ms`（`player.cpp:1194-1201`） |
| 歌单翻动刷新 | ✅ 编译通过 | 上下移动只重画旧 / 新选中两行；翻页只重画 6 行条目（不动背景 / 表头 / 脚注）；到顶 / 到底零重画 |
| 全屏歌词页 | ✅ 编译通过 + PC 逐像素渲染验证 | 控制栏新增 [歌词] 按钮、PD12 进出；5 行视窗居中、当前行 **1.5× 放大高亮**（`ui_text15()`）、随 `pos_ms` 自动滚动、脏刷新。PC 复刻 C 渲染实测：当前行 12 字 1× 168 px → 1.5× 252 px（x=34..286 屏内，墨迹 y=109..129 落在 30 px 槽 104..133 内、上下各余 5/4 px），38 字超长行经 `fit_text` 截到 14 字加 `…`（1.5× 312 px，两侧各留 4 px），footer 左/中/右三段互不重叠；另用 `verify_text15.py` 对全字库 3973 字形 × 7 个位置回归：**1× 与 2× 路径与改前的整数实现逐像素一致**，1.5× 与独立逆映射参考实现一致、放大不漏源行/列；控制栏 [歌词]200..236 / [模式]276..312，音量文字与 [歌词] 框间距 11 px；底部进度条改**增量刷新**（只补新增像素、不整块重铺 ⇒ 不闪频） |
| 主界面进度条增量刷新 | ✅ 编译通过 + PC 逐像素回归 | `draw_progress_delta()` 每帧只补/擦差额像素（前进补新段；回退擦多出的段并按是否仍被填充覆盖复原左/右边框列），左侧时间 `draw_progress_time()` 只在文本变化时擦旧/新并集那一小块；`verify_delta_p6.py` 391 帧（平滑前进 / 每个整秒 / 走满超长 / 换曲换总时长 / 无总时长 / 随机拖动）与「从头全量重画同一状态」逐像素一致，单帧最多写 616 px（全量信息区 ≈21888 px） |
| 音量显示增量 + 歌词页音量读数 | ✅ 编译通过 + PC 逐像素回归 | 状态栏 / 控制栏数字、侧边音量条、歌词页页脚「音量 N」都只重画那一小块：状态栏改固定 21 px 字段（`vol_field_w()`，否则 9→100 时数字变宽会把 "SD" 的尾巴擦掉）、歌词页读数缓存「已画出的块宽」（3 位→1 位再恢复高亮时不留残影）；回归 A 状态栏 21 帧 / B 控制栏 18 帧 / D 侧边条 37 帧 / E 歌词页页脚 399 帧全部一致，单次最多写 3107 px（全量页脚 10240 px） |
| 切页「局部恢复」（valid 位 + restore_main） | ✅ 编译通过 + PC 逐像素回归 | 主界面四块加有效位 `s_blk`：整块重画函数末尾自置位，五个增量函数开头 `if ((s_blk & BLK_xxx) == 0u) return;` 放弃守卫；`ui_player_list_open()` 只失效 `COVER|INFO|CTRL`（浮层从 y=26 起，状态栏保持有效）、`ui_player_lyrics_open()` 四块全失效；关闭时 `restore_main()` + `clear_main_bg()` 只擦 5 条背景缝隙（10496 px）再逐块重画，`ui_player_full()` 一行未改、仍留作兜底（`app/main.c:175/180/214/219` 启动期仍用）。`verify_restore_p7.py`（复用 verify_delta_p6.py 的字库 / 版面 / 帧缓冲模型）：① 源码断言 22 项全绿（四块底板铺满整块且是函数里第一条 `bar()`、四个自置位、五个守卫、歌词脏刷新新增 `BLK_INFO` 条件、四条切换路径掩码、浮层只盖 y=26..239 / 歌词页盖整屏）；② 覆盖性：写入矩形 ⊇ 被覆盖区域，关歌单浮层 68480/68480 px、退歌词页 76800/76800 px，**0 空洞**，且不碰未被覆盖的状态栏；③ 把被覆盖区域先填垃圾像素再跑局部恢复，与整屏兜底参考帧**全屏逐像素 0 差异**（6 个场景：状态未变 / 期间调音量 / 期间暂停 / 期间换曲换模式 / 退歌词页两种）⇒ 垃圾像素零残留；④ 浮层 / 歌词页期间五个增量刷新在覆盖区写入 **0 px**（不会把浮层画花），对照四块全有效时写 3645 px ⇒ 守卫是按块放行；⑤ 写入量：关歌单浮层 93974 px（整屏兜底 170271 px 的 55%）、退歌词页 103967 px（61%），**擦除步**从 76800 px 降到 10496 px |
| 切页背光淡变（PWM 背光） | ✅ 编译通过 + PC 数学 / 状态机回归 | 背光由 GPIO 开关升级为 **TIM2_CH3(PB10) 10 kHz PWM**（`bsp/lcd/bsp_lcd.c/.h`：PSC=83 / ARR=99、`lcd_backlight_init/set/get/fade`，保留 `lcd_set_backlight(on)` 兼容包装；`LCD_BLK_ON/OFF` 宏在 PWM 模式下不再使用）；`app/main.c:226` 上电改 `lcd_backlight_fade(LCD_BL_FULL, 60u)`（最后一帧画完之后、主循环之前）。四类切页改为 `pg_begin(动作)` + `page_fade_poll()`：淡出 60 ms（占空比 100→0）→ 到 0 那一帧 `page_commit()` 才真正 `draw_pl_full()` / `restore_main()` / `ly_draw_full()` → 淡入 60 ms（0→100）→ 清状态位；过渡期间 `ui_player_update()` 与 `ui_player_list_move()` 直接 return、`ui_player_vol_flash()` 也不闪，`ui_player_full()` 一行未改（不碰背光，保住「先画完再点背光」）。`verify_fade_p8.py` = **ALL PASS**：A~D 共 44 项源码断言（含门闸早于两个早退、commit 只在占空比 0 那一帧、四个切页函数都不再当场重画），E PWM 数学（84 MHz ÷ (83+1) ÷ (99+1) = **10.000 kHz**、`LCD_BL_FULL == ARR+1`），F 状态机逐毫秒仿真（四类切页各 commit 恰一次、淡出序列 100,99,…,2,0 单调不增、淡入单调不减且终值恰为 100、单程 60 ms 总 120 ms；淡出途中反悔只 commit 最后一次；淡入途中反悔两次 commit 都在全黑帧；连续乱按 20 次后 120 ms 内回到全亮），G commit 复用的就是上一轮验证过的三个整块 / 局部恢复函数（淡变只改「什么时候画」，不改「画什么」） |
| 真实封面（迷你 JPEG 解码） | ✅ 编译通过 + PC 逐像素回归 | `app/cover.c/.h`：自写 baseline JPEG 解码器（Huffman + 反量化 + 整数 IDCT，**不存整图**）——边解边把样本按 cover-fit（填满正方形 + 居中裁剪）归到 144×144 格子做盒式降采样，累加和/计数放 **18 行滑动窗口**，满一行就「取平均 → 补洞 → YCbCr→RGB565」写 `s_px`，`cover_pixels()` 直接交给 `lcd_blit`。来源：ID3v2.2 `PIC` / v2.3 / v2.4 `APIC`（synchsafe 帧长、`0x40` 扩展头、`0x10` footer、`0x80` unsync 放弃、描述区 `head[256]` 截断）→ 失败回退同目录 sidecar 12 个候选。回归 `verify_cover_p9.py`（`cvjpeg.py` 是 cover.c 的逐位 Python 镜像，zigzag / IDCT 常量表运行时从 cover.c 源码抽）：A 常量与接线、B Huffman 码表 vs 独立教科书实现、C 全分辨率 RGB vs Pillow 逐像素、D 窗口统计 vs numpy 暴力参考**逐格完全相等**、E 装配（取平均/补洞/YCbCr/RGB565）vs numpy 独立重写**逐像素相等**、F 接受/拒绝边界（灰度 63 拒 / 64 收、4:2:0 135 拒 / 136 收、渐进 / CMYK / 截断 / 超 1600 / 坏 RSTn 全拒）、G APIC 定位（v2.2/2.3/2.4 及各种边界）与 sidecar 候选顺序，共 **PASS 194 / FAIL 0**；`render_cover_p9.py` → `out_cover_p9.png`（灰度 / 4:4:4 / 4:2:2 / 4:2:0 / 小图放大 5 格预览）。**代价**：静态 RAM +68.3 KB，其中 **63.7 KB 已在 P11 搬进 CCM**（见 §7 与 §9.6 / §9.8） |
| 按键提示音（PB8 有源蜂鸣器） | ✅ 编译通过 + PC 行为模型回归 | 新增 `bsp/beep/bsp_beep.c/.h`（PB8 推挽输出、**低 = 响 / 高 = 静音**、内部上拉保证复位期静音；API `beep_init/beep_on/beep_off/beep_poll/beep_busy/beep_key/beep_ms/beep_set_enable/beep_enabled`，常量 `BEEP_MS_KEY 30u` / `BEEP_MS_ACT 60u`）；`app/main.c` 上电 `beep_init()`、K1~K5 与旋钮每格 / 进出浮层与歌词页共 4 类触发点、主循环调 `beep_poll()`；`Project.uvprojx` 注册 + IncludePath 加 `..\..\bsp\beep;`。回归 `verify_cover_p10.py` A 段：源码 / 接线断言 + Python 行为模型（30 ms 到点自动关、`uint32_t` 回绕安全、`beep_on(0)` → 1 ms、`beep_set_enable(0)` 立即静音、全文件无 `delay_ms`） |
| 切歌提速（封面分片异步） | ✅ 编译通过 + PC 逐像素回归 | `app/cover.c` 把单体 `cv_jpeg()` 拆成 `cv_head()` + `cv_scan_step(budget_ms)`（断点 `s_step_my/s_step_mx`，返回 2 = 还需继续、1 = 完成），对外新增 `cover_begin/cover_poll/cover_busy`，`cover_load()` 退化为 `begin + poll(0)` 同步包装；`app/main.c` 先 `audio_srv_open()` 出声、再 `cover_begin()`、主循环 `cover_poll(2u)`。回归 `verify_cover_p10.py` B 段：8 类 JPEG（灰度 / 4:4:4 / 4:2:2 / 4:2:0 / 非整 MCU 尺寸 / 100×100 放大 / 320×240 / DRI 重启）在 budget=1 逐 MCU 停、budget=0、budget=3、抖动时钟下与一次性解码**逐像素完全相等**；PASS 154 / FAIL 0 |
| 死代码清理 + CCM 搬 RAM（P11） | ✅ 编译通过 + 9 个回归脚本全绿 | 删掉全工程 **0 调用点**的 19 个函数（含头文件声明），armlink 之前已丢弃其中大部分，实际净省 332 B Code；把封面 4 个静态缓冲（`s_px` 41472 B + `s_acc` 15552 B + `s_accc` 7776 B + `s_prev` 432 B = **65232 B**）用 `__attribute__((section(".ccmram"), zero_init))` 搬进 **CCM 0x10000000（64 KB）**，主板 SRAM 占用 57936 B ⇒ **余量 ≈71.4 KB**（改前 7.7 KB）；新增 `project/MDK(V5)/Project_ccm.sct` + uvprojx 改用该 scatter。armcc 全量 **66 个 .c / 0 问题**，`Code=55348 RO-data=332528 RW-data=564 ZI-data=122604`（Flash ≈388.4 KB / 余量 ≈123.6 KB）。详见 §9.8 |
| **上板实测** | ❌ **未做** | 本机**没有任何主机 C 编译器**（gcc/clang/cl/tcc 均无），也没有实物板卡 ⇒ 只能做到"armcc 编译 + 逐行人工审查"。**需上板验证：编码器方向与每档计数手感、歌词时间轴对齐、中文歌词实际显示效果、全屏歌词页 1.5× 放大字号与 PD12 进出/返回手感** |

---

## 7. 已知限制

| 限制 | 说明 |
|---|---|
| 单声道输出 | SD 接 3V3 后功放固定左声道，固件下混为单声道；如需真立体声，SD 要接 0.30 V 分压点（经 1 MΩ 上拉到 3.3 V）并让 ESP32 输出立体声 |
| 仅支持 MP3 | 不支持 WAV/FLAC/AAC；MP3 采样率按文件头自适应（实测 44.1 kHz / 32 kHz 均可） |
| 无实时时钟 | FatFS `FF_FS_NORTC=1`，文件时间戳固定，UI 不显示日期 |
| Flash 占用 | 中文字库约 156 KB 在 STM32 片内 Flash（3973 字形：108455 B 位图 + 3973×12 B 描述符）。当前总占用 ≈ **388.4 KB / 512 KB，余量 ≈ 123.6 KB**（P11 后 Code=55348，Flash+RO-data+RW-data ≈388.4 KB；本轮删死代码省 332 B Code，另因新增一个 scatter 区多 16 B RO-data） |
| 内部 RAM 布局（P11 起） | 真实封面把静态 RAM 抬到 **120.3 KB**（ZI-data 122604 B + RW-data 564 B），但 **P11 起不再全压在主板 SRAM 上**：`app/cover.c` 的 `s_px[144*144]` 41472 B + 窗口 `s_acc/s_accc` 23328 B + `s_prev` 432 B = **65232 B** 用 `__attribute__((section(".ccmram"), zero_init))` 搬进 **CCM（0x10000000，64 KB）**，主板 SRAM（0x20000000，128 KB）只占 **≈56.6 KB（余量 ≈71.4 KB，改前仅 7.7 KB）**。CCM 只挂 CPU 数据总线、**不能做 DMA 源/目标**，而本工程全工程无外设 DMA（LCD 直接写、SDIO 轮询 FIFO），所以放封面缓冲完全安全。布局见 `project/MDK(V5)/Project_ccm.sct` 与 §9.8 |
| 字库只覆盖 GB2312 一级 | 二级汉字（GB2312 0xD8A1 之后，3008 字）与符号区（682 字符）**没有字形**。`ui_text` 找不到字形时只前进 7 像素、不画方框 ⇒ 生僻字表现为**空白缺口**。全量 GB2312 需约 296 KB 字库，放不进剩余 Flash |
| 歌词格式上限 | 单个 .lrc **最多 128 行**、每行正文 **最多 95 字节**（UTF-8，约 31 个汉字），超出截断；不支持逐字卡拉 OK（只按行滚动）；不支持 UTF-16 编码的 .lrc |
| 全屏歌词页字号 | 当前行放大到 **1.5 倍**（22 px 行盒，在 30 px 槽位里上下居中），一行最多约 **15 个汉字**（1.5× 约 312 px）即被 `fit_text` 截断加 `…`；顶部歌名条按 210 px 截断（追加的 `…` 不计宽度，极端情况下可到约 224 px，仍不会碰到右侧序号） |
| 歌词/编码器未经上板实测 | 见 §6.4：本机无主机 C 编译器与实物板卡，P2 代码只经过 armcc 全量编译与人工审查 |

---

## 8. 故障排查

| 现象 / 日志 | 原因与处理 |
|---|---|
| `[MP3] !! sd_init FAILED` | SDIO 初始化失败：查 PC8-12/PD2 接线、卡是否插好、是否换成慢速卡（12 MHz 已是折中值，可降到 0x03/6 MHz 试） |
| `[MP3] !! f_mount failed (13)` | FR_NO_FILESYSTEM：卡不是 FAT/FAT32 或分区表损坏，用电脑重新格式化 |
| `[MP3] playlist: 0 song(s)` | 卡里没有 `/MUSIC/*.mp3`；注意扩展名必须是 .mp3 |
| ESP32 `[PLAY] rs#...` 持续增长 | 音频流里混入了非帧数据：看 `junk=` 与 `hdr=` 十六进制；若是开机第一条（`skip=166`）属正常 |
| ESP32 `nosync` / `holes` 增长 | 链路丢字节：查 USART2 连线/长度/共地，或看 STM32 侧 `drops` |
| ESP32 `ovr` 增长 | 输入环溢出（STM32 发太快）：信用窗口自会收敛，持续增长则查 `CREDIT_MAX_INFLIGHT` |
| 完全无声但哔声正常 | 先看 ESP32 `aframes/bytes` 是否为 0；为 0 说明 STM32 没发音频帧，检查是否已按 **K1（或旋钮按下）** 开始播放、`credit` 是否为 0 |
| 有沙沙声 | 见 §6.2：确认 SD/MODE 是接 3V3（不是 GPIO8），GAIN 悬空或按要求接 |
| 大音量破音 | 见 §6.3：先试 GAIN 3 dB 档，再查供电与喇叭 |
| 上电闪一下屏 | 在 BLK 与 GND 之间加 10 kΩ 下拉（MCU 复位期间 PB10 高阻）；若模块是低电平点亮，对调 `bsp_lcd.h` 里 `LCD_BLK_ON/OFF` |
| 歌词一直显示"暂无歌词 / 请欣赏纯音乐" | ① .lrc 必须与 mp3 **同目录同主名**（`/MUSIC/歌名.mp3` ↔ `/MUSIC/歌名.lrc`）；② 串口看 `[LRC] N line(s) GBK\|UTF-8`，N=0 说明没找到文件或文件里没有任何 `[mm:ss]` 时间标签 |
| 歌词显示为乱码 | 文件既不是 UTF-8 也不是 GBK（如 UTF-16/BIG5）⇒ 用记事本另存为 UTF-8 或 ANSI(GBK)；串口日志里 GBK/UTF-8 的判定结果可直接看出识错 |
| 歌词比人声早/晚 | 用 `[offset:±N]` 校正（单位毫秒，正值让歌词提前，负值延后） |
| 转动旋钮没反应 | 查 PC6/PC7 是否焊到 A/B 相、编码器公共端是否接 GND；A/B 接反只会导致方向相反（见下一条）。若完全无反应，先看串口心跳里的 `vol=` 是否随转动变化 |
| 旋钮方向反了 | 把 `bsp/encoder/bsp_encoder.c` 里的 `ENC_DIR_INVERT` 从 0 改成 1 |
| 转一格跳很多/不灵敏 | 编码器型号不同 → 改 `ENC_COUNTS_PER_DETENT`（默认 4，即 4 个计数为 1 档）；抖动大时把 `TIM_ICFilter` 调大（当前 10） |
| 旋钮按下不响应 / 一直触发 | 按下脚是 **PD12**（另一端接 GND，内部上拉）；按下响应为"刚按下"事件，消抖 15 ms（`ENC_SW_DEBOUNCE_MS`）。**别把 SW 接到 PC5 或 PC8**：PC5 已按需求保持空置，PC8 是 SDIO_D0，接上去会读不出卡 |

---

## 9. 关键参数速查

### 9.1 时钟（STM32F407）
HSE 8 MHz → PLL M=8 / N=336 / P=2 / Q=7 ⇒ SYSCLK **168 MHz**，PLL48CK 48 MHz（SDIO），APB1 42 MHz，APB2 84 MHz。
SDIO_CK = SDIOCLK / (2 + CLKDIV)：初始化 CLKDIV=0x76 ≈ 400 kHz，传输 CLKDIV=0x02 ⇒ **12 MHz**（24 MHz 因轮询喂不上 FIFO 而失败）。

### 9.2 FatFS R0.16 配置
`FF_CODE_PAGE=437`（省掉 174 KB GBK 转换表；GBK 歌词改由 `app/gbk2312_map.h` 提供）、`FF_LFN_UNICODE=2`(UTF-8)、`FF_USE_LFN=1`、`FF_MAX_LFN=255`、`FF_FS_TINY=1`、`FF_FS_NORTC=1`、`FF_VOLUMES=1`；取文件名用 `fno.fname`。

### 9.3 板间协议
帧：`[0xAA][0x55][type][len_lo][len_hi][payload...][crc_lo][crc_hi]`，小端，CRC-16/CCITT-FALSE（poly 0x1021，init 0xFFFF，不反转、无末异或），覆盖 type+len+payload；最大 payload 1040 B。

| 命令 | 值 | 载荷 |
|---|---|---|
| CMD_PLAY | 0x10 | u16 曲目号 |
| CMD_PAUSE / RESUME / NEXT / PREV / STOP | 0x11/0x12/0x13/0x14/0x15 | 无 |
| CMD_VOL | 0x16 | u8 0~100 |
| CMD_MODE | 0x17 | u8 0=顺序 1=单曲循环 2=随机（UI 显示「顺序 / 单曲 / 随机」） |
| CMD_SEEK | 0x18 | u32 毫秒 |
| CMD_AUDIO_REQ | 0x19 | u16 块数（信用申请，块=1024 B） |
| CMD_LIST_REQ | 0x1A | u16 起点, u16 条数 |
| CMD_PING | 0x1B | u32 tick |
| CMD_SYNC_POS | 0x1C | u16 曲目, u32 **播放位置**(ms，= 已交给 I2S DMA 的音频时长 + seek 起点), u32 本曲已送 DMA 毫秒（换曲握手用） |

STM32 侧只在收到上报后 **2.5 s** 内信任该位置（`app/audio_srv.c:90`），超时退回「已送出文件字节 × 时长 / 总字节」估计（该估计比出声超前，最多约一个输入环 ≈ 4 s；上报周期 500 ms，正常走不到兜底）。上报值必须是**真实出声位置**：`s_written_us` 只统计已进 I2S DMA 的音频，环内待播 PCM 不在其中，**不能二次扣减**（`player.cpp:1194`）。

### 9.4 旋转编码器与 .lrc 歌词（P2 新增）
**编码器**：PC6=TIM8_CH1(AF3) A 相、PC7=TIM8_CH2(AF3) B 相、**PD12=旋钮按下**（内部上拉、低有效；PC5 保持空置、PC8 是 SDIO_D0 不可用）。引脚宏在 `bsp/encoder/bsp_encoder.c` 顶部：`ENC_SW_PORT` / `ENC_SW_PIN`，换脚只改这两行。
TIM8 用**硬件编码器模式** `TIM_EncoderMode_TI12`、CH1/CH2 均为上升沿、`TIM_ICFilter = 10`、ARR=0xFFFF；**4 个计数 = 1 档**，软件保留余数（不丢半格），一次最多报告 ±4 档 ⇒ ±20 音量；旋钮消抖 15 ms。
手感方向相反时把 `bsp/encoder/bsp_encoder.c` 里的 `ENC_DIR_INVERT` 置 1。

**.lrc 歌词**：文件名与 mp3 **同目录同主名**（`/MUSIC/周杰伦 - 晴天.mp3` → `/MUSIC/周杰伦 - 晴天.lrc`）。
支持 `[mm:ss]` / `[mm:ss.x]` / `[mm:ss.xx]` / `[mm:ss.xxx]`、**一行多个时间标签**（副歌复用，最多 8 个）、`[offset:±N]`（单位 ms，**正值让歌词提前**）、`[ti:][ar:][al:][by:]` 忽略、无时间标签的行丢弃。
编码自动识别：UTF-8 BOM / UTF-8 / **GBK**（GBK 经 `app/gbk2312_map.h` 查表转 Unicode，+16 KB Flash）。
内存：`s_time[128]` + `s_text[128][96]` ≈ 12.8 KB 静态（不用 malloc）。前奏时固定停在第一行（与 HTML 基线一致）。
歌词区为**脏刷新**：4 行窗口逐行与上一帧比较，只有变化的行才重绘，滚动时封面/歌手/进度/时间不再跟着闪。

**全屏歌词页**（本轮新增）：控制栏 [歌词] 按钮 / 旋钮按下 **PD12** 进出（`ui_player_lyrics_open/close/is_open`）。
版面常量在 `bsp/ui/ui_player.c`：`LY_HEAD 30` + 5×`LY_SLOT 30` + `LY_FOOT 32` ⇒ `LY_Y0 = 44`（槽位 44/74/104/134/164），当前行固定在 `LY_CUR_ROW = 2` 槽位（屏幕中间）；当前行 `ui_text15()` 1.5 倍放大 + `CLR_PRI` 高亮（行盒 `LY_BIG = 22` px，在 30 px 槽位里上下居中），其余行 1× `CLR_T2`。
放大渲染在 `bsp/ui/bsp_ui.c`：`ui_text_scl(x,y,s,fg,bg,num,den)` —— 输出 = 源 × num/den 的**最近邻采样**（`3/2` = 1.5 倍；`1/1` 与改前的整数实现逐像素等价，已用全字库回归证明），`ui_text()` / `ui_text15()` / `ui_text2()` 分别是 (1,1)/(3,2)/(2,1) 的壳，宽度函数 `ui_text15_w()` 逐字形取整以保证与绘制步进一致；`gy<0` 时从输出行 `r0=-gy` 开始裁。字形缓冲区 32×32（+1536 B RAM；1.5× 单字形只需 24×24）。
脏刷新：歌名 / 序号 / 总数 / 歌词条数变化 → 整页；只有当前行变化 → 只重画变化的槽位；**进度条每帧只补新增像素**（`ly_draw_progress()`），**左时间只在文本变化时擦 40×15 那一小块**（`ly_draw_time()`），底部不再整块重铺。
（旧实现是「秒一变就 `ly_draw_footer()` 铺整条 320×32 底板再重画」⇒ 进度条区域每秒黑一下，即用户看到的闪频。）

**主界面增量刷新**（本轮）：`draw_progress_delta()` / `draw_progress_time()`（进度条与左侧当前时间）、`draw_status_vol()`（状态栏音量数字，只擦固定 21 px 字段，`vol_field_w()` 同时让 SD/模式位置不随位数跳动）、`draw_ctrl_vol()`（控制栏音量数字，此前只在切播放/模式时才刷新 ⇒ 会残留旧值）、`draw_side_vol_delta()`（侧边音量条弹出一次后只补差额）、`ly_draw_vol()`（歌词页页脚音量读数，`hl=1` 高亮 1.5 s）；`ui_player_update()` 新增 `total_ms` 变化 → 信息区整块重画（换曲后右侧总时长与进度刻度刷新），歌单浮层打开时提前 return（缓存不动，关闭时按有效位局部恢复）。

**切页局部恢复**（本轮）：`ui_player.c` 新增有效位 `static uint8_t s_blk`（`BLK_STATUS 0x01` / `BLK_COVER 0x02` / `BLK_INFO 0x04` / `BLK_CTRL 0x08` / `BLK_ALL 0x0F`）。`draw_status()` / `draw_cover()` / `draw_info()` / `draw_controls()` 末尾各自 `s_blk |= BLK_xxx;`（因此 `ui_player_full()` 一行未改也照样把四块标为有效，仍是兜底）；`draw_status_vol()` / `draw_progress_delta()` / `draw_progress_time()` / `draw_ctrl_vol()` / `draw_side_vol_delta()` 开头各加一句 `if ((s_blk & BLK_xxx) == 0u) return;`，块无效时直接放弃、不在浮层 / 歌词页上乱画；`ui_player_update()` 的歌词行脏刷新也多了 `(s_blk & BLK_INFO) != 0u` 条件。
切页：`ui_player_list_open()` 只把 `COVER|INFO|CTRL` 置为无效（`draw_pl_full()` 从 `PL_TOP = TITLE_H = 26` 起清底 ⇒ 状态栏 y=0..25 没被覆盖）、`ui_player_lyrics_open()` 置 `s_blk = 0`（`ly_draw_full()` 整屏清底）；关闭走新增的 `restore_main(st, mask)`：先判断"没被恢复"的状态栏是否过期（本来就无效 ⇒ 整条重画；`playing` / `mode` / `sd_ok` 变了 ⇒ 整条重画；只有 `volume` 变了 ⇒ 只补 `draw_status_vol()` 那 21×15），再 `clear_main_bg()` 只擦四块底板铺不到的 5 条缝隙（上 11 px 横带 y=26..36、下 11 px 横带 y=181..191、封面左侧 x=0..7、封面与信息区之间 x=152..159、信息区右侧 x=312..319，共 10496 px），最后逐块重画并 `sync_state_cache()` 同步"上一帧状态"缓存。关歌单浮层传 `COVER|INFO|CTRL`（状态栏不重画、不闪），退歌词页传 `BLK_ALL`。回归 `verify_restore_p7.py` = **ALL PASS**。

**切页背光淡变**（本轮）：**背光由 GPIO 二值开关升级为 TIM2_CH3(PB10, AF1) 10 kHz 硬件 PWM** —— `bsp/lcd/bsp_lcd.h` 新增 `LCD_BLK_PIN_SRC/LCD_BLK_AF/LCD_BL_TIM/LCD_BL_PSC 83/LCD_BL_ARR 99/LCD_BL_FULL 100` 与 `lcd_backlight_init()` / `lcd_backlight_set(duty)` / `lcd_backlight_get()` / `lcd_backlight_fade(to, ms)`；`lcd_gpio_init()` 里先把 PB10 拉低（外部 10 kΩ 下拉 ⇒ 复位期间不亮）再切 AF，`lcd_backlight_init()` 先写 CCR=0 再 `TIM_Cmd`（不会一闪）；`lcd_set_backlight(on)` 保留为兼容包装（on→100、off→0），`LCD_BLK_ON/OFF` 宏在 PWM 模式下不再使用。上电时序不变：`app/main.c` 仍是全部初始化 + 首帧画完之后才点背光，只是 :226 改成 `lcd_backlight_fade(LCD_BL_FULL, 60u)`（60 ms 淡入；此时主循环未开始，阻塞无副作用）。
`bsp/ui/ui_player.c` 新增 `PG_FADE_MS 60` 与状态机 `pg_begin(act)` / `page_commit(st)` / `page_fade_poll(st, now_ms)`：四个对外切页函数（`ui_player_list_open/close`、`ui_player_lyrics_open/close`）不再当场重画，只 `pg_begin()` 起过渡；`page_fade_poll()` 挂在 `ui_player_update()` 最前面（早于歌词页 / 浮层两个早退），过渡期间整帧更新体直接 return（不会把中间帧画到错误的块上），`ui_player_list_move()` 与 `ui_player_vol_flash()` 同样被门闸挡住。占空比线性从 100 降到 0，**恰在占空比 0 的那一帧**才执行 `page_commit()`（`LIST_ON→draw_pl_full()` / `LIST_OFF→restore_main(COVER|INFO|CTRL)` / `LY_ON→ly_draw_full()` / `LY_OFF→restore_main(BLK_ALL)`），随后线性升回 100 才清 `s_pg_act`；单程 60 ms、一次切页 120 ms。`ui_player_full()` 一行未改、也绝不碰背光。
回归 `verify_fade_p8.py` = **ALL PASS**：A~D 44 项源码断言、E PWM 数学（84 MHz ÷ 84 ÷ 100 = **10.000 kHz**、`LCD_BL_FULL == ARR+1`、0 = 灭 / 100 = 全亮）、F 逐毫秒状态机仿真（四类切页各 commit 恰一次且都在占空比 0 那一帧、淡出单调不增 100,99,…,2,0、淡入单调不减且终值恰为 100、总时长 120 ms；淡出途中反悔只 commit 最后一次；淡入途中反悔两次 commit 都在全黑帧；连续乱按 20 次后 120 ms 内回到全亮）、G commit 复用的正是上一轮验证过的三个函数。预览出图 `out_fade_p8.png`（淡出 100/75/50/25/0 + 淡入 0/25/50/75/100 共 10 帧）。

### 9.5 ESP32 缓冲
输入环 64 KB（PSRAM）+ PCM 环 128 KB（PSRAM），起播预缓冲 24 KB（±139 ms），I2S DMA 8×512 B；软件音量 `s_vol_scale = vol*65536/100`；软限幅门限 −6 dBFS（16384），最大夹到 ±32000。

### 9.6 真实封面：迷你 baseline JPEG 解码器（本轮）
**为什么自写**：板上没有 framebuffer，内部 RAM 只有 128 KB，一张 500×500 的整帧 RGB 就要 500 KB。`app/cover.c` 的做法是**不存整图**：
1. `cv_scan()` 按 MCU 顺序解 8×8 块（`cv_sof/cv_dqt/cv_dht/cv_dri/cv_sos` 解析标记、`huff_build/huff_decode` 建表与解码、`idct_8x8` 是 `(sum+2048)>>12` 的可分离整数 IDCT）；
2. `cv_map_setup()` 先算 cover-fit 映射：`m = max(W,H)` 缩到 144（还有 `stride` 抽样）、`ncx/ncy` 与裁剪原点 `x0/y0`；**源图小于窗口容量时整张拒绝**（`CV_ACC_ROWS 18` 的推导写在 `cover.c:22-27`，例如 4:2:0 的最小可解边长是 136）；
3. `cv_place_row()` 把每个分量样本经 `ox/oy` 落到 144×144 的格子（`acc_put`），累加和/计数放 18 行滑动窗口（`s_acc[18][3][144]` + `s_accc[18][3][144]`）；
4. 窗口压满就 `win_flush_before()` / `emit_row()` 整行倒出：`fill_plane()` 做「整行无样本沿用上一行 / 前置空洞回填 / 空列取左邻」补洞 → YCbCr→RGB565（`rr = y + ((91881*cr)>>16)`、`gg = y - ((22554*cb + 46802*cr)>>16)`、`bb = y + ((116130*cb)>>16)` 再裁 0..255）→ 写 `s_px`；
5. 解码结束 `s_px` 就是能直接 `lcd_blit` 的 144×144 RGB565（`cover_pixels()`）。诊断接口：`cover_dec_ms()` 耗时、`cover_dbg_w/h()` 源图尺寸、`cover_dbg_src()` 来源（`"APIC"`/`"sidecar"`/`"-"`）、`cover_dbg_drop()` 窗口外丢弃样本数（正常恒 0）。

**来源顺序**（`cover_load()`）：`find_apic()` 先在 ID3v2 头里逐帧走（v2.2 6 字节帧头 / v2.3 10 字节 / v2.4 synchsafe 帧长，跳过 `0x40` 扩展头与 `0x10` footer，遇 `0x80` unsync 直接放弃），命中 `APIC`/`PIC` 且后面两字节是 `FF D8` 就把文件中那一段当独立 JPEG 解；没有再 `sidecar_try()` 按 **12 个候选**（8 个 `cover.*`/`folder.*` + 4 个 `<同名>.<后缀>`）逐个试开。任一步失败都返回 0、界面保留占位图标。
**接入**：`app/main.c:191` 上电 `cover_init()`、`set_track()` 里 `cover_begin(path)`（P10 起异步，见 §9.7；`cover_load()` 仍是同步包装）；`bsp/ui/ui_player.c:346 draw_cover()` 里 `cover_ready()` 成立就 `lcd_blit(COVER_X, COVER_Y, COVER_S, COVER_S, cover_pixels())`，否则画占位图标；另有 `ui_player_cover_refresh()`（`ui_player.c:1146`）供换曲后只重画封面块（走 §9.4 的 `BLK_COVER` 路径）。
**回归与预览**：`verify_cover_p9.py` = **PASS 194 / FAIL 0**（A~G 见 §6.4），其中 D 段用「只用已放置样本的暴力参考」重算窗口统计、与镜像逐格 `np.array_equal`，E 段用 numpy 独立重写装配链并与镜像逐像素相等；`render_cover_p9.py` 出 `out_cover_p9.png`（5 格：灰度 200×150 / 4:4:4 / 4:2:2 / 4:2:0 / 100×100 放大）。
**代价与余量**：静态内存 51.8 KB → **120.3 KB**（P11 起其中 63.7 KB 在 **CCM**，主板 SRAM 只占 ≈56.6 KB / 128 KB、余量 ≈71.4 KB，见 §9.8）、Flash +7.4 KB（Code +6968 B）；复杂度上这是全工程最需要上板实测的一块（1600×1600 约 0.5 s 同步解码，原会阻塞 UI 与 I2S 喂数据；**P10 起已改为分片异步**，见 §9.7）。

---

### 9.7 按键提示音与切歌提速（本轮）

**按键提示音（PB8 有源蜂鸣器，低电平触发）**
- ⚠ **P11 起默认静音**：`bsp_beep.h` 里 `#define BEEP_DEFAULT_EN 0u`，`beep_init()` 用它初始化 `s_en`，所以上电后所有 `beep_key()` / `beep_ms()` 都直接 return 不发声。要打开只需改该宏为 `1u` 或运行期调 `beep_set_enable(1)`，调用点和接线都不用动。
- 新增 `bsp/beep/bsp_beep.c/.h`：`beep_init()` 开 GPIOB 时钟 + PB8 配成**推挽输出、2 MHz、内部上拉**（复位期引脚为高 = **静音**，不会“上电就响”）；`beep_hw(on)` 里 **拉低 = 响、拉高 = 静音**。
- **非阻塞**：`beep_on(ms)` 只做两件事 —— 拉低 PB8、记 `s_until = tick_ms() + ms`；`beep_poll()`（主循环每轮调用）用 `(int32_t)(tick_ms() - s_until) >= 0` 判断到点后自动拉高。全文件**没有 `delay_ms`** ⇒ 连按 K1~K5 或旋钮连续转动都不会卡顿、不会“吃掉”下一次按键；用有符号差还顺带免疫 `tick_ms()` 回绕（49.7 天）。
- 时长：任意按键 / 旋钮每格 **30 ms**（`BEEP_MS_KEY`）；进 / 出歌单浮层与全屏歌词页 **60 ms**（`BEEP_MS_ACT`，听感上“确认”更明显）。`beep_set_enable(0)` 立即静音并忽略后续请求（可留作“静音提示音”开关）。
- 接入：`app/main.c` 上电 `beep_init()`；触发点 4 类 —— `key_scan()` 非空、编码器每格、进出歌单浮层、进出歌词页；`Project.uvprojx` 注册 `bsp_beep.c/.h` 且 IncludePath 补 `../../bsp/beep;`。

**切歌提速（封面解码分片异步）**
- 问题：P9 的 `set_track()` 在 `audio_srv_open()` **之前**同步跑 `cover_load()`，1600×1600 封面要 ≈0.5 s ⇒ 按 K3 后要等半秒才出声，且期间主循环完全卡住（界面不刷新、按键无响应）。
- 改法：`app/cover.c` 把单体 `cv_jpeg()` 拆成**可断点续跑**的两段 ——
  1. `cv_head()`：标记解析到 SOS（`cv_sof/dqt/dht/dri`、`cv_sos()` + `win_clear()` + 复位 `s_step_my/s_step_mx`），毫秒级完成；
  2. `cv_scan_step(budget_ms)`：按 MCU 粒度推进（每个 MCU 解完就 `if (++s_step_mx >= s_mcu_cols) { s_step_mx = 0u; s_step_my++; }` 存档断点；`budget_ms != 0` 且已用满 `(uint32_t)(tick_ms() - t0) >= budget_ms` 就返回 2 = 还要继续），全部解完才 `win_flush_before((int32_t)COVER_PX)` 收尾并返回 1；返回 0 = 数据错误。
  3. 对外 API：`cover_begin(path)`（定位源 + 解头，1 = 已开始 / 0 = 没有封面）、`cover_poll(budget_ms)`（0 = 进行中 / 1 = 完成 / 2 = 失败）、`cover_busy()`；`cover_load()` 退化为 `cover_begin()` + 循环 `cover_poll(0u)` 的同步包装，**语义与原实现完全一致**（P9 回归不受影响）。
- `app/main.c` 的顺序改为：`mp3_duration_ms()` → **`audio_srv_open()` + `audio_srv_set_playing()`（先出声）** → `lrc_load()` → `cover_begin(path)` 后刷一次封面区（解码期间保持占位图）→ `printf("[MP3] ...")`；主循环里 `if (cover_busy()) { cr = cover_poll(2u); ... }`，每帧最多花 2 ms，完成时打 `[COVER]` 诊断并 `ui_player_cover_refresh()`。
- 效果：按 K3 → 出声从 **≈500 ms 降到 ≈40 ms**（时长解析 + `audio_srv_open`），封面在最坏 0.5 s 内由主循环分片补上，全程界面刷新、按键与提示音正常。
- 回归：`verify_cover_p10.py` = **PASS 154 / FAIL 0** —— 除了源码 / 接线断言，B 段对 8 类 JPEG（灰度 200×150、4:4:4 / 4:2:2 / 4:2:0 240×180、4:2:0 239×181、灰度 100×100 放大、4:4:4 320×240、带 DRI 重启的 200×160）做**分片等价性**：budget=1 时每解 1 个 MCU 就返回（调用次数恰为 `mcu_rows × mcu_cols`）、budget=0 一次解完、budget=3、时钟抖动四种节奏下，`px` / `rgb` 与一次性解码**逐像素完全相等**；错误路径（截断图 / 未 begin 就 poll / 渐进式 / 同步包装）也都覆盖。预览图 `render_beep_p10.py` → `out_beep_p10.png`（PB8 波形 30 / 60 ms + 切歌时间线 P9 vs P10 对比）。

### 9.8 死代码清理与 CCM 搬 RAM（本轮）

**为什么动 RAM**：P9 的真实封面把静态 RAM 从 51.8 KB 抬到 120.3 KB / 128 KB，主板 SRAM 余量只剩 ≈7.7 KB —— 再加一个 4 KB 缓冲就可能溢出。片内其实还有 64 KB **CCM（0x10000000）**，在原来的 `Objects/Project.sct` 里一直没被使用：它挂在 CPU 数据总线上（比 SRAM 更快、延迟确定），**不能做 DMA 源/目标**，所以只适合放"CPU 自己读写、DMA 不碰"的数据。本工程确认**无任何外设 DMA**（`bsp/lcd/bsp_lcd.c:13` 注释 *No DMA yet*，LCD 用 `lcd_blit` 直接写；`bsp/sdio/bsp_sdio.c` 轮询 FIFO + CMD17/24），封面解码缓冲正是理想候选。

**做法（不牺牲任何功能）**
- `app/cover.c:299-307` 新增 `#define CV_CCM __attribute__((section(".ccmram"), zero_init))`，四个数组加后缀：`s_px[144*144]` 41472 B、`s_acc[18][3][144]` 15552 B、`s_accc[18][3][144]` 7776 B、`s_prev[3][144]` 432 B，合计 **65232 B**。`zero_init` 让它们变成 `SHT_NOBITS`（`fromelf -z` 实测 `s_px@0 / s_acc@0xa200 / s_accc@0xdec0`）⇒ **不占 Flash 初始化数据**。
- 新增手写分散加载文件 `stm32/project/MDK(V5)/Project_ccm.sct`：`LR_IROM1 0x08000000 0x00080000` 里除 `ER_IROM1 …` 外，把 `.ccmram` 定点放到 `RW_CCM 0x10000000 0x00010000`，主板 SRAM 仍是 `RW_IRAM1 0x20000000 0x00020000 { .ANY (+RW +ZI) }`。
- `Project.uvprojx` 相应改为 `umfTarg=0` / `useFile=1` / `ScatterFile=Project_ccm.sct`（原先用 Target 对话框内存布局、`ScatterFile` 为空）。
- 在 `app/cover.c` 四块缓冲下面加了一条**编译期容量自检**：`#if ((COVER_PX*COVER_PX*2u) + (CV_ACC_ROWS*3u*COVER_PX*3u) + (3u*COVER_PX)) > 0x10000u` → `#error` —— 以后谁再加大封面缓冲，**编译期就报错**，而不是等链接器报 `RW_CCM region overflow`（RAM 余量不会再被默默吃掉）。

**结果**（`p2build.py` 全量构建，armcc/armlink 参数与 uVision 一致）
- `Program Size: Code=55348 RO-data=332528 RW-data=564 ZI-data=122604`；
- `p2.map`：`RW_CCM 0x10000000 Size 0x0000fed0 Max 0x00010000`（65232 B + 对齐，余 304 B）、`RW_IRAM1 0x20000000 Size 0x0000e250` = 57936 B ⇒ **主板 SRAM 余量 73136 B ≈ 71.4 KB**（改前 7.7 KB）；Flash ≈388.4 KB / 余量 ≈123.6 KB。

**同时删掉的无调用点函数（19 个，删前逐个在工程内 grep 确认 0 引用）**
`key_down`、`enc_sw_down`、`lcd_test_pattern`、`ui_text2`、`ui_text2_w`、`lrc_path`、`sd_card_type`、`uart2_puts`、`uart2_tx_flush`、`proto_crc16`（内部 `crc16_update` 保留，`proto_send` 在用）、`proto_rx_ok`、`audio_srv_total_ms`、`audio_srv_is_open`、`audio_srv_playing`、`delay_1ms`、`delay_1us`（`delay_ms/delay_us` 保留）及配套头声明。**刻意保留**：`fputc`（microlib 的 printf 重定向，删了串口日志全没）、`lcd_backlight_get` / `ui_player_vol_flash` / `beep_busy / beep_set_enable / beep_enabled`（回归脚本断言 + 可留作静音开关）；第三方库（FatFS / StdPeriph / CMSIS / Helix）不动，其未用 API 由链接器丢弃。

**顺带清掉的无用文件**：仓库根目录 19 个散落 `.o`；`Objects` 里旧"双板示波器"模板留下的 `bsp_adc / bsp_buzzer / bsp_oled / bsp_scope` 的 `.crf/.d/.o`（12 个）与 `Project_Project.dep`；一轮性实验脚本（`ccm_probe*` / `unused*` / `rm_*` / `dbg_*` / `junk_scan*` / `check_docx_p3~p10` 等）。`_dsh_tmp` 里可复现的验证链全部保留（`cvjpeg.py` + `verify_*` + `render_*` + `docgen_*` + `p2build.py` + 预览 PNG + 最新 `p2.map`）。

**回归（9 个脚本全绿）**：`verify_cover_p9.py` PASS 194 / FAIL 0、`verify_cover_p10.py` PASS 154 / FAIL 0、`verify_delta_p6.py`、`verify_fade_p8.py`、`verify_progress.py`、`verify_text15.py`、`verify_font_p2.py`（GB2312 一级 3755/3755）、`verify_delivery.py` 全 **ALL PASS**；`verify_restore_p7.py` 的两条断言随 P8 背光淡变把恢复动作移进 `page_commit()` 而失效，本轮同步为「`ui_player_list_close()` 走 `pg_begin(PG_ACT_LIST_OFF)`、`ui_player_lyrics_close()` 走 `pg_begin(PG_ACT_LY_OFF)`，对应恢复动作在 `page_commit()` 里分别是 `restore_main(st,(uint8_t)(BLK_COVER|BLK_INFO|BLK_CTRL))` 与 `restore_main(st,BLK_ALL)`，两条路径都不得出现 `ui_player_full()`」，改后 **ALL PASS**。

---
## 10. 代码整理记录

- `bsp/ui/bsp_ui.c/.h` 原先残留整套示波器屏幕层（画布/网格/波形/触发条/测量卡片/模态菜单，共约 835 行）。项目转向 MP3 播放器后这些函数**已无任何调用点**，本次精简为纯 UTF-8 文本渲染层（`ui_text` / `ui_text_w`，约 150 行），播放器界面全部由 `bsp/ui/ui_player.c` 负责。
- ESP32 侧每秒一条的 `pcmq` 统计一度收进编译开关 `-DDBG_STATS`（默认 0）；**开源整理时该开关、统计与 5 秒一条的 `[PLAY] alive` 全部删除**，日志只保留启动横幅、关键状态与错误。
- 本次改动后：STM32 **armcc --c99 全量 62 个 .c，0 错 0 警告**；ESP32 **构建 SUCCESS**，RAM 7.3% / Flash 10.3%。
- **P2 新增/改动**：新增 `app/lyrics.c/.h`（.lrc 解析 + 行定位）与 `bsp/encoder/bsp_encoder.c/.h`（TIM8 硬件编码器）；`bsp/ui/ui_player.c` 歌词区改为真实滚动 + 控制栏改版；`app/main.c` 按键重映射（K1=播放/暂停、K4=歌单浮层、K5=切换播放模式）并接入编码器；`ui_font_cn14.c/.h` 扩到 3973 字形。改动后 **64 个 .c，0 问题**，`Code=42968 RO-data=332180 RW-data=332 ZI-data=50140`。
- **歌词体验修复（本轮）**：① `bsp/ui/ui_player.c` 歌词区改**脏刷新**（4 行逐行比较 + `s_lyr[4][LRC_LINE_LEN]` 缓存，只有变化行重绘，64 个 .c 0 问题，`Code=43236 RO-data=332180 RW-data=336 ZI-data=50520`）；② 定案「高亮比声音慢」的根因是 ESP32 位置上报把 PCM 环内待播余量扣了两遍，改为 `pm = s_written_us/1000 + s_base_ms`。**ESP32 侧本次未能在本机重编译**（PlatformIO 要写 `C:\Users\hxh\.platformio` 且要联网补装工具包，本机两条都不通），请在你自己的开发机上 `pio run -e esp32s3` 复核后烧录。
- **键位互换 + 歌单脏刷新（本轮）**：按用户要求把 **K1 与 K5 的功能对调**（K1=播放/暂停，K5=切播放模式；歌单内 K1 仍是"选中并播放"）；`bsp/ui/ui_player.c` 新增 `draw_pl_rows()`、`ui_player_list_move(dir, st)` 改为**脏刷新**（只重画旧/新选中两行，翻页只重画 6 行，到顶/到底零重画，背景/头/脚不再重画）。改动后 **64 个 .c，0 问题**，`Code=43384 RO-data=332180 RW-data=336 ZI-data=50520`（Flash ≈375.5 KB / 512 KB、RAM ≈49.7 KB / 128 KB）。
- **全屏歌词模式（本轮）**：控制栏新增 **[歌词]** 按钮与全屏歌词页（`bsp/ui/ui_player.c` + `bsp/ui/bsp_ui.c` 新增的 2 倍缩放渲染器，后一轮已按用户要求改为 1.5×）；旋钮按下 **PD12 由「播放/暂停」改为「歌词模式」开关**，播放/暂停保留在 K1（键位表已同步）。改动后 **64 个 .c，0 问题**，`Code=45308 RO-data=332188 RW-data=356 ZI-data=52620`（Flash ≈377.8 KB / 512 KB、RAM ≈51.7 KB / 128 KB）。
- **全屏歌词页进度条防闪（本轮）**：旧实现每整秒用 `bar(0, y, LCD_W, LY_FOOT, CLR_BG)` 铺整条 320×32 底板再重画，进度条区域每秒黑一下 ⇒ 闪频。改为 `ly_draw_progress()` 只补「新增的那一段」像素（前进补新段 / 回退只擦多出的段）+ `ly_draw_time()` 只在时间文本变化时擦 40×15 那一小块（缓存 `s_ly2_fw`、`s_ly2_tstr`，进页 / 初始化时复位）；轨道 / 边框 / 中间提示一次画好不再重画。改动后 **64 个 .c，0 问题**，`Code=45640 RO-data=332188 RW-data=360 ZI-data=52632`。
- **全屏歌词页当前行放大由 2× 改为 1.5×（本轮）**：把 `bsp/ui/bsp_ui.c` 的整数倍缩放渲染器泛化为**分数** `ui_text_scl(x,y,s,fg,bg,num,den)`（输出 = 源 × num/den 最近邻），新增 `ui_text15()/ui_text15_w()` = (3,2)，全屏歌词页当前行改用 1.5×（行盒 `LY_BIG = 22` px，居中于 30 px 槽位），`fit_text` 上限由 `(LCD_W-24)/2` 改为 `(LCD_W-24)*2/3`。验证：`verify_text15.py` 全字库 3973 字形 × 7 位置证明 **1× / 2× 路径与改前逐像素一致**、1.5× 与独立逆映射参考一致、放大不漏源行/列；`render_lyrics_p2.py` 重新逐像素复刻 C 渲染：12 字行 1× 168 px → 1.5× 252 px（x=34..286）、超长行截到 14 字加 `…`（312 px，两侧各 4 px）、墨迹 y=109..129 落在槽 104..133 内。改动后 **64 个 .c，0 问题**，`Code=45756 RO-data=332188 RW-data=360 ZI-data=52632`（Flash ≈377.9 KB / 512 KB、RAM ≈51.8 KB / 128 KB）。
- **主界面进度条 + 音量显示改增量刷新，并给歌词页加音量读数（本轮）**：`bsp/ui/ui_player.c` 新增 `draw_progress_delta()` / `draw_progress_time()` / `draw_status_vol()` / `draw_ctrl_vol()` / `draw_side_vol_delta()` / `ly_draw_vol()`（返回值改为 void 的 `draw_progress_only()` 被移除）与缓存 `s_pr_fw`、`s_pr_tstr`、`s_st_vol`、`s_cb_vol`、`s_sv_txt`、`s_sv_fh`、`s_ly2_vol/_w/_hl/_t`，歌词页 `ly_refresh(st, now_ms)` 新增音量分支（值变 → 高亮 1.5 s → 恢复常规色）。用 `verify_delta_p6.py`（复刻 C 渲染、逐像素比对「增量刷新后」与「从头全量重画」）发现并修掉三处真实缺陷：① 状态栏音量数字按实际位数算擦除宽度 ⇒ 9→100 时新数字的擦除区会盖掉 "SD" 的尾巴，改为 `vol_field_w()` 固定 21 px 字段；② 歌词页音量块缓存改存「已画出的块宽」（`s_ly2_vol_w = we`）⇒ 3 位读数后调小到 1 位、高亮到期时会留一小块高亮残影；③ `ui_player_update()` 新增 `total_ms` 变化触发信息区整块重画（否则换曲后右侧总时长/进度刻度不刷新）。回归 A 21 / B 18 / C 391 / D 37 / E 399 帧全部 **ALL PASS**；增量性：单帧最多写 616 px（主界面进度条）与 3107 px（歌词页页脚），全量分别为 ≈21888 px 与 10240 px。改动后 **64 个 .c，0 问题**，`Code=46912 RO-data=332188 RW-data=400 ZI-data=52640`（Flash ≈379.0 KB / 512 KB、RAM ≈51.8 KB / 128 KB）。
- **切页由「整屏清屏」改为「只重画被覆盖区域」（本轮）**：`bsp/ui/ui_player.c` 新增主界面四块有效位 `s_blk`（`BLK_STATUS/COVER/INFO/CTRL`，整块重画函数末尾自置位）+ 三个静态函数 `sync_state_cache()` / `clear_main_bg()` / `restore_main()`；五个增量刷新函数开头加 `s_blk` 放弃守卫，`ui_player_update()` 歌词脏刷新加 `BLK_INFO` 条件；`ui_player_list_open()` 只失效被浮层盖住的三块，`ui_player_list_close()` / `ui_player_lyrics_close()` 改调 `restore_main()`（不再整屏清屏），`ui_player_full()` 一行未改、仍是启动期与兜底路径（`app/main.c:175/180/214/219`）。新增 `verify_restore_p7.py`（复用 verify_delta_p6.py 模型）：22 项源码断言 + 覆盖性（关浮层 68480/68480 px、退歌词页 76800/76800 px，0 空洞）+ 被覆盖区域填垃圾像素后与整屏兜底参考帧**全屏逐像素 0 差异**（6 场景）+ 浮层 / 歌词页期间增量刷新写入 0 px + 写入量对比（关浮层 93974 px = 兜底 170271 px 的 55%，退歌词页 103967 px = 61%；擦除步 10496 px vs 76800 px）全部 **ALL PASS**。改动后 **64 个 .c，0 问题**，`Code=47428 RO-data=332188 RW-data=400 ZI-data=52640`（Flash ≈380.0 KB / 512 KB、RAM ≈51.8 KB / 128 KB；较上一版 +516 B）。
- **切页加极短背光淡入/淡出过渡（本轮）**：`bsp/lcd/bsp_lcd.h/.c` 把 LCD_BLK（PB10）从 GPIO 开关改成 **TIM2_CH3 AF1 10 kHz PWM**（`LCD_BL_PSC 83` / `LCD_BL_ARR 99` / `LCD_BL_FULL 100`、`lcd_backlight_init/set/get/fade`，`lcd_set_backlight(on)` 保留为兼容包装；`LCD_BLK_ON/OFF` 宏在 PWM 模式下不再使用），`lcd_gpio_init()` 里先 GPIO 拉低（PB10 外部 10 kΩ 下拉）再切 AF、先写 CCR=0 再 `TIM_Cmd`；`app/main.c:226` 上电改 `lcd_backlight_fade(LCD_BL_FULL, 60u)`（在最后一次 `ui_player_full()` 之后、`for(;;)` 之前，主循环未开始所以阻塞无副作用）；`bsp/ui/ui_player.c` 新增 `PG_FADE_MS 60` 与 `pg_begin()` / `page_commit()` / `page_fade_poll()`（四类切页都变成「淡出 → 全黑那一帧换画面 → 淡入」，过渡期间 `ui_player_update()` 直接 return，`ui_player_list_move()` / `ui_player_vol_flash()` 都被门闸挡住；`ui_player_full()` 一行未改、不碰背光）。`verify_fade_p8.py` 全部 **ALL PASS**（源码断言 + PWM 数学 + 逐毫秒状态机仿真）。改动后 **64 个 .c，0 问题**，`Code=48072 RO-data=332188 RW-data=412 ZI-data=52644`（Flash ≈380.7 KB / 512 KB、RAM ≈51.8 KB / 128 KB；较上一版 +644 B）。

- **真实封面（迷你 baseline JPEG 解码器）（本轮）**：新增 `app/cover.c/.h`（Huffman/反量化/整数 IDCT + cover-fit 窗口降采样 + ID3v2 APIC/sidecar 定位，约 1100 行），`app/main.c:191/:124` 接上 `cover_init()`/`cover_load()`，`bsp/ui/ui_player.c:346 draw_cover()` 改为「`cover_ready()` 就 `lcd_blit` 真封面，否则占位图标」并新增 `ui_player_cover_refresh()`；新增 `cvjpeg.py`（cover.c 逐位镜像）+ `verify_cover_p9.py`（**194 项 ALL PASS**）+ `render_cover_p9.py` / `out_cover_p9.png` 预览。改动后 **65 个 .c，0 问题**，`Code=55040 RO-data=332512 RW-data=544 ZI-data=122608`（Flash ≈388.1 KB / 512 KB、余量 ≈123.9 KB；RAM ≈120.3 KB / 128 KB、余量 ≈7.7 KB）。**待办**：cover 解码目前在 `set_track()` 里同步跑（500×500 约 60 ms、1600×1600 约 0.5 s），上板后若发现切歌卡顿，可把 `cover_load()` 挪到换曲后的空闲帧分块执行。

- **P10 按键提示音（有源蜂鸣器，PB8 低电平触发）（本轮）**：新增 `bsp/beep/bsp_beep.c/.h`（PB8 推挽输出、低 = 响 / 高 = 静音、内部上拉保证复位期静音；`beep_init/beep_on/beep_off/beep_poll/beep_busy/beep_key/beep_ms/beep_set_enable/beep_enabled`，`BEEP_MS_KEY 30u` / `BEEP_MS_ACT 60u`，全程非阻塞、无 `delay_ms`）；`Project.uvprojx` 注册 + IncludePath 加 `../../bsp/beep;`，`app/main.c` 上电 `beep_init()`、K1~K5 / 旋钮每格 / 进出歌单浮层与歌词页 4 类触发点、主循环每轮 `beep_poll()`；接线图同步加 PB8 蜂鸣器节点与 §4 接线表。改动后 **66 个 .c，0 问题**，`Code=55680 RO-data=332512 RW-data=564 ZI-data=122604`（Flash ≈388.8 KB / 余量 ≈123.2 KB、RAM ≈120.3 KB / 余量 ≈7.7 KB）。
- **P10 切歌提速（封面解码分片异步）（本轮）**：`app/cover.c` 把单体 `cv_jpeg()` 拆成 `cv_head()`（解析到 SOS）+ `cv_scan_step(budget_ms)`（MCU 粒度断点 `s_step_my/s_step_mx`、返回 2 = 继续），`cv_jpeg()` 删除；对外新增 `cover_begin/cover_poll/cover_busy`，`cover_load()` 保留为 `begin + poll(0)` 同步包装；`app/main.c` 把 `audio_srv_open()` 提到封面解码之前（先出声），主循环 `cover_poll(2u)` 分片补封面。回归 `verify_cover_p10.py` = **PASS 154 / FAIL 0**（8 类 JPEG 在 budget=1/0/3/抖动时钟下与一次性解码逐像素完全相等），预览 `out_beep_p10.png`。
- **P11 死代码清理 + CCM 搬 RAM（本轮）**：删掉全工程 0 引用函数的 19 处（见 §9.8），封面 4 个静态缓冲 65232 B 用 `.ccmram` 段搬进 **CCM（0x10000000 / 64 KB）**，新增 `Project_ccm.sct` 并把 uvprojx 切到它；同时清掉根目录散落 `.o`、`Objects` 里旧示波器模板产物与一轮性实验脚本。改动后 **66 个 .c，0 问题**，`Code=55348 RO-data=332528 RW-data=564 ZI-data=122604`（Flash ≈388.4 KB / 余量 ≈123.6 KB；主板 SRAM 只占 ≈56.6 KB / 128 KB、**余量 ≈71.4 KB**），9 个回归脚本全 ALL PASS。

---

本说明与规划书（[`docs/roadmap.md`](roadmap.md)）、接线图（[`hardware/wiring-diagram.html`](../hardware/wiring-diagram.html)）共同构成交付文档；界面以 [`hardware/tft-ui-demo.html`](../hardware/tft-ui-demo.html) 为验收基线。
