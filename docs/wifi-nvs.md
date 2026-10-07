# ESP32-S3 多 WiFi 管理：NVS 持久化 + 网页配置表单

> 需求（4 条）与落地位置
>
> | 需求 | 实现 |
> |---|---|
> | ① 两组数组存多组 WiFi 账号（SSID 数组、密码数组） | esp32/lib/wifimgr/src/wifimgr.cpp 的 s_ssid[8][33] / s_pass[8][65] |
> | ② 网页做表单，提交新 WiFi 名称 + 密码 | 同文件 h_root() 生成表单，h_save() 接收 POST /save |
> | ③ 新 WiFi 存进 ESP32 **NVS**（断电不丢） | nvs_save_table() 写 NVS 命名空间 wifi（Preferences） |
> | ④ 开机先读 NVS 里的 ssid/password，STA 连接 | wifimgr_init() -> nvs_load() -> sta_begin() 按顺序尝试 |
>
> 账号**不是**普通全局数组：开机第一步就是从 NVS 读回来；每次网页/串口改动立刻落盘。
> 断电重启后仍然是上次保存的那几组，不会变回代码里写死的值。

---

## 1. 新增 / 改动的文件

| 文件 | 说明 |
|---|---|
| esp32/lib/wifimgr/src/wifimgr.h | 对外接口 + 容量/超时/热点宏（新增） |
| esp32/lib/wifimgr/src/wifimgr.cpp | 账号表 + NVS 读写 + 连接状态机 + 网页（新增） |
| esp32/lib/wifimgr/library.json | PlatformIO 库描述（新增） |
| esp32/lib/netdl/src/netdl.cpp | WiFi 连接改为调用 wifimgr；网页上传（/upload）（改动） |
| esp32/lib/netdl/src/netdl.h | 文档补充（改动） |
| esp32/src/main.cpp | setup 里 wifimgr_init()，loop 里 wifimgr_poll()（改动） |

platformio.ini 不需要改：lib/ 下的库 PlatformIO 会自动发现。

---

## 2. 账号表：两组平行数组

~~~cpp
#define WIFIMGR_MAX       8u     // 最多 8 组账号
#define WIFIMGR_SSID_LEN  33u    // 32 字符 + 结尾 0
#define WIFIMGR_PASS_LEN  65u    // 64 字符 + 结尾 0

static char    s_ssid[WIFIMGR_MAX][WIFIMGR_SSID_LEN];  // 第 0 维 = 第几组
static char    s_pass[WIFIMGR_MAX][WIFIMGR_PASS_LEN];  // 与 s_ssid 一一对应
static uint8_t s_count;                                // 有效组数
static int8_t  s_active;                               // 上次连上的下标，开机优先试
~~~

只读访问：wifimgr_count() / wifimgr_ssid(i) / wifimgr_pass(i) / wifimgr_active()。

---

## 3. NVS 键布局（命名空间 wifi）

| 键 | 类型 | 含义 |
|---|---|---|
| s0..s7 | string | 第 i 组 SSID |
| p0..p7 | string | 第 i 组密码 |
| act | i8 | 上次成功连上的下标（开机优先试它） |
| n | u8 | 有效组数（**最后写**） |
| aps / app | string | 配网热点名 / 密码（可选覆盖） |

**掉电安全设计**

- 每条 nvs_set_* 之后立刻 nvs_commit，没有整表擦写窗口；
- 计数键 n 最后写：中途断电最多让新加的那组不可见，旧状态始终完整可读；
- 开机 nvs_flash_init() 失败（或版本不符/无空闲页）时自动 nvs_flash_erase() 重来，不会卡死。

分区表实测（解 .pio/build/esp32s3/partitions.bin 得到）：

~~~
nvs      data nvs      off=0x009000 size=0x005000 ( 20 KB)   <- 账号存这里
otadata  data ota      off=0x00e000 size=0x002000 (  8 KB)
app0     app  ota_0    off=0x010000 size=0x330000 (3264 KB)
app1     app  ota_1    off=0x340000 size=0x330000 (3264 KB)
spiffs   data spiffs   off=0x670000 size=0x180000 (1536 KB)
coredump data coredump off=0x7f0000 size=0x010000 (  64 KB)
~~~

20 KB 存 8 组账号（每组最多约 100 B）绰绰有余。

---

## 4. 开机流程

~~~
wifimgr_init()
  |- nvs_flash_init()（失败自动擦除重建）
  |- Preferences.begin("wifi") -> nvs_load()        <- 先读 NVS
  |    |- 读到 n 组 -> s_count=n, s_active=act
  |    \- 一个都没有且编译期有兜底账号 -> 写入 NVS 当第 0 组
  |- WiFi.mode(STA) / setHostname("mp3player") / setSleep(false)
  |- 起网页（端口 80）、准备 mDNS
  \- s_count==0 ? 直接开配网热点 : sta_begin(优先试 active 那组)
~~~

连接由 wifimgr_poll() 里的**非阻塞状态机**推进（不会卡住 I2S 播放）：

~~~
每 12 s 换下一个候选账号（顺序 = active 优先，然后 0,1,2...）
  全部超时 -> 开配网热点 + 等 15 s 再轮一遍
  连上     -> 记 active 到 NVS、打印 IP/RSSI、起 mDNS、20 s 后关掉热点
  中途掉线 -> 2 s 后回到轮询
~~~

网页上传全程 wifimgr_set_busy(true)（upload_begin/end/abort 成对调用），期间状态机不切 AP/STA
模式，避免和上传抢射频。

### 配网模式（AP 自动兜底）—— 一条账号都连不上时

| 情况 | 行为 |
|---|---|
| NVS 里一个账号都没有 | 上电立刻开热点 ESP32-MP3-Setup / mp3setup123 |
| 有账号但全部连不上 | 依次试完（密码错/找不到网络会立刻跳过，不等满 12 s）-> **自动开热点** |
| 热点开着期间 | 每 15 s 再轮一遍账号；**重试时用 WIFI_AP_STA，热点不会被关掉**（否则手机上的配置页会闪断） |
| 连上路由器后 | 热点再保留 20 s（方便回头改配置）然后自动关闭，回到纯 STA |

手机上连上热点后，浏览器输 **192.168.4.1** 就能打开配置页；另外模块跑了一个
DNS 劫持（DNSServer 监听 53，所有域名都指到 192.168.4.1）并把未知请求 302 到配置页，
所以**多数手机/电脑连上热点会自动弹出配置页**，不用手输地址。

---

## 5. 网页（配置表单）

| 路由 | 方法 | 作用 |
|---|---|---|
| / | GET | 配置主页：当前状态 + 添加表单 + 已保存账号表（连接/删除） |
| /save | POST | 表单提交 ssid + pass -> wifimgr_add() -> 写 NVS -> 立刻重连 |
| /conn | POST | idx：切到第 idx 组并重连 |
| /del | POST | idx：删除第 idx 组（同步更新 NVS） |
| /scan | GET | 返回附近 AP 的 JSON（页面里「扫描附近网络」按钮用） |
| /status | GET | 纯文本状态，页面每 5 s 自动刷新 |

> 想给 ESP32 加自己的网页接口，用 `wifimgr_web()` 拿到这个 WebServer 指针再 `on()` 挂路由即可
> （lib/netdl 就是这么挂上 `/upload` 歌曲上传页的，见 [`docs/spi-upload-link.md`](spi-upload-link.md) 第 5 节）。
> 注意 `F("...")` 在 ESP32 上展开成 `__FlashStringHelper*`，不能传给 `const char*` 形参。

**怎么打开网页**

| 场景 | 地址 | 说明 |
|---|---|---|
| 已连上路由器 | http://<串口打印的 IP>/ | 手机/电脑要在同一个路由器下 |
| 已连上路由器（mDNS） | http://mp3player.local/ | Windows 需装 Bonjour；iOS/部分安卓原生支持 |
| 没连上（配网模式） | 手机连热点 ESP32-MP3-Setup（密码 mp3setup123）后打开 http://192.168.4.1/ | 首次上电、或账号全部连不上时自动进入；多数手机会自动弹页 |

页面里填 SSID + 密码 -> 保存 -> 自动写 NVS 并重连，2.5 s 后自动跳回主页。

---

## 6. 串口日志（115200，板载 CH340 那个 USB-C 口）

> **开源版删掉了交互式串口命令**（原来的 `wifi` / `wifilist` / `wifidel` / `wifiap` / `wififorget` /
> `status` / `help`）：配网、切换、删除账号全部在配置网页上做（见第 5 节），串口只负责启动横幅、
> 连网结果与错误日志。

上电时能看到（示例：NVS 里有 2 组账号，第 0 组连上）：

~~~
[WIFI] --- wifimgr 启动：先从 NVS 读回账号 ---
[WIFI] 读到 2 组账号，active=0
[WIFI] 正在连 [0] 我家WiFi ...
[WIFI] 已连接 [0] 我家WiFi  ip=192.168.1.23  rssi=-52 dBm
[WIFI] mDNS 就绪: http://mp3player.local/
[WIFI] 配置网页已监听端口 80
~~~

看不到「已连接」就是所有账号都没连上，此时模块会自动开配网热点 `ESP32-MP3-Setup` /
`mp3setup123`，手机连它打开 http://192.168.4.1/ 就能配网。

---

## 7. 编译与烧录

~~~powershell
cd esp32
pio run -e esp32s3            # 编译
pio run -e esp32s3 -t upload  # 烧录
pio device monitor -b 115200  # 看日志
~~~

---

## 8. 怎么验证「断电不丢」

1. 上电 -> 看串口：\[WIFI\] --- wifimgr 启动：先从 NVS 读回账号 ---，首次会打印「没有账号：直接开配网热点」。
2. 手机连热点 -> 打开 http://192.168.4.1/ -> 填 SSID + 密码 -> 保存。
3. 串口应出现：

~~~
[WIFI] 已写入 NVS：1 组账号
[WIFI] 新增/更新账号 [0] 你的SSID
[WIFI] 已连接 [0] 你的SSID  ip=...  rssi=...
[WIFI] mDNS 就绪: http://mp3player.local/
~~~

4. **拔电、等 5 秒、重新上电**。串口应出现：

~~~
[WIFI] --- wifimgr 启动：先从 NVS 读回账号 ---
[WIFI] 读到 1 组账号，active=0
[WIFI]   [0] 你的SSID
[WIFI] 正在连 [0] 你的SSID ...
[WIFI] 已连接 [0] 你的SSID  ip=192.168.1.23 ...
~~~

   不需要再配一次，也不会回到代码里写死的那组 —— 这就是和普通全局数组的区别。
5. 再上电几次都保持；在网页上删掉这组账号后重启，NVS 里就没有了。

---

## 9. 注意事项 / 限制

- **密码是明文存在 NVS 里的**（本工程用的是普通 NVS 分区，没开加密分区）。能打开配置
  网页的人就能改 WiFi —— 家用设备的常规做法；要更严可以给 h_save/h_del/h_conn 加 HTTP Basic Auth。
- 并发：账号表的写入来自配置网页（Arduino 任务），读取来自同一个任务里的网页上传回调。写入是先改数组再落盘，
  最坏情况是读到一组正在改的账号（重试一次即可）。网页上传期间 wifimgr_set_busy(true)
  已禁止状态机切模式。
- /scan 会阻塞 2~4 s（WiFi.scanNetworks() 是同步的），扫描期间页面会稍慢。
- 热点只在「没连上路由器」或「刚连上 20 s 内」开启，省电并避免和 STA 抢信道。
- 想让某组账号开机优先：在网页里点它的「连接」，或删掉后重加；下次开机 active 那组最先被尝试。
- 配置主页顶部有「歌曲上传」入口；上传期间 `wifimgr_set_busy(true)` 会占住方向，
  但 WiFi 在传输中掉线仍会导致传到一半失败（STM32 会删掉 `.part`），重传即可。

---

## 10. 数据结构速查（给后续维护）

~~~
SRC  wifimgr.cpp
  nvs_boot()          nvs_flash_init + 自动擦除重建
  nvs_load()          NVS -> s_ssid/s_pass/s_count/s_active
  nvs_save_table()    s_ssid/s_pass -> NVS（n 最后写）
  try_index(k)        第 k 个候选下标（active 优先）
  sta_begin(i)        WiFi.begin(s_ssid[i], s_pass[i])
  on_connected(i)     记 active 落 NVS + 打印 IP + 起 mDNS
  ap_start/ap_stop    配网热点开关
  h_root/h_save/h_del/h_conn/h_scan/h_status   网页
  wifimgr_init/poll/ensure_connected/set_busy  生命周期
  wifimgr_add/remove/connect_index/forget_all  增删改（都写 NVS）
~~~
