#ifndef __BSP_SPID_H
#define __BSP_SPID_H

/*
 * bsp_spid.h - 板间「网页上传存卡」专用 SPI 从机驱动（STM32F407 侧）
 *
 * ==================== 为什么另起一条 SPI ====================
 * 现有板间 UART（USART2 @1 Mbps）用来送音频流 + 控制 + 断点续播位置回报，
 * 不能也不该拿来做整首歌（几 MB）的上传搬运：1 Mbps 理论 100 KB/s，
 * 一首 5 MB 的歌要 50 s 以上。所以按需求另起一条独立 SPI 链路，专做
 * 「浏览器选文件 → ESP32-S3 → STM32 写 TF 卡」，两条链路互不干扰。
 *
 * ==================== 接线（新增 5 根线 + 共地） ====================
 *   信号         STM32F407(天空星)        ESP32-S3        方向
 *   SPI_SCK      PB13 (SPI2_SCK  AF5)     GPIO12          ESP32 -> STM32
 *   SPI_MOSI     PB15 (SPI2_MOSI AF5)     GPIO11          ESP32 -> STM32
 *   SPI_MISO     PB14 (SPI2_MISO AF5)     GPIO13          STM32 -> ESP32
 *   SPI_CS       PB12 (GPIO 输入上拉)      GPIO10          ESP32 -> STM32
 *   SPID_READY   PB11 (GPIO 推挽输出)      GPIO9 (输入)    STM32 -> ESP32
 *   GND          GND                      GND             必须共地
 *
 * 选脚理由：LCD 已占 SPI1(PA5/PA7/PA4/PB0/PB1/PB10)，SPI3 默认脚 PC10-PC12
 * 被 SDIO 占，SPI3 remap 的 PB3/PB4 是 SWO/NJTRST；PB11-PB15 全部空闲，
 * 且都挂在同一端口便于走线。
 *
 * ==================== 电气/协议约定 ====================
 * - SPI2 从机，8 bit，CPOL_Low + CPHA_1Edge（Mode 0），MSB first。
 * - NSS 用**软件管理**（SSM=1 + SSI=1，永久选中），PB12 的 CS 不接 SPI 外设，
 *   只当普通输入 + EXTI 用来界定事务边界（规避 F4 从机硬件 NSS 的首字节坑）。
 * - **每次事务固定 2064 字节**（16 B 头 + 2048 B 载荷）：长度固定，从机才能
 *   用 DMA 的 NDTR 直接界定事务，不需要事先知道长度。
 * - 流控靠 READY：从机 READY=高 才表示「两个 DMA 都已 Arm 好，可以来下一帧」。
 *   ESP32 每帧前必须等 READY 变高（超时判错）。
 * - 应答滞后一帧：主机发第 N 帧时，MISO 上回的是第 N-1 帧的处理结果。
 *   所以最后一帧之后要再发一帧 T_PING 取最终确认。
 *
 * ==================== 帧格式（MOSI: ESP32 -> STM32） ====================
 *   [0]      u8  sync0 = 0xA5
 *   [1]      u8  sync1 = 0x5A
 *   [2]      u8  type    （SPID_T_xxx）
 *   [3]      u8  flags   （保留，填 0）
 *   [4..5]   u16 seq     块序号（T_DATA: 从 0 起；其它填 0）
 *   [6..7]   u16 len     载荷有效字节数（<= 2048）
 *   [8..11]  u32 total   T_BEGIN: 文件总字节数；其它填 0
 *   [12..15] u32 crc32   载荷（或文件名）的 CRC32，0 = 不校验
 *   [16..2063] u8 payload[2048]
 *
 * ==================== 帧格式（MISO: STM32 -> ESP32） ====================
 *   [0]      u8  sync0 = 0x5A
 *   [1]      u8  sync1 = 0xA5
 *   [2]      u8  status  （SPID_ST_xxx）
 *   [3]      u8  errcode （SPID_E_xxx）
 *   [4..5]   u16 ack_type  已处理完的上一帧类型
 *   [6..7]   u16 ack_seq   已成功写入的最后一块 seq
 *   [8..11]  u32 written   已写入字节数
 *   [12..15] u32 total     当前文件总字节数（T_BEGIN 之后已知）
 *   [16..79] char msg[64]  ASCII 状态文字（printf 风格提示）
 *   其余保留 0xFF
 *
 * ==================== 调用流程 ====================
 *   spid_init();
 *   for (;;) {
 *       if (spid_frame_ready()) {
 *           const uint8_t *rx = spid_rx_frame();
 *           uint8_t *tx = spid_tx_frame();
 *           ...解析 rx、写 SD 卡、填 tx...
 *           spid_frame_done();          // 重新 Arm 两个 DMA + 拉高 READY
 *       }
 *   }
 */
#include "stm32f4xx.h"
#include <stdint.h>

/* ---------------- 帧长度 ---------------- */
#define SPID_FRAME        2064u     /* 16 B 头 + 2048 B 载荷（每次事务固定长度） */
#define SPID_PAYLOAD_MAX  2048u

/* ---------------- magic ---------------- */
#define SPID_MOSI_SYNC0   0xA5u
#define SPID_MOSI_SYNC1   0x5Au
#define SPID_MISO_SYNC0   0x5Au
#define SPID_MISO_SYNC1   0xA5u

/* ---------------- MOSI 头偏移 ---------------- */
#define SPID_M_SYNC0      0u
#define SPID_M_SYNC1      1u
#define SPID_M_TYPE       2u
#define SPID_M_FLAGS      3u
#define SPID_M_SEQ        4u    /* u16 LE */
#define SPID_M_LEN        6u    /* u16 LE */
#define SPID_M_TOTAL      8u    /* u32 LE */
#define SPID_M_CRC        12u   /* u32 LE */
#define SPID_M_PAYLOAD    16u

/* ---------------- MISO 头偏移 ---------------- */
#define SPID_S_SYNC0      0u
#define SPID_S_SYNC1      1u
#define SPID_S_STATUS     2u
#define SPID_S_ERRCODE    3u
#define SPID_S_ACKTYPE    4u    /* u16 LE */
#define SPID_S_ACKSEQ     6u    /* u16 LE */
#define SPID_S_WRITTEN    8u    /* u32 LE */
#define SPID_S_TOTAL      12u   /* u32 LE */
#define SPID_S_MSG        16u   /* 64 B ASCII */
#define SPID_S_MSG_LEN    64u

/* ---------------- 帧类型 ---------------- */
#define SPID_T_BEGIN      1u    /* 开始：payload = 文件名(UTF-8)，total = 文件大小 */
#define SPID_T_DATA       2u    /* 数据：seq = 块号，payload = 数据 */
#define SPID_T_END        3u    /* 结束 */
#define SPID_T_ABORT      4u    /* 中止（从机删掉半成品） */
#define SPID_T_PING       5u    /* 只取状态 */

/* ---------------- 状态 / 错误码 ---------------- */
#define SPID_ST_OK        0u
#define SPID_ST_BUSY      1u
#define SPID_ST_ERROR     2u

#define SPID_E_NONE       0u
#define SPID_E_BADFRAME   1u    /* 头/CRC 校验不过 */
#define SPID_E_NOFILE     2u    /* 还没 BEGIN 就来 DATA */
#define SPID_E_OPEN       3u    /* 建文件失败（卡满/写保护/目录不存在） */
#define SPID_E_WRITE      4u    /* f_write 失败 */
#define SPID_E_SEQ        5u    /* 块序号不连续 */
#define SPID_E_BADNAME    6u    /* 文件名非法（空/含路径/后缀不允许） */
#define SPID_E_RENAME     7u    /* 收尾改名失败 */
#define SPID_E_NOCARD     8u    /* 卡不在 */

/* ---------------- API ---------------- */
void     spid_init(void);

/* 有完整一帧待处理（RX DMA 已完成，READY 已自动拉低） */
uint8_t  spid_frame_ready(void);

/* 收到的那一帧（2064 B，DMA 已停，主循环可安全读） */
const uint8_t *spid_rx_frame(void);

/* 待发送的应答缓冲（2064 B，主循环填完再调 spid_frame_done） */
uint8_t *spid_tx_frame(void);

/* 处理完毕：重新 Arm TX/RX DMA 并拉高 READY，允许 ESP32 发下一帧 */
void     spid_frame_done(void);

/* 丢弃当前帧（异常恢复）：同 done，但计一次错误 */
void     spid_abort_frame(void);

/* 诊断 */
uint8_t  spid_ready(void);      /* 当前 READY 电平 */
uint32_t spid_frames(void);     /* 收到的完整帧数 */
uint32_t spid_errs(void);       /* 帧校验失败 / CS 提前结束的对齐错误数 */
uint32_t spid_dma_errs(void);   /* DMA 传输错误数 */

#endif /* __BSP_SPID_H */
