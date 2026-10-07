#ifndef BSP_PROTO_H
#define BSP_PROTO_H

/*
 * bsp_proto.h - 板间串口帧协议（STM32 侧）
 *
 * 帧格式（小端，CRC-16/CCITT-FALSE）：
 *   [0xAA][0x55][type][len_lo][len_hi][payload...][crc_lo][crc_hi]
 *   CRC 覆盖 type + len_lo + len_hi + payload
 *   （poly 0x1021、init 0xFFFF、不反射、无最终异或）
 *
 * 承载：USART2 PA2(TX)/PA3(RX)，1 Mbps，对端是 ESP32-S3。
 */
#include <stdint.h>

#define PROTO_SYNC0         0xAAu
#define PROTO_SYNC1         0x55u
#define PROTO_MAX_PAYLOAD   1040u     /* >= RESP_AUDIO: 6 头 + 1024 数据 */

/* ---- ESP32 -> STM32 (commands, 0x1x) ---- */
#define CMD_PLAY            0x10u      /* u16 track                         */
#define CMD_PAUSE           0x11u      /* none                              */
#define CMD_RESUME          0x12u      /* none                              */
#define CMD_NEXT            0x13u      /* none                              */
#define CMD_PREV            0x14u      /* none                              */
#define CMD_STOP            0x15u      /* none                              */
#define CMD_VOL             0x16u      /* u8 vol (0-100)                    */
#define CMD_MODE            0x17u      /* u8 mode (0=list 1=single 2=random)*/
#define CMD_SEEK            0x18u      /* u32 ms                            */
#define CMD_AUDIO_REQ       0x19u      /* u16 blocks (credit request)       */
#define CMD_LIST_REQ        0x1Au      /* u16 from, u16 count               */
#define CMD_PING            0x1Bu      /* u32 tick                          */
#define CMD_SYNC_POS        0x1Cu      /* u16 track, u32 offset, u32 seq    */

/* ---- STM32 -> ESP32 (responses, 0x2x) ---- */
#define RESP_AUDIO          0x20u      /* u32 seq, u16 len, data[len]       */
#define RESP_STATE          0x21u      /* u16 track, u16 total, u8 state... */
#define RESP_TIME           0x22u      /* u32 ms, u32 total_ms              */
#define RESP_LIST           0x23u      /* u16 index, u16 total, char name[64]*/
#define RESP_END            0x24u      /* u16 track, u8 reason              */
#define RESP_PONG           0x25u      /* u32 tick, u16 drops               */
#define RESP_ACK            0x26u      /* u8 cmd, u8 status                 */

/* ---- API ---- */

/* 复位接收状态机（初始化时调用） */
void     proto_init(void);

/* 发送一帧（非阻塞：整帧投入 UART2 TX 环，环装不下则整帧丢弃） */
void     proto_send(uint8_t type, const uint8_t *payload, uint16_t len);

/* 累计丢帧/丢字节数（TX 环满导致，0 = 正常） */
uint32_t proto_drops(void);

/* 收帧诊断：CRC 校验失败数 / 长度超限数（0 = 正常） */
uint32_t proto_rx_bad_crc(void);
uint32_t proto_rx_bad_len(void);

/* 非阻塞收帧：收到完整且 CRC 正确的帧返回 1 */
uint8_t  proto_recv(uint8_t *type, uint8_t *buf, uint16_t maxlen,
                    uint16_t *len_out);

#endif /* BSP_PROTO_H */
