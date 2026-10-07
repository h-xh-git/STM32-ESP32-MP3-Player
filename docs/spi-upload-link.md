# 网页上传存 TF 卡 · SPI 主从链路说明

> 目标：手机/电脑浏览器把本地 mp3/wav 上传给 ESP32-S3，经 SPI 分包推给 STM32F407，由 STM32 用 FatFS 写入 TF 卡。
> **这是一条与现有板间 UART 完全独立的新链路**：音频播放、按键、歌单、断点续播仍然全部走原来的 USART2（1 Mbps）。

---

## 1. 总体数据流

```
 手机/电脑浏览器里选的本地 mp3/wav
    |  POST /upload（multipart，边收边转）
    v
  ESP32-S3  --SPI2 主机，固定 2064 B 一帧-->  STM32F407（SPI2 从机）
                                              |  解析帧（magic + CRC32）
                                              |  FatFS f_write
                                              v
                                        TF 卡 <歌单目录>/<名字>.part
                                              | 收完 T_END
                                              v
                                        f_rename -> <名字>.mp3（歌单可见）
```

两条链路分工：

| 链路 | 用途 | 引脚 |
|---|---|---|
| USART2（已有） | 音频压缩流下发、播放控制、断点续播位置回报 | PA2/PA3 <-> GPIO17/GPIO18 |
| **SPI2（新增）** | **浏览器上传本地文件 -> 写 TF 卡**（浏览器 -> ESP32 -> SPI -> STM32 -> TF） | PB11-PB15 <-> GPIO9-GPIO13 |

---

## 2. 接线（新增 5 根线 + 共地）

| 信号 | STM32F407（天空星） | ESP32-S3 | 方向 |
|---|---|---|---|
| SPI_SCK | **PB13**（SPI2_SCK，AF5） | **GPIO12** | ESP32 -> STM32 |
| SPI_MOSI | **PB15**（SPI2_MOSI，AF5） | **GPIO11** | ESP32 -> STM32 |
| SPI_MISO | **PB14**（SPI2_MISO，AF5） | **GPIO13** | STM32 -> ESP32 |
| SPI_CS | **PB12**（普通 GPIO 输入上拉） | **GPIO10** | ESP32 -> STM32 |
| DL_READY | **PB11**（推挽输出） | **GPIO9**（输入上拉） | STM32 -> ESP32 |
| GND | GND | GND | 必须共地 |

**选脚理由**：LCD 已占 SPI1（PA5/PA7/PA4/PB0/PB1/PB10），SPI3 默认脚 PC10-PC12 被 SDIO 独占，
SPI3 重映射用的 PB3/PB4 是 SWO/NJTRST；PB11-PB15 目前全部空闲且同端口便于走线。
ESP32 侧 GPIO9-13 落在可用区间（禁区为 GPIO26-37 / 19 / 20 / 43 / 44 / 0 / 3 / 45 / 46）。

> **注意**：CS 没有接到 STM32 的 SPI2_NSS 上。STM32 侧用**软件 NSS（SSM=1 + SSI=1）**永久选中，
> CS 只当普通输入 + EXTI 用来界定事务边界 —— 这样可以避开 F4 从机硬件 NSS 的经典首字节坑。

---

## 3. 协议

### 3.1 帧格式（每次事务**固定 2064 字节** = 16 B 头 + 2048 B 载荷）

固定长度是刻意的：STM32 从机用 DMA 的 NDTR 直接界定一帧，不需要事先知道长度，
也就不用处理长度字段本身出错的边界情况。

**MOSI（ESP32 -> STM32）**

| 偏移 | 类型 | 含义 |
|---|---|---|
| 0 | u8 | 同步字 0xA5 |
| 1 | u8 | 同步字 0x5A |
| 2 | u8 | 类型（见 3.2） |
| 3 | u8 | 保留（0） |
| 4..5 | u16 LE | 块序号 seq（T_DATA 从 0 起） |
| 6..7 | u16 LE | 载荷有效字节数（<= 2048） |
| 8..11 | u32 LE | 文件总字节数（T_BEGIN 有效） |
| 12..15 | u32 LE | 载荷 CRC32（0 = 不校验） |
| 16..2063 | - | 载荷 |

**MISO（STM32 -> ESP32）**

| 偏移 | 类型 | 含义 |
|---|---|---|
| 0 | u8 | 同步字 0x5A |
| 1 | u8 | 同步字 0xA5 |
| 2 | u8 | 状态：0=OK / 1=BUSY / 2=ERROR |
| 3 | u8 | 错误码 |
| 4..5 | u16 LE | 已处理完的上一帧类型 |
| 6..7 | u16 LE | 已成功写入的最后一块 seq |
| 8..11 | u32 LE | 已写入字节数 |
| 12..15 | u32 LE | 文件总字节数 |
| 16..79 | ASCII | 状态文字（64 B） |

### 3.2 帧类型

| 值 | 名称 | 含义 |
|---|---|---|
| 1 | T_BEGIN | 开始：载荷 = 文件名（UTF-8），total = 文件大小 |
| 2 | T_DATA | 数据块：seq = 块号，载荷 = 数据 |
| 3 | T_END | 结束：STM32 收尾落盘并改名 |
| 4 | T_ABORT | 中止：STM32 关文件并删掉半成品 |
| 5 | T_PING | 只取状态（用于收尾确认） |

### 3.3 错误码

| 值 | 名称 | 含义 |
|---|---|---|
| 0 | NONE | 无错 |
| 1 | BADFRAME | 同步字 / CRC32 / 长度校验不过 |
| 2 | NOFILE | 还没 BEGIN 就收到 DATA |
| 3 | OPEN | 建文件失败（写保护 / 目录不存在） |
| 4 | WRITE | f_write / f_sync / f_close 失败 |
| 5 | SEQ | 块序号不连续 |
| 6 | BADNAME | 文件名非法（空 / 含路径 / 后缀不是 .mp3 或 .wav） |
| 7 | RENAME | 收尾改名失败 |
| 8 | NOCARD | 卡不在或文件系统没挂载 |

### 3.4 流控与应答（**关键**）

- **一帧一应答**：STM32 的 RX DMA 收满一帧后只做三件事 —— 停 RX 流、**拉低 READY**、
  置「有帧待处理」；解析 + 写卡 + 组织应答放在主循环，处理完重新 Arm 两条 DMA 再**拉高 READY**。
- **ESP32 每次事务前必须等 READY 变高**（超时 3 s 判链路故障）。
- **应答滞后一帧**：ESP32 发第 N 帧时，MISO 上读到的是第 N-1 帧的处理结果。
  所以最后一帧（T_END）之后要再发一帧 T_PING 才能拿到 END 的确认。
- ESP32 中途放弃或 STM32 重启时，CS 上升沿会让 STM32 重新对齐 DMA，不会长期错帧；
  帧内 magic + CRC32 双重校验，坏帧直接丢弃并计一次错误。

---

## 4. 代码结构

### STM32 侧（新增 2 个模块，已注册进 Keil 工程）

| 文件 | 职责 |
|---|---|
| `stm32/bsp/spid/bsp_spid.{h,c}` | SPI2 从机 + DMA1 Stream3/4 + READY 流控 + CS EXTI 对齐。**只管搬一帧的字节**，不含协议语义 |
| `stm32/app/netdl.{h,c}` | 解析帧、维护 FatFS 文件、组织应答、进度统计 |

- 关键宏在 `bsp_spid.h`：`SPID_FRAME 2064u`、`SPID_PAYLOAD_MAX 2048u`、`SPID_T_*`、`SPID_ST_*`、`SPID_E_*`。
- DMA 缓冲放在**主板 SRAM**（CCM 0x10000000 不能给 DMA 用）。
- 写卡策略：先写 `<歌单目录>/<名字>.part`，收完 T_END 才 `f_unlink` 同名旧文件 + `f_rename` 成最终名；
  T_ABORT 或 **20 s 无帧**则删掉半成品。这样歌单永远不会扫到写了一半的文件。
- 目标目录取自 `pl_base()`（`/MUSIC` 存在就用它，否则根目录），保证上传的歌**和歌单同一目录**。
- main.c 接入点：`spid_init(); netdl_init();`（uart2 初始化之后）、`netdl_set_fs_ready(1/0)`（f_mount 结果）、
  主循环 `netdl_poll();`、上传完成置 `dl_rescan` 标志，**等不播放时**再 `pl_scan()` 刷新歌单。

### ESP32 侧（新增 lib/netdl）

| 文件 | 职责 |
|---|---|
| `esp32/lib/netdl/src/netdl.{h,cpp}` | 网页上传接收 + SPI2 主机分帧推送 + 上传 API（WiFi 交 wifimgr 管理） |
| `esp32/lib/netdl/library.json` | PlatformIO 库描述 |

- SPI 用 ESP-IDF `spi_master`（SPI2_HOST = FSPI），8 MHz、Mode 0，DMA 通道自动分配。
- 上传走 wifimgr 借出的 `WebServer`（`HTTPUpload` 回调），边收边推，每 64 帧打印一次进度。
- 上传在 WebServer 回调里同步处理（已无 FreeRTOS 下载任务）；SPI 帧是同步推的，所以上传期间 ESP32 主循环会忙。
- **每次上传占一套 SPI 会话**（`ses_take / ses_begin / ses_data / ses_end`，静态变量 `s_owner`）：
  同一时刻只允许一个传输占用链路，第二个并发请求会被拒（网页提示「当前有传输在跑」）。
- 网页上传页挂在 wifimgr 的 WebServer 上（同一个 80 端口），见 wifimgr.h 的 `wifimgr_web()`：
  `GET /upload` 上传表单、`POST /upload` 接收 multipart（`HTTPUpload` 回调里按 2048 B 切块喂 SPI）、
  `GET /upstatus` 返回 JSON 进度。主循环 `wifimgr_poll()` 里 `handleClient()` 处理这些请求 ——
  所以**上传期间别指望按键响应多灵敏**（SPI 传输本身是阻塞的，与播放任务是分开的）。
- 网页上传时主机未必知道总大小（浏览器给不出可靠长度时才不发 `size`），STM32 侧
  `dl_end()` 会把 `s_total` 改成**实际落盘字节数**，所以 TFT 上的上传百分比最终一定是准的。

---

## 5. 操作步骤（从零到「新歌出现在歌单里」）

### 0) 前提

- STM32F407（天空星）已插好 TF 卡并格式化为 FAT32；
- 两块板各自供电（ESP32 用板载 CH340 的 USB-C 口，STM32 用它自己的 USB/ST-Link），**必须共地**；
- 两个串口都按 115200：STM32 看 PA9/PA10（USART1），ESP32 看 CH340 那个 USB-C 口。

### 1) 接线：新增 5 根线 + 1 根地

见第 2 节的表：`PB13-GPIO12`(SCK)、`PB15-GPIO11`(MOSI)、`PB14-GPIO13`(MISO)、`PB12-GPIO10`(CS)、`PB11-GPIO9`(READY)、`GND-GND`。
杜邦线尽量短；先别接 5V/VIN，两块板各自供电更安全。

### 2) 烧录 STM32

Keil 打开 `stm32/project/MDK(V5)/Project.uvprojx` -> **Rebuild** -> Download -> 复位。
STM32 串口应出现：

```
[SPID] SPI2 slave on PB13/PB14/PB15, CS=PB12 RDY=PB11, frame=2064 B
[NETDL] ready (SPI slave -> FATFS)
```

（`[NETDL] ready` 说明 SPI 从机起来了；此时 PB11 READY 已经是高，ESP32 就可以发帧。）

### 3) 烧录 ESP32

```powershell
cd esp32
pio run -e esp32s3 -t upload
pio device monitor -b 115200     # 或 VSCode/PlatformIO 的串口监视器
```

ESP32 串口应出现 `[NETDL] SPI master: SCK=12 MOSI=11 MISO=13 CS=10 READY=9 @ 8000000 Hz`，
以及 `[WIFI] --- wifimgr 启动：先从 NVS 读回账号 ---`。

### 4) 配网（只需一次，之后存在 NVS 里，断电不丢）

**首次上电 / 账号全连不上时**：ESP32 会自动开热点 `ESP32-MP3-Setup` / 密码 `mp3setup123`。
手机连这个热点 -> 多数手机会自动弹出配置页（没弹就浏览器开 `http://192.168.4.1/`）
-> 填 SSID + 密码 -> 保存并连接。

> 开源版**删掉了交互式串口命令**（原来的 `wifi` / `wifilist` / `status` / `help` 等）：
> 配网与账号管理全部在配置网页上做（`/` 页里可添加、切换、删除），串口只打印启动横幅、连网结果与错误。

连上后串口会打印：`[WIFI] 已连接 [0] 你的SSID  ip=192.168.x.x  rssi=-5x dBm`。

配置网页**一直开着**：连上路由器之后，同一局域网里的手机/电脑也能随时打开它改 WiFi ——
`http://<ESP32 的 IP>/` 或 `http://mp3player.local/`（mDNS）。不配网时它就是下载用的同一块 ESP32。

### 5) 上传歌曲（手机/电脑浏览器选文件，经 SPI 写进 TF 卡）

前提：ESP32 已联网（STA），手机与 ESP32 在同一个局域网；或者 ESP32 正开着热点
`ESP32-MP3-Setup`（密码 `mp3setup123`）时手机直接连热点（会弹配置页）。

1. 浏览器打开 `http://<ESP32 的 IP>/`（或 `http://mp3player.local/`），在「WiFi 配置」页点
   **歌曲上传** 链接，也可以直接开 `http://<ESP32 的 IP>/upload`；
2. 点「选择文件」挑一个 **.mp3 / .wav**；「存到卡上的名字」留空 = 用文件原名；
3. 点「开始上传」，页面显示进度条；进度条反映的是**浏览器已经发出去的字节**，不是 STM32 写卡进度；
4. 传完自动跳结果页：`写卡成功：song.mp3 — 5242880 B`；失败会显示 STM32 返回的原因
   （如 `no card`、`bad filename`）。

注意事项：

- **上传期间不要关页面**：关掉页面 = 连接断 = ESP32 收不到剩余数据 -> 发 `T_ABORT` -> STM32 删掉 `.part`；
- 一次只能传一个文件（ESP32 侧 `s_owner` 会话锁），第二个并发请求会被拒：网页提示「当前有传输在跑」；
- 卡上文件名就是你填的名字，按 UTF-8 原样写出；**中文名在 TFT 歌单里能正常显示**
  （FatFS `FF_LFN_UNICODE=2` 的长名走 UTF-16↔UTF-8，与码页无关）。名字里的空格会换成 `_`，路径分隔符会被去掉；
- 上传速度约 700-900 KB/s（取决于 TF 卡写入速度），一首 5 MB 的歌约 6-10 s；
- 上传期间 ESP32 主循环会忙（SPI 帧同步推），网页响应变慢属正常现象。

### 6) 两边同时能看到什么

| 位置 | 现象 |
|---|---|
| ESP32 串口 | 开传：`[NETDL] upload from browser: song.mp3 (5242880 B declared)` + `[NETDL] BEGIN ok: song.mp3 total=5242880 B`；收尾：`[NETDL] upload song.mp3: OK (5242880 B)`（失败则是 `FAIL (...)` + STM32 给的原因） |
| STM32 串口 | 开传：`[NETDL] begin /MUSIC/song.mp3.part total=5242880 B`；收尾：`[NETDL] done /MUSIC/song.mp3  5242880 B`。**没有逐段进度打印**，进度看网页或 TFT |
| STM32 TFT（暂停态） | 标题显示歌名、信息行显示 `DL 12% 480KB`（由 `netdl_percent()` 驱动） |
| 网页 | 进度条 = 浏览器已经发出去的字节；结果页显示 `写卡成功：…` 或 STM32 返回的错误 |

### 7) 收尾与验证

1. STM32 收到 T_END -> `f_sync/f_close` -> 删同名旧文件 -> `f_rename` 成正式名：
   串口打印 `[NETDL] done /MUSIC/song.mp3  5242880 B`；
2. 上传完成标志置位，**等当前不播放**时自动重扫歌单：`[NETDL] playlist rescanned: N song(s)`；
3. 按 K3 下一曲即可听到新歌（它已经和原来的歌单在同一目录 `/MUSIC`，没有 `/MUSIC` 时就是根目录）。
4. 想确认文件本身：把 TF 卡插到电脑上，看 `/MUSIC/song.mp3` 大小是否与源一致；
   写了一半的文件是 `song.mp3.part`，正常流程不会留在卡上。

### 8) 常用操作问答

| 想干什么 | 怎么做 |
|---|---|
| 上传时还能听歌吗 | 能。SPI 上传与 UART 播放是两条独立链路；只是 UI 刷新与网页响应会变慢，声音不断 |
| 中途不想下了 | 直接断电/复位即可；STM32 在 20 s 收不到帧会自动删掉 `.part` 半成品 |
| 手机/电脑怎么传歌进去 | 同一局域网里开 `http://<ESP32 IP>/upload`，选文件点上传（见第 6 节） |
| 能同时传两个文件吗 | 不能，共用一条 SPI 会话；等一个结束再传下一个 |
| 上传失败了想重来 | 重新选文件再传一次即可（没有断点续传，会从头写，半成品会被删掉） |
| 换 WiFi | 在配置网页里再加一组账号（新账号写进 NVS，旧账号留着，可在网页上删） |
| 换一根网线/路由器 | 不用改代码，开机自动按 NVS 里存的账号依次尝试 |

---

## 6. 性能与限制

| 项 | 数值 / 说明 |
|---|---|
| SPI 时钟 | 8 MHz（可调到 16 MHz，但杜邦线质量与 STM32 写卡速度会先成为瓶颈） |
| 单帧 | 2064 B，传输约 2 ms（**DMA 搬运，不占 CPU**） |
| 实测吞吐预期 | 约 **700-900 KB/s**（受 TF 卡写入速度限制），一首 5 MB 的歌约 6-10 s |
| STM32 CPU 占用 | 约 10-20%（主要是 f_write）；音频流只需约 16 KB/s，可边下边播 |
| 断点续传 | 无。中断后重下/重传会从头开始（半成品会被删掉） |
| 文件名 | 只接受 `.mp3` / `.wav`，且不能含路径分隔符与通配符 |
| 上传进度条 | 只反映「浏览器已发出的字节」，不是 STM32 写卡进度；两边串口日志才是真实进度 |
| 传输互斥 | 任意时刻只允许一路传输，由 ESP32 侧 `s_owner` 会话锁保证 |

**已知限制**：

- **不支持 HTTPS**：配置页与上传页都是明文 HTTP（内网使用没问题）。
- **没有重传**：一帧 CRC 不过就整帧丢弃并计错，ESP32 看到 `status=ERROR` 会中止整次上传（重新选文件再传即可）。
- 上传中如果正在播放，STM32 主循环会变忙，UI 刷新可能变慢（声音不会断，有 ESP32 侧缓冲兜着）。
- 上传完成后如果正在播放，歌单**不会立刻**刷新（避免打断播放），暂停后自动重扫。
- 网页上传要求手机与 ESP32 在同一网段（或手机连 ESP32 的热点），且**页面必须保持打开**到传完。
- 网页上传只做"分块转发"，不做浏览器端断点续传：中途断了要重新选文件重传。

---

## 7. 排障

| 现象 | 检查 |
|---|---|
| **一上传就报 `bad reply sync xx xx`** | MISO 上没有 STM32 的应答。MISO 上一点数据都没有，按顺序查：① **STM32 有没有烧进含 `bsp/spid` 的新固件** —— STM32 串口应能看到 `[SPID] SPI2 slave on PB13/PB14/PB15, CS=PB12 RDY=PB11, frame=2064 B` 与 `[NETDL] ready (SPI slave -> FATFS)`，没有这两行就是固件不对；② MISO 是不是 STM32**PB14** -> ESP32**GPIO13**（别接到 PB13/PB15）；③ 两板是否共地。 |
| `[NETDL] warn: BEGIN ack_type=...` | 读到的应答不是 T_BEGIN 的（链路错位或 STM32 中途复位）。偶发一次不用管；反复出现就查接线。 |
| ESP32 报 `STM32 READY timeout` | READY(GPIO9) 一直是低：5 根线接对没 / 共地 / STM32 是否跑到 `spid_init`（看它的串口横幅）。 |
| STM32 报 `bad sync` 或 `payload crc mismatch` | SCK/MOSI 线太长或没共地；把 `esp32/lib/netdl/src/netdl.h` 的 `NETDL_SPI_HZ` 从 8 MHz 降到 4 MHz 重编再试。 |
| STM32 报 `bad filename` | 文件名后缀不是 .mp3/.wav，或名字里带了路径与非法字符。 |
| STM32 报 `no card / fs not mounted` | TF 卡没插好，或 f_mount 失败（看 STM32 启动日志）。 |
| 报 `STM32 err 5 (seq)` | 上一次事务被中断过；重新上传即可（STM32 会在 CS 上升沿重新对齐）。 |
| 打开 `/upload` 提示 disabled | ESP32 启动时 WebServer 还没起来，看串口是否打印 `[NETDL] !! wifimgr web server not ready`；确认 main.cpp 里 `wifimgr_init()` 在 `netdl_init()` 之前。 |
| 上传到一半断了 | 页面必须保持打开到传完；断了 STM32 会在 20 s 后删 `.part`，重新传即可。 |
| WiFi 连不上 / 网页打不开 | 看 ESP32 启动日志里的连网结果（连上会打印 IP）；连不上时 ESP32 会自动开热点 `ESP32-MP3-Setup`（密码 `mp3setup123`），手机连它再开 `http://192.168.4.1/`。 |

---

## 8. 与断点续播共存的注意事项

断点续播把片内 Flash 的 **Sector 7（0x08060000-0x0807FFFF）** 当成自己的记录区。
加了 SPI 上传链路后固件 ROM 仍在 Sector 7 之前，但要留意余量：

- 最新实测（开源版：网页上传链路 + GBK 歌词码表 `app/gbk2312_map.h`，已移除 HTTP 下载通路与调试代码）：
  FatFS 码页为 437（`FatFS/ffconf.h` 的 `FF_CODE_PAGE`，省掉 174 KB GBK 转换表），**Total ROM 224556 B（219.29 kB）**
  （`Code 49648 / RO-data 174792 / RW-data 624 / ZI-data 127536`），Load Region `LR_IROM1` 结束于
  **0x08036F28**（Size 0x00036F28），距 0x08060000 **余约 164 KB**。
  （参考：删调试代码前是 215804 B / 0x08034D2C；换码页之前是 389984 B / 0x0805F58C / 只剩 2.6 KB；
  RO-data 里约 16 KB 是 GB2312 码表，删掉的调试代码省回约 7 KB。）
- 后续再往固件里加东西（新 UI、TLS、更多解码器）时，**必须确认链接结果不超过 0x08060000**，
  否则会把断点续播的记录区当成代码区烧进去。

---

## 9. 本次联调改动记录（相对上一版）

- **ESP32 侧：删掉 HTTP 下载通路**（`dl` 命令、`dl_task()`、`netdl_start()`、`derive_name()`、`HTTPClient` 依赖全部移除），
  只保留浏览器上传。
- **ESP32 侧：修 `ses_begin()` 的应答错位** —— 以前把 T_BEGIN 那一帧的应答当 BEGIN 结果，而它其实是上一帧/上一会话的残留；
  现在先丢掉 T_BEGIN 的应答（`dl_frame(..., NULL)`），再发一帧 T_PING 把 BEGIN 的真实结果读回来（与 `ses_end()` 的收尾手法对称，
  并顺带检查 `ack_type` 是否为 `NT_BEGIN`）。
- **ESP32 侧：READY(GPIO9) 由内部上拉改为下拉** —— STM32 不在/没跑起来时读到低电平，直接报 `STM32 READY timeout`，
  而不是误判「可以发帧」后收到一屏 `bad reply sync`。
- **STM32 侧：手动上一曲/下一曲到顶/到底不再回绕**（`app/main.c` 的 KEY_2/KEY_3 与远程 `CMD_NEXT`/`CMD_PREV` 改成夹紧：
  最后一首再按下一曲不动，第一首再按上一曲不动）。一首播完的自动切歌**仍是列表循环** `(fin+1)%total`；要改回回绕就是那 4 行改回 `% st.total`。
- **STM32 侧 ROM**：改动过的 TU 全部 armcc 0 error 0 warning，armlink 全量重链接 `Total ROM 224556 B（219.29 kB）`、
  `LR_IROM1` 结束 `0x08036F28`（含 16 KB GB2312 码表；删掉的调试代码约省 7 KB）。