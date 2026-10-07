/*
 * proto.cpp - 板间协议实现：CRC-16/CCITT-FALSE、组帧、流式解析
 *   与 STM32 侧 bsp_proto.c 行为一致，帧格式见 proto.h。解析器逐字节喂入，
 *   收齐一帧返回 true；长度非法或 CRC 错则丢弃并重新同步。
 *   依赖：无（只用 string.h）。调用者：link.cpp（组帧/解析）、player.cpp（payload 读写）。
 */
#include "proto.h"
#include <string.h>

/* ---- CRC-16/CCITT-FALSE ---------------------------------------------- */
static inline uint16_t crc16_update(uint16_t crc, uint8_t data)
{
    uint8_t j;
    crc ^= (uint16_t)((uint16_t)data << 8);
    for (j = 0u; j < 8u; j++)
        crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u)
                              : (uint16_t)(crc << 1);
    return crc;
}

uint16_t proto_crc16(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0xFFFFu;
    uint16_t i;
    for (i = 0u; i < len; i++) crc = crc16_update(crc, data[i]);
    return crc;
}

/* ---- 组帧 ------------------------------------------------------------ */
uint16_t proto_build(uint8_t *out, uint16_t cap, uint8_t type,
                     const uint8_t *payload, uint16_t len)
{
    uint16_t crc, i;

    if ((out == 0) || ((uint32_t)len + PROTO_OVERHEAD > (uint32_t)cap)) return 0u;

    out[0] = PROTO_SYNC0;
    out[1] = PROTO_SYNC1;
    out[2] = type;
    out[3] = (uint8_t)(len & 0xFFu);
    out[4] = (uint8_t)(len >> 8);

    crc = crc16_update(0xFFFFu, type);
    crc = crc16_update(crc, out[3]);
    crc = crc16_update(crc, out[4]);
    for (i = 0u; i < len; i++) {
        out[5u + i] = payload[i];
        crc = crc16_update(crc, payload[i]);
    }
    out[5u + len]      = (uint8_t)(crc & 0xFFu);
    out[5u + len + 1u] = (uint8_t)(crc >> 8);
    return (uint16_t)(len + PROTO_OVERHEAD);
}

/* ---- 流式解析 -------------------------------------------------------- */
enum { ST_SYNC0 = 0, ST_SYNC1, ST_TYPE, ST_LEN_LO, ST_LEN_HI,
       ST_PAYLOAD, ST_CRC_LO, ST_CRC_HI };

void ProtoParser::reset()
{
    m_state = ST_SYNC0;
    m_len = 0;
    m_idx = 0;
    m_crc = 0;
    m_crc_lo = 0;
}

bool ProtoParser::feed(uint8_t b)
{
    switch (m_state) {
    case ST_SYNC0:
        if (b == PROTO_SYNC0) m_state = ST_SYNC1;
        break;

    case ST_SYNC1:
        if (b == PROTO_SYNC1)      m_state = ST_TYPE;
        else if (b == PROTO_SYNC0) m_state = ST_SYNC1;  /* 可能是新帧起点 */
        else                       m_state = ST_SYNC0;
        break;

    case ST_TYPE:
        m_type = b;
        m_crc = crc16_update(0xFFFFu, b);
        m_state = ST_LEN_LO;
        break;

    case ST_LEN_LO:
        m_len = (uint16_t)b;
        m_crc = crc16_update(m_crc, b);
        m_state = ST_LEN_HI;
        break;

    case ST_LEN_HI: {
        uint16_t full = (uint16_t)(m_len | ((uint16_t)b << 8));
        m_crc = crc16_update(m_crc, b);
        m_len = full;
        if (full > PROTO_MAX_PAYLOAD) {
            m_state = ST_SYNC0;            /* 长度非法：重新同步 */
            break;
        }
        m_idx = 0u;
        m_state = (full == 0u) ? ST_CRC_LO : ST_PAYLOAD;
        break;
    }

    case ST_PAYLOAD:
        m_payload[m_idx++] = b;
        m_crc = crc16_update(m_crc, b);
        if (m_idx >= m_len) m_state = ST_CRC_LO;
        break;

    case ST_CRC_LO:
        m_crc_lo = b;
        m_state = ST_CRC_HI;
        break;

    case ST_CRC_HI: {
        uint16_t rcv = (uint16_t)(((uint16_t)b << 8) | m_crc_lo);
        m_state = ST_SYNC0;
        if (rcv == m_crc) return true;
        break;                             /* CRC 错：丢弃该帧 */

    }

    default:
        m_state = ST_SYNC0;
        break;
    }
    return false;
}
