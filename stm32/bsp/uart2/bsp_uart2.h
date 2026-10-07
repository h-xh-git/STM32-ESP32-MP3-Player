/*
 * bsp_uart2.h - 与 ESP32-S3 的板间串口链路（USART2，PA2=TX / PA3=RX，AF7）
 *
 * 双向字节流通道，承载 bsp_proto 的帧 [0xAA][0x55][type][len LE][payload][crc16 LE]，
 * 默认 1 Mbps、8N1；收发都带环形缓冲，接口全非阻塞，不会让主循环卡住。
 * 基于 StdPeriph 库，库文件不改动。
 */
#ifndef __BSP_UART2_H__
#define __BSP_UART2_H__

#include "stm32f4xx.h"
#include <stdint.h>

/* 按给定波特率初始化 USART2，开 RX 中断 + 收发环形缓冲 */
void     uart2_init(uint32_t baud);

/* 非阻塞发送：字节投入 TX 环，由 TXE 中断送出；只有环完全满才会被静默丢弃
 * （计入 s_tx_drops），调用者永远不会被阻塞。 */
void     uart2_putc(uint8_t c);

/* TX 环剩余可写空间（还能投多少字节） */
uint16_t uart2_tx_free(void);

/* 因 TX 环满丢弃的累计字节数 */
uint32_t uart2_tx_drops(void);

/* 因 RX 环满丢弃的累计字节数（0 = 正常） */
uint32_t uart2_rx_drops(void);

/* 非阻塞取字节：取到返回 1，环空返回 0 */
int      uart2_getc(uint8_t *out_c);

/* RX 环中待读字节数 */
uint16_t uart2_available(void);

/* 中断服务函数（在中断向量表里挂接） */
void USART2_IRQHandler(void);

#endif /* __BSP_UART2_H__ */
