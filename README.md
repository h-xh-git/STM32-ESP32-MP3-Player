# 双板 MP3 播放器（STM32F407 + ESP32-S3）

[![MCU](https://img.shields.io/badge/MCU-STM32F407VET6-03234B.svg)](#22-stm32f407-%E4%BE%A7)
[![Codec](https://img.shields.io/badge/Codec-ESP32--S3--N16R8-E7352C.svg)](#23-esp32-s3-%E4%BE%A7)
[![License](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

> 一块 **STM32F407** 负责界面、按键、TF 卡与文件系统；一块 **ESP32-S3** 负责 MP3 解码、I2S 功放与 WiFi。
> 两板之间用一根 1 Mbps 串口传 MP3 压缩流（STM32 按需拉取），另用一条 **SPI 链路**把手机网页上传的歌曲写进 TF 卡。
> 没有 RTOS、没有动态内存，两个板子都是裸机主循环 + 中断 + DMA。

**English.** A two-board MP3 player: an STM32F407 does UI, keys, SDIO/FatFs and file streaming; an ESP32-S3 does MP3 decoding (Helix fixed-point), I2S output and WiFi. A 1 Mbps UART link carries the compressed MP3 stream, and a separate 5-wire SPI link lets the ESP32 write songs uploaded from a phone browser onto the STM32 side TF card. Keil MDK5 (ARMCC AC5) for the STM32, PlatformIO/Arduino for the ESP32. MIT licensed.

---

## 目录

- [1. 功能特性](#1-功能特性)
- [2. 硬件与接线](#2-硬件与接线)
- [3. 目录结构](#3-目录结构)
- [4. 编译与烧录](#4-编译与烧录)
- [5. 使用说明](#5-使用说明)
- [6. 两板通信协议](#6-两板通信协议)
- [7. Flash 与内存布局](#7-flash-与内存布局)
- [8. 已知限制](#8-已知限制)
- [9. 第三方组件与许可](#9-第三方组件与许可)
- [10. 文档索引](#10-文档索引)

---

## 1. 功能特性

**播放**

- TF 卡 MP3 播放：SDIO 4-bit @12 MHz + FatFs R0.16，优先扫描 `/MUSIC` 目录，最多 300 首
- 歌单自然排序（数字按数值大小排，`track2` 排在 `track10` 前面），浮层式歌单界面
- MP3 文件由 STM32 按信用窗口（credit window）分块推给 ESP32 解码，ESP32 侧 64 KB 压缩环 + 128 KB PCM 环两级缓冲，播放不卡顿
- 3 种播放模式：顺序 / 单曲 / 随机；播完自动下一首
- 音量 0~100（旋转编码器，每格 5），按键提示音可开关

**显示（2.4 寸 ST7789 320×240 横屏）**

- 主界面：状态栏 / 封面 / 曲目信息 / 进度条 / 控制栏；进度条与音量只做**增量刷新**（只补/擦差额像素）
- 封面：ID3v2 APIC 内嵌图，或同目录 sidecar 图片；自写迷你 baseline JPEG 解码器，**分片异步**解码（8×8 块逐块解码），不阻塞播放
- 歌词：`.lrc` 滚动歌词，**UTF-8 / GBK 自动识别**；全屏歌词页当前行 1.5× 放大高亮
- 背光 PWM（TIM2_CH3, 10 kHz）淡入淡出；切页只重画被覆盖区域

**断点续播（原始需求）**

- 关机前把「当前曲目序号 + 已播位置」写进 **F407 片内 Flash Sector 7**，上电恢复
- 写成追加式环形记录（96 B/条，带 magic + CRC16 校验），每秒一个检查点；掉电最坏只丢失最后一个检查点（≤1 s）
- 掉电时不需要检测电路：被写坏的条目会在上电扫描时按 CRC 丢弃

**联网**

- 手机网页上传：浏览器里选 `mp3/wav` 文件 → ESP32 → SPI → STM32 写进 TF 卡（不经过电脑）
- 多 WiFi 管理：最多 8 组 SSID/密码存进 **ESP32 NVS**，网页表单增删改，开机按序连接
- 所有 WiFi 都连不上时自动开 AP 热点（`ESP32-MP3-Setup` / `mp3setup123`）+ 强制门户，手机连上热点即弹出配置页
- 两板都用串口打印运行状态（STM32 115200，ESP32 115200）

## 2. 硬件与接线

### 2.1 物料清单

| 数量 | 器件 | 说明 |
| --- | --- | --- |
| 1 | STM32F407VET6 开发板 | 主控：UI、按键、SDIO/FatFs、SPI 从机、断点续播 |
| 1 | ESP32-S3-N16R8 开发板 | 16 MB Flash + 8 MB OPI PSRAM，MP3 解码 / I2S / WiFi / SPI 主机 |
| 1 | ST7789 2.4" SPI 屏 | 320×240，BGR 面板 |
| 1 | MAX98357A | I2S 功放，接 8 Ω / 3 W 喇叭 |
| 1 | TF 卡 | FAT32，歌曲放 `/MUSIC`（`.mp3`），歌词同名 `.lrc` |
| 5 | 轻触按键 | K1~K5，另一端接 GND |
| 1 | 旋转编码器 | A/B 相 + 按压开关 |
| 1 | 有源蜂鸣器（可选） | 3 脚，低电平触发；固件默认关闭 |

### 2.2 STM32F407 侧

| 功能 | 引脚 | 备注 |
| --- | --- | --- |
| TF 卡（SDIO 4-bit） | PC8 D0 / PC9 D1 / PC10 D2 / PC11 D3 / PC12 CK / PD2 CMD / PD3 CD | 12 MHz，卡检测用 PD3 |
| LCD（SPI1） | PA5 SCK / PA7 MOSI / PA4 CS / PB0 DC / PB1 RST / PB10 BLK | 背光走 TIM2_CH3 PWM 10 kHz |
| 按键 K1~K5 | PC0 / PC1 / PC2 / PC3 / PC4 | 内部上拉，按下接地 |
| 旋转编码器 | PC6 A / PC7 B / PD12 按下 | TIM8 编码器模式 |
| 蜂鸣器 | PB8 | 低电平响，`BEEP_DEFAULT_EN=0` 默认静音 |
| 板间串口 USART2 | PA2 TX → ESP32 GPIO17 / PA3 RX ← ESP32 GPIO18 | 1 Mbps，8N1 |
| 板间 SPI2 从机 | PB13 SCK / PB14 MISO / PB15 MOSI / PB12 CS / PB11 READY | CS 为普通 GPIO + EXTI12（软件 NSS） |
| 调试串口 USART1 | PA9 TX / PA10 RX | 115200，仅打印日志 |
| SWD | PA13 SWDIO / PA14 SWCLK | 下载与调试 |

### 2.3 ESP32-S3 侧

| 功能 | 引脚 | 备注 |
| --- | --- | --- |
| I2S 输出 | GPIO5 BCLK / GPIO6 LRCK / GPIO7 DIN | → MAX98357A |
| 功放使能 | MAX98357A 的 SD/MODE 直接接 3V3 | 功放常开、只出左声道；固件已下混为单声道，不丢内容；GAIN 悬空 = 9 dB |
| 板间串口 UART1 | GPIO17 RX ← STM32 PA2 / GPIO18 TX → STM32 PA3 | 1 Mbps |
| 板间 SPI 主机 | GPIO12 SCK / GPIO11 MOSI / GPIO13 MISO / GPIO10 CS / GPIO9 READY（输入） | 8 MHz，固定 2064 B 帧 |
| 状态灯 | GPIO48 | 绿=播放，黄=缓冲，蓝=停止 |
| 串口控制台 | UART0 GPIO43/44 | 板载 CH340，115200 |

> **注意**：`MAX98357A` 的 `SD/MODE` 必须接 3V3（或由 MCU 拉高）。实测用 GPIO8 经电阻驱动时电平不稳，喇叭有沙沙声，因此本工程直接把 SD 接 3V3、固件里 `PIN_AMP_SD = -1`，并把立体声下混成单声道写入左右两槽。

### 2.4 板间连线

```
UART（音频流，5 线）：STM32 PA2 -> ESP32 GPIO17
                        STM32 PA3 <- ESP32 GPIO18
                        GND 共地
SPI（写卡，6 线）：    STM32 PB13 <- ESP32 GPIO12  SCK
                        STM32 PB14 -> ESP32 GPIO13  MISO
                        STM32 PB15 <- ESP32 GPIO11  MOSI
                        STM32 PB12 <- ESP32 GPIO10  CS
                        STM32 PB11 -> ESP32 GPIO9   READY
                        GND 共地
```

接线图与界面预览见 [`hardware/`](hardware/)（浏览器直接打开 HTML）。

## 3. 目录结构

```
STM32-ESP32-MP3-Player/
├─ stm32/                     STM32F407 工程（Keil MDK5 / ARMCC AC5）
│  ├─ app/                    main.c、音频服务、歌单、歌词、封面、断点续播、SPI 写卡服务
│  ├─ board/                  时钟与板级初始化
│  ├─ bsp/                    lcd / key / beep / encoder / sdio / uart2 / proto / spid / storage / ui
│  ├─ FatFS/                  FatFs R0.16 + SDIO diskio
│  ├─ libraries/              CMSIS + STM32F4xx 标准外设库
│  ├─ module/                 stm32f4xx_conf.h / 中断向量
│  └─ project/MDK(V5)/        uVision 工程 + 自定义 scatter（CCM 放封面缓冲）
├─ esp32/                     ESP32-S3 工程（PlatformIO / Arduino）
│  ├─ src/main.cpp            初始化 + 主循环
│  ├─ lib/link/               UART 链路驱动（帧、CRC16、信用窗口）
│  ├─ lib/proto/              控制命令与状态同步
│  ├─ lib/player/             Helix 解码 + I2S 输出 + 双环缓冲（播放器核心）
│  ├─ lib/mp3dec/             Helix 定点 MP3 解码器（RealNetworks，第三方）
│  ├─ lib/netdl/              SPI 主机 + 网页上传（`/upload`、`/upstatus`）
│  ├─ lib/wifimgr/            多 WiFi（NVS 持久化 + 网页配置 + AP 兜底）
│  └─ platformio.ini
├─ docs/                      协议说明、WiFi 说明、交付记录、开发规划
└─ hardware/                  接线图、TFT 界面预览（HTML）
```

## 4. 编译与烧录

### 4.1 STM32F407（Keil MDK5）

1. 用 Keil MDK5 打开 `stm32/project/MDK(V5)/Project.uvprojx`
2. 编译器选 **ARM Compiler 5（AC5）**：工程使用 `--c99` 与标准外设库，`AC6` 会因严格 C 规则报错
3. 宏定义 `USE_STDPERIPH_DRIVER`, `STM32F40_41xxx`；散列文件 `Project_ccm.sct`（`.ccmram` 段放 CCM，用于封面缓冲）
4. Build（F7）→ Download（ST-Link / J-Link，SWD PA13/PA14）

串口 `USART1`（PA9/PA10，115200 8N1）输出运行日志。

本仓库代码实测（ARMCC `-O1` + armlink 全量重链接）：`Code 49648 + RO-data 174792 + RW-data 624` = **Total ROM 224556 B（219.3 KB）**，`ZI-data 127536 B（124.5 KB）`（主 SRAM），CCM 64 KB 里用掉 ~63.7 KB。RO-data 里约 16 KB 是 `app/gbk2312_map.h` 的 GB2312 码表。详见 [§7](#7-flash-与内存布局)。

### 4.2 ESP32-S3（PlatformIO）

```bash
cd esp32
pio run                 # 编译
pio run -t upload       # 烧录（板载 USB-C，CH340）
pio device monitor -b 115200
```

`platformio.ini` 关键项：

```ini
platform = espressif32@6.9.0
board    = esp32-s3-devkitc-1        ; N16R8：16 MB QIO Flash + 8 MB OPI PSRAM
framework = arduino
board_build.arduino.memory_type = qio_opi
build_flags = -DBOARD_HAS_PSRAM -mfix-esp32-psram-cache-issue -DPIN_AMP_SD=-1
```

> 不要把 `Serial` 切到原生 USB（不要再定义 `ARDUINO_USB_MODE=0`），否则 CH340 口看不到日志。

## 5. 使用说明

### 5.1 按键

| 界面 | 操作 | 功能 |
| --- | --- | --- |
| 播放界面 | K1 | 播放 / 暂停 |
| 播放界面 | K2 / K3 | 上一曲 / 下一曲（到头不回绕，防止误触跳歌） |
| 播放界面 | K4 | 打开歌单 |
| 播放界面 | K5 | 切换播放模式（顺序 / 单曲 / 随机） |
| 播放界面 | 旋转 / 按下旋钮 | 音量 ±5 / 进出全屏歌词页 |
| 歌单 | K2 / K3 | 上移 / 下移 |
| 歌单 | K1 | 选中并播放 |
| 歌单 | K4 / K5 | 返回 |

### 5.2 断点续播

- 播放中每秒把「曲目序号 + 已播毫秒数」写进片内 Flash Sector 7；切歌、暂停、收到停止命令时立即补写一次
- 上电后自动回到上次的曲目与位置，界面直接显示该位置；**默认暂停**，按 K1 继续
- 记录里同时存了文件名，上电时会跟当前歌单核对，文件被删/改名就不会错误恢复
- 歌单为空或没有有效记录时写一条「墓碑」记录，避免下次开机误恢复

### 5.3 手机网页上传歌曲

1. 手机连上与 ESP32 同一个 WiFi（或 ESP32 的配网热点），串口日志里会打印网页地址
2. 浏览器打开 `http://<ESP32-IP>/` → 点顶部「歌曲上传」；也可直接访问 `http://<ESP32-IP>/upload`
3. 选一个 `mp3` / `wav` 文件，点上传，页面显示进度；传完会提示结果
4. STM32 把文件先写成 `<名字>.part`，收完最后一帧再改名成正式文件；中途断了（20 s 无数据）会自动删掉半截文件
5. 上传完成后歌单不会立刻刷新（避免打断播放）：停止播放后按 K3 切歌即会重扫

> 上传过程中不要关闭页面；上传与其它写卡操作互斥（同一时刻只有一条会话）。

### 5.4 WiFi 配置

1. 浏览器打开 `http://<ESP32-IP>/`，填 SSID / 密码后保存（最多 8 组，存在 ESP32 NVS，掉电不丢）
2. 开机按保存顺序依次尝试连接，每次 12 s 超时
3. **全部连不上**：自动开热点 `ESP32-MP3-Setup`（密码 `mp3setup123`）+ 强制门户，手机连上热点后浏览器会自动跳到配置页（`http://192.168.4.1/`）
4. 局域网里找不到 ESP32 的 IP 时，看串口日志（115200）里打印的地址

## 6. 两板通信协议

### 6.1 UART 链路（音频流 + 控制）

- 物理层：USART2 @1 Mbps，8N1；STM32 侧收中断 + 双向环形缓冲
- 帧格式：`[0xAA][0x55][type][len u16 LE][payload][crc16 u16 LE]`，CRC-16/CCITT-FALSE（poly 0x1021，init 0xFFFF，不反转）
- 流控：**信用窗口**。STM32 用 `CMD_AUDIO_REQ` 向 ESP32 要数据，ESP32 发多少就扣多少额度，额度耗尽即停；ESP32 每 10 ms 补一次请求，另有 600 ms 数据看门狗自愈
- 寻址：STM32 是唯一主动方（歌单在它这里），ESP32 只应答与上报（`RESP_*`：状态、播放进度时间、播放结束、PONG）
- 歌词/封面/歌单全部在 STM32 本地处理，不经链路

### 6.2 SPI 链路（写卡：手机网页上传）

ESP32-S3 是主机（8 MHz），STM32F407 是从机（SPI2 + DMA）。**固定长度 2064 B 帧**，一次传输一帧、从机回一帧应答：

```
MOSI（主机 -> 从机，2064 B）
  0     0xA5          帧头 1
  1     0x5A          帧头 2
  2     type          1=BEGIN 2=DATA 3=END 4=ABORT 5=PING
  3     flags         保留
  4..5  seq   u16 LE  数据序号（从 0 递增）
  6..7  len   u16 LE  本帧有效字节数（<= 2048）
  8..11 total u32 LE  文件总长度（未知时填 0）
  12..15 crc32        载荷 CRC32（poly 0xEDB88320）
  16..2063           载荷（最后一帧不足则补 0）

MISO（从机 -> 主机，16 B 头 + 64 B 消息）
  0     0x5A
  1     0xA5
  2     status        0=OK 1=BUSY 2=ERROR
  3     errcode       1=BADFRAME 2=NOFILE 3=OPEN 4=WRITE 5=SEQ 6=BADNAME 7=RENAME 8=NOCARD
  4..5  ack_type      本应答对应的请求类型
  6..7  ack_seq       已确认的序号
  8..11 written u32   已写字节数
  12..15 total u32    文件总长度
  16..79 msg[64]      从机提示文本
```

**握手与防丢包**：

1. 从机准备好才拉高 `READY`（PB11）；主机先等 READY 再发帧，超时 3 s 报链路断开
2. 一帧一应答，**应答滞后一帧**：主机发第 N 帧时读到的其实是第 N-1 帧的应答；因此数据发完后主机必须补一帧 `PING` 把最后一帧的应答取回来（BEGIN 也用它读回真实结果）
3. DATA 帧序号必须严格连续（`seq != 期望值` → 从机回 `SEQ` 错误，主机放弃本次传输）
4. 载荷和帧头都带 CRC；CS 在一帧未收满时提前拉高按错帧处理并重新对齐
5. 文件名只允许长度 1~95、不含路径分隔符/通配符，且必须以 `.mp3` / `.wav` 结尾

详细说明与排障：[`docs/spi-upload-link.md`](docs/spi-upload-link.md)。

## 7. Flash 与内存布局

STM32F407VET6 片内 512 KB Flash 按 16/64/128 KB 分扇区：

| 区域 | 地址 | 用途 |
| --- | --- | --- |
| Sector 0-6 | `0x0800_0000` ~ `0x0805_FFFF` | 固件（本工程实测 Total ROM 224556 B，`LR_IROM1` 结束于 `0x0803_6F28`，余量约 164 KB） |
| Sector 7 | `0x0806_0000` ~ `0x0807_FFFF` | **断点续播记录区**（128 KB，追加式环形，96 B/条） |

- 记录条目：`magic` + `seq` + `len` + `kind` + `pos_ms` + 载荷（曲目号/文件大小/文件名）+ `crc16`
- 写入策略：优先找空白条目写入；没有空位时先把最新记录复制到安全位置，再整扇区擦除重排。写入前预先擦好备用条目（prime），把每次写入的阻塞从 ~1 s 降到毫秒级
- 擦写在主循环里同步完成（几百 ms），期间靠串口环形缓冲 + 上层重发自愈，不需要关中断
- FatFs 的 `FF_CODE_PAGE` 设为 **437**（而不是 936）以省下约 170 KB ROM：文件名走 LFN（UTF-16 ↔ UTF-8）与码页无关，中文文件名照常读写；GBK 歌词所需的转码表由本项目自带的 `stm32/app/gbk2312_map.h`（16 KB）提供

ESP32-S3 N16R8 分区（`platformio.ini` 默认 16 MB 表）：NVS 20 KB、应用 2×3264 KB、SPIFFS 1536 KB、coredump 64 KB。

## 8. 已知限制

- **上电默认暂停**：恢复曲目与位置后停在暂停态，按 K1 继续（与原始需求一致，方便确认位置）
- **断点精度 ~1 s**：没有掉电检测电路，靠每秒检查点近似；掉电最坏回退一个检查点
- **网页上传的文件名**：浏览器发来什么就按 UTF-8 原样存，FatFs 长名（LFN）能正确保存，TFT 歌单里的中文名也正常显示；名字里的空格会换成 `_`，路径分隔符会被去掉
- **`.lrc` 歌词编码**：UTF-8 与 GBK 都能识别；GBK 转码表覆盖 GB2312（7445 字），GBK 扩展区（罕见字）会显示成 `?`
- **不需要 PC 端软件**：所有操作都在板子上（`/upload` 网页除外，见 [§5.3](#53-手机网页上传歌曲)）
- ESP32 的 `link`/`player` 依赖 PSRAM，换成无 PSRAM 的模组需要改缓冲区大小

## 9. 第三方组件与许可

本仓库整体以 **MIT** 许可发布（见 [`LICENSE`](LICENSE)）。第三方组件的版权与许可归原作者，随源码保留：

| 组件 | 位置 | 许可 |
| --- | --- | --- |
| STM32F4xx CMSIS + 标准外设库 | `stm32/libraries/` | ST 的 BSD-3-Clause，见各目录 `LICENSE.txt` |
| FatFs R0.16 | `stm32/FatFS/` | ChaN 的 FatFs 许可（1-clause BSD 风格），见 <http://elm-chan.org/fsw/ff/> |
| Helix 定点 MP3 解码器 | `esp32/lib/mp3dec/` | RealNetworks RPSL（原件 `LICENSE.txt`，中文摘要见 `LICENSE-HELIX.md`）。**这是互惠许可，商用前请自行阅读条款** |
| ESP32 Arduino Core / ESP-IDF | 由 PlatformIO 自动下载，不入库 | LGPL-2.1 / Apache-2.0 |
| 14 px 中文字库 `ui_font_cn14.c` | `stm32/bsp/ui/` | 点阵由 **SimSun（中易宋体）** 生成的自用子集（ASCII + GB2312 一级）。字体版权归中易/微软，**商用请自行替换字体重新生成** |

## 10. 文档索引

| 文档 | 内容 |
| --- | --- |
| [`docs/spi-upload-link.md`](docs/spi-upload-link.md) | SPI 写卡链路：帧格式、会话/握手、网页上传、排障表 |
| [`docs/wifi-nvs.md`](docs/wifi-nvs.md) | 多 WiFi 管理：NVS 键值、网页路由、AP 兜底与强制门户 |
| [`docs/delivery-notes.md`](docs/delivery-notes.md) | 交付记录：各阶段功能、验证方式、构建体积 |
| [`docs/roadmap.md`](docs/roadmap.md) | 开发规划与需求分解 |
| [`hardware/wiring-diagram.html`](hardware/wiring-diagram.html) | 接线图（浏览器打开） |
| [`hardware/tft-ui-demo.html`](hardware/tft-ui-demo.html) | TFT 界面像素级预览 |

## 11. 致谢

- Helix MP3 解码器（RealNetworks）与 FatFs（ChaN）是两板能跑起来的基础
- 字库取自 SimSun 14 px 点阵，仅用于学习交流
