#ifndef ESP_LINK_H
#define ESP_LINK_H

/*
 * link.h - 板间串口链路（UART1, 1 Mbps, GPIO17=RX / GPIO18=TX）
 *
 * 作用：把 UART1 上收到的字节流切成一帧帧协议帧，并提供发帧接口。
 * 帧格式：[0xAA][0x55][type][len_lo][len_hi][payload][crc_lo][crc_hi]
 *         CRC-16/CCITT-FALSE，覆盖 type + len + payload（见 proto.h）。
 * 依赖：proto.h（组帧/流式解析）、driver/uart.h。
 * 调用者：player.cpp（task_link 收发协议帧）、netdl.cpp（发命令/等应答）。
 *
 * 接线（与接线图一致）：
 *   STM32 PA2(TX) --> ESP32 GPIO17(RX)
 *   STM32 PA3(RX) <-- ESP32 GPIO18(TX)
 */
#include <stdint.h>
#include "proto.h"

#define LINK_BAUD   1000000
#define LINK_RX_PIN 17
#define LINK_TX_PIN 18
#define LINK_RX_BUF 8192

/* 初始化 UART1（1 Mbps 8N1） */
void link_init(void);

/* 发一帧（小帧阻塞发送，不丢） */
void link_send(uint8_t type, const uint8_t *payload, uint16_t len);

/* 便利函数 */
void link_send_u16(uint8_t type, uint16_t v);
void link_send_u32(uint8_t type, uint32_t v);

/*
 * 等一帧。收到返回 true，*type/*len 有效，*pl 指向解析器内部缓冲
 * （下次调用 link_wait_frame 前有效，回调里要立刻用/拷贝）。
 * 超时返回 false。
 */
bool link_wait_frame(uint8_t *type, const uint8_t **pl, uint16_t *len,
                     uint32_t timeout_ms);

#endif /* ESP_LINK_H */
