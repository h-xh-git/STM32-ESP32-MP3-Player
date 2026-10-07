#ifndef ESP_PROTO_H
#define ESP_PROTO_H

/*
 * proto.h - 板间串口协议（ESP32 侧，与 STM32 bsp_proto.h 完全一致）
 *
 * 帧格式（小端，CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, 不反转, 不异或）:
 *   [0xAA][0x55][type][len_lo][len_hi][payload...][crc_lo][crc_hi]
 *   CRC 覆盖 type + len_lo + len_hi + payload
 *
 * 注意：RESP_AUDIO 的 payload = 6 + 数据长度(<=1024) = 最大 1030 字节，
 *       所以本侧解析上限取 1040（STM32 侧只收短命令，其上限 1024 不冲突）。
 *
 * 依赖：无（只用 stdint.h）。调用者：link.cpp（组帧 + 解析）、player.cpp（payload 读写）。
 */
#include <stdint.h>

#define PROTO_SYNC0        0xAAu
#define PROTO_SYNC1        0x55u
#define PROTO_MAX_PAYLOAD  1040u
#define PROTO_OVERHEAD     8u      /* 2 sync + 1 type + 2 len + 2 crc (+1 余量) */

/* ---- ESP32 -> STM32 (CMD, 0x1x) ---- */
#define CMD_PLAY        0x10u      /* u16 track                          */
#define CMD_PAUSE       0x11u
#define CMD_RESUME      0x12u
#define CMD_NEXT        0x13u
#define CMD_PREV        0x14u
#define CMD_STOP        0x15u
#define CMD_VOL         0x16u      /* u8 vol (0-100)                     */
#define CMD_MODE        0x17u      /* u8 mode (0=list 1=single 2=random) */
#define CMD_SEEK        0x18u      /* u32 ms                             */
#define CMD_AUDIO_REQ   0x19u      /* u16 blocks (信用请求)              */
#define CMD_LIST_REQ    0x1Au      /* u16 from, u16 count                */
#define CMD_PING        0x1Bu      /* u32 tick                           */
#define CMD_SYNC_POS    0x1Cu      /* u16 track, u32 offset_ms, u32 seq  */

/* ---- STM32 -> ESP32 (RESP, 0x2x) ---- */
#define RESP_AUDIO      0x20u      /* u32 seq, u16 len, data[len]        */
#define RESP_STATE      0x21u      /* u16 track, u16 total, u8 playing, u8 mode, u8 vol */
#define RESP_TIME       0x22u      /* u32 ms, u32 total_ms               */
#define RESP_LIST       0x23u      /* u16 index, u16 total, char name[]  */
#define RESP_END        0x24u      /* u16 track, u8 reason               */
#define RESP_PONG       0x25u      /* u32 tick, u16 drops                */
#define RESP_ACK        0x26u      /* u8 cmd, u8 status                  */

/* CRC-16/CCITT-FALSE */
uint16_t proto_crc16(const uint8_t *data, uint16_t len);

/* 组帧到 out（需要 >= len + PROTO_OVERHEAD 字节），返回总字节数；0 = 参数非法 */
uint16_t proto_build(uint8_t *out, uint16_t cap, uint8_t type,
                     const uint8_t *payload, uint16_t len);

/* 流式解析器：逐字节喂入，收齐一帧返回 true */
class ProtoParser {
public:
    ProtoParser() { reset(); }
    void reset();
    bool feed(uint8_t b);

    uint8_t        type() const { return m_type; }
    const uint8_t *data() const { return m_payload; }
    uint16_t       len()  const { return m_len; }

private:
    uint8_t  m_state = 0;
    uint8_t  m_type = 0;
    uint16_t m_len = 0;
    uint16_t m_idx = 0;
    uint16_t m_crc = 0;
    uint8_t  m_crc_lo = 0;
    uint8_t  m_payload[PROTO_MAX_PAYLOAD];
};

/* 小端读写工具 */
static inline uint16_t proto_get_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static inline uint32_t proto_get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline void proto_put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu); p[1] = (uint8_t)(v >> 8);
}
static inline void proto_put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu); p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu); p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

#endif /* ESP_PROTO_H */
