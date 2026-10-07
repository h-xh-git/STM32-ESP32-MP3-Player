#ifndef ESP_NETDL_H
#define ESP_NETDL_H

/*
 * netdl.h - ESP32-S3 侧「网页上传存 TF 卡」模块（SPI 主机）
 *
 * 链路：浏览器 HTTP POST 上传本地 mp3/wav -> SPI2 主机分包推送
 *       -> STM32F407 SPI2 从机（app/netdl.c）-> FatFS f_write -> TF 卡
 *
 * ==================== 接线（与 STM32 侧 bsp/spid/bsp_spid.h 一致） ====================
 *   信号          ESP32-S3      STM32F407(天空星)
 *   SPI_SCK       GPIO12        PB13 (SPI2_SCK  AF5)
 *   SPI_MOSI      GPIO11        PB15 (SPI2_MOSI AF5)
 *   SPI_MISO      GPIO13        PB14 (SPI2_MISO AF5)
 *   SPI_CS        GPIO10        PB12 (GPIO 输入上拉，STM32 侧只当 EXTI 用)
 *   DL_READY      GPIO9 (输入)  PB11 (推挽输出)
 *   GND           GND           GND
 *
 * 说明：ESP32 只做主机推数据；是否落盘、写到哪个目录、文件名叫什么，
 *       全由 STM32 侧决定（它会写到歌单扫描的同一目录，先写 .part 再改名）。
 *
 * ==================== 流控 ====================
 *   每次事务固定 2064 字节（16 B 头 + 2048 B 载荷）。发之前必须等
 *   DL_READY 变高（STM32 已把 DMA 备好）；STM32 处理完一帧会再拉高。
 *   本次读到的状态是上一帧的结果（第一帧是上电初值，不可信）：
 *   T_BEGIN 的真实结果要用紧随的一帧 T_PING 读回来；最后一帧之后
 *   也要再发一帧 T_PING 收尾确认。
 *
 * ==================== 写入通路（SPI 会话） ====================
 *   网页上传：手机/电脑浏览器打开 http://<ip>/upload -> 选本地 mp3/wav ->
 *   HTTP POST（multipart 流式）-> SPI 推帧 -> STM32 写 TF 卡
 *   SPI 会话（T_BEGIN / T_DATA / T_END + READY 握手）同一时刻只允许一个占用：
 *       网页上传回调: netdl_upload_begin() / netdl_upload_write() / netdl_upload_end()
 *   浏览器会把文件大小放进 URL（?size=N），STM32 因此能显示百分比进度；
 *   若没带 size（例如老浏览器走原生表单提交），STM32 收尾时以实际落盘字节数为准。
 *   上传页面挂在 wifimgr 的配置网页（80 端口）上，靠 wifimgr_web() 拿 WebServer 句柄。
 *
 * ==================== WiFi（已搬进 wifimgr 模块） ====================
 *   账号表和连接逻辑全部由 lib/wifimgr 负责，本模块只调用它：
 *     - 最多 8 组 SSID/密码，保存在 ESP32 NVS 里（断电不丢，不是普通全局数组）
 *     - 开机先把 NVS 里的账号读回来，STA 模式依次尝试连接
 *     - 连不上就开配网热点 ESP32-MP3-Setup / mp3setup123，网页 http://192.168.4.1/
 *     - 已连上路由器时网页在 http://<sta-ip>/（串口会打印），另有 http://mp3player.local/
 *   网页表单里填 SSID + 密码保存 -> 立刻写 NVS 并重连。
 *
 * ==================== 依赖与调用者 ====================
 *   依赖：wifimgr（WiFi 配置网页，本模块借它的 WebServer 挂 /upload）、SPI2 主机 + GPIO。
 *   调用者：src/main.cpp 的 setup()（netdl_init）；传输由网页 POST /upload 的回调驱动
 *           （netdl_upload_begin / write / end / abort）。
 */

#include <stdint.h>

/* ---------------- 引脚 ---------------- */
#define NETDL_SPI_SCK     12
#define NETDL_SPI_MOSI    11
#define NETDL_SPI_MISO    13
#define NETDL_SPI_CS      10
#define NETDL_READY_PIN    9

/* SPI 时钟：8 MHz 保守值（STM32F407 SPI2 从机在 APB1=42 MHz 下绰绰有余）。
   想更快可改 16 MHz，但要注意杜邦线质量与 STM32 主循环写卡速度。 */
#define NETDL_SPI_HZ      (8 * 1000 * 1000)

/* ---------------- 帧 ---------------- */
#define NETDL_FRAME       2064u      /* 16 B 头 + 2048 B 载荷，固定长度 */
#define NETDL_PAYLOAD     2048u
#define NETDL_SYNC0       0xA5u
#define NETDL_SYNC1       0x5Au
#define NETDL_NAME_MAX    96u

/* MOSI 头偏移（ESP32 -> STM32） */
#define NM_SYNC0          0u
#define NM_SYNC1          1u
#define NM_TYPE           2u
#define NM_FLAGS          3u
#define NM_SEQ            4u
#define NM_LEN            6u
#define NM_TOTAL          8u
#define NM_CRC            12u
#define NM_PAYLOAD        16u

/* MISO 头偏移（STM32 -> ESP32） */
#define NS_SYNC0          0u
#define NS_SYNC1          1u
#define NS_STATUS         2u
#define NS_ERRCODE        3u
#define NS_ACKTYPE        4u
#define NS_ACKSEQ         6u
#define NS_WRITTEN        8u
#define NS_TOTAL          12u
#define NS_MSG            16u

/* 帧类型 */
#define NT_BEGIN          1u
#define NT_DATA           2u
#define NT_END            3u
#define NT_ABORT          4u
#define NT_PING           5u

/* 状态 */
#define NS_ST_OK          0u
#define NS_ST_BUSY        1u
#define NS_ST_ERROR       2u

/* 模块状态 */
#define NETDL_IDLE        0
#define NETDL_RUNNING     1
#define NETDL_OK          2
#define NETDL_FAIL        3

/* ---------------- API ---------------- */
void        netdl_init(void);                                   /* SPI 主机 + READY 输入 + 挂 /upload 上传页 */

/* ---- 网页上传（由 WebServer 的上传回调驱动；同时只允许一个会话） ---- */
bool        netdl_upload_begin(const char *name, uint32_t total);   /* 发 T_BEGIN；false = 忙/参数错 */
bool        netdl_upload_write(const uint8_t *data, uint32_t len);  /* 发若干 T_DATA（自动切成 2048 B） */
bool        netdl_upload_end(void);                                 /* 发 T_END + T_PING 收尾；true = 写卡成功 */
void        netdl_upload_abort(void);                               /* 中止，并让 STM32 删掉 .part */
bool        netdl_upload_running(void);                             /* 网页上传是否正占着会话 */
bool        netdl_busy(void);
int         netdl_state(void);
uint32_t    netdl_written(void);
uint32_t    netdl_total(void);
const char *netdl_name(void);
const char *netdl_msg(void);

#endif /* ESP_NETDL_H */
