/*
 * bsp_proto.c - 板间串口帧协议（STM32 侧）
 *
 * 帧格式（小端，CRC 覆盖 type + len + payload）：
 *   [0xAA][0x55][type][len_lo][len_hi][payload...][crc_lo][crc_hi]
 *   CRC-16/CCITT-FALSE：poly 0x1021、init 0xFFFF、不反射、无最终异或
 *
 * 承载：USART2（bsp_uart2）1 Mbps；发帧一次性投入 TX 环，环装不下则整帧丢弃
 *       （收端靠 AA55 重新同步）；收帧是非阻塞状态机，逐字节从 RX 环取。
 * 依赖：bsp_proto.h（类型码/长度上限）、bsp_uart2.h（环形缓冲）、string.h。
 * 调用：main.c 初始化 proto_init()，主循环 proto_recv() 取命令，应答/音频用 proto_send()。
 */

#include "bsp_proto.h"
#include "bsp_uart2.h"
#include <string.h>

/* 逐字节更新 CRC-16/CCITT-FALSE（poly 0x1021、init 0xFFFF、不反射、无最终异或） */
static uint16_t crc16_update(uint16_t crc, uint8_t data)
{
    uint8_t j;
    crc ^= (uint16_t)((uint16_t)data << 8);
    for (j = 0u; j < 8u; j++)
        crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u)
                               : (uint16_t)(crc << 1);
    return crc;
}

/* 发帧诊断：TX 环装不下整帧而丢弃的帧数（0 = 正常） */
static uint32_t s_frame_drops = 0;

/* 收帧诊断计数（0 = 正常） */
static uint32_t s_rx_bad_crc = 0u;   /* CRC 校验失败而丢弃的帧数 */
static uint32_t s_rx_bad_len = 0u;   /* 长度超过 PROTO_MAX_PAYLOAD 而丢弃的帧数 */

void proto_send(uint8_t type, const uint8_t *payload, uint16_t len)
{
    uint8_t hdr[5u];
    uint16_t crc;
    uint16_t i;

    /* 帧开销 = 5 字节头 + 2 字节 CRC = 7 字节（这里按 len+8 保守判断）。TX 环装不下
     * 整帧就整帧丢弃，收端会靠 AA55 重新同步；TX 环有 4096 B，只有拥塞时才会触发。 */
    if (uart2_tx_free() < (uint16_t)(len + 8u))
    {
        s_frame_drops++;
        return;
    }

    hdr[0u] = PROTO_SYNC0;
    hdr[1u] = PROTO_SYNC1;
    hdr[2u] = type;
    hdr[3u] = (uint8_t)(len & 0xFFu);
    hdr[4u] = (uint8_t)(len >> 8);

    /* CRC-16 覆盖 type + len_lo + len_hi + payload */
    crc = crc16_update(0xFFFFu, type);
    crc = crc16_update(crc, hdr[3u]);
    crc = crc16_update(crc, hdr[4u]);
    for (i = 0u; i < len; i++)
        crc = crc16_update(crc, payload[i]);

    uart2_putc(hdr[0u]);
    uart2_putc(hdr[1u]);
    uart2_putc(hdr[2u]);
    uart2_putc(hdr[3u]);
    uart2_putc(hdr[4u]);
    for (i = 0u; i < len; i++)
        uart2_putc(payload[i]);
    uart2_putc((uint8_t)(crc & 0xFFu));          /* CRC lo */
    uart2_putc((uint8_t)(crc >> 8));             /* CRC hi */
}

/* 累计丢帧数 = 整帧丢弃数 + TX 环丢字节数（0 = 正常） */
uint32_t proto_drops(void)
{
    return s_frame_drops + uart2_tx_drops();
}

/* CRC 校验失败的帧数（诊断） */
uint32_t proto_rx_bad_crc(void) { return s_rx_bad_crc; }
/* 长度超限的帧数（诊断） */
uint32_t proto_rx_bad_len(void) { return s_rx_bad_len; }

/* ==================================================================
 * 接收状态机
 * ================================================================== */
typedef enum
{
    RX_SYNC0,      /* 等 0xAA */
    RX_SYNC1,      /* 等 0x55 */
    RX_TYPE,
    RX_LEN_LO,
    RX_LEN_HI,
    RX_PAYLOAD,
    RX_CRC_LO,
    RX_CRC_HI
} rx_state_t;

static rx_state_t s_state;                    /* 当前状态 */
static uint8_t  s_type;                       /* 本帧类型码 */
static uint16_t s_len;                        /* 本帧载荷长度 */
static uint16_t s_idx;                        /* 载荷已收字节数 */
static uint16_t s_crc;                        /* 边收边算的 CRC */
static uint8_t  s_crc_lo;                     /* 收到的 CRC 低字节（暂存） */
static uint8_t  s_buf[PROTO_MAX_PAYLOAD];     /* 载荷暂存区 */

/* 复位接收状态机，丢弃半截帧（初始化时调用）
 * 注意：半截帧靠 AA55 重新同步，这里不做超时清零 */
void proto_init(void)
{
    s_state = RX_SYNC0;
    s_len   = 0u;
    s_idx   = 0u;
}

/* 非阻塞收帧：把 RX 环里的字节喂给状态机，收到完整且 CRC 正确的帧返回 1；
 * 载荷最多拷贝 maxlen 字节到 buf，*len_out 给出帧内实际长度（可能大于 maxlen） */
uint8_t proto_recv(uint8_t *type, uint8_t *buf, uint16_t maxlen,
                   uint16_t *len_out)
{
    uint8_t byte;

    while (uart2_getc(&byte) != 0)
    {
        switch (s_state)
        {
        case RX_SYNC0:
            if (byte == PROTO_SYNC0)
                s_state = RX_SYNC1;
            break;

        case RX_SYNC1:
            if (byte == PROTO_SYNC1)
                s_state = RX_TYPE;
            else if (byte == PROTO_SYNC0)
                ; /* 又收到 0xAA，可能是一帧的新起点，保持等待 0x55 */
            else
                s_state = RX_SYNC0;
            break;

        case RX_TYPE:
            s_type = byte;
            s_crc  = crc16_update(0xFFFFu, byte);
            s_state = RX_LEN_LO;
            break;

        case RX_LEN_LO:
            s_len = (uint16_t)byte;
            s_crc = crc16_update(s_crc, byte);
            s_state = RX_LEN_HI;
            break;

        case RX_LEN_HI:
            {
                uint16_t full = (uint16_t)(s_len | ((uint16_t)byte << 8));
                s_crc = crc16_update(s_crc, byte);
                s_len = full;
                if (full > PROTO_MAX_PAYLOAD)
                {
                    s_rx_bad_len++;
                    s_state = RX_SYNC0;      /* 长度非法：丢帧并重新同步 */
                    break;
                }
                s_idx = 0u;
                s_state = (full == 0u) ? RX_CRC_LO : RX_PAYLOAD;
            }
            break;

        case RX_PAYLOAD:
            if (s_idx < PROTO_MAX_PAYLOAD)
                s_buf[s_idx] = byte;
            s_crc = crc16_update(s_crc, byte);
            s_idx++;
            if (s_idx >= s_len)
                s_state = RX_CRC_LO;
            break;

        case RX_CRC_LO:
            s_crc_lo = byte;
            s_state = RX_CRC_HI;
            break;

        case RX_CRC_HI:
        {
            uint16_t rcv = (uint16_t)((uint16_t)byte << 8) | s_crc_lo;
            s_state = RX_SYNC0;
            if (rcv == s_crc)
            {
                uint16_t copy = (s_len <= maxlen) ? s_len : maxlen;
                if (copy > 0u)
                    memcpy(buf, s_buf, copy);
                *type    = s_type;
                *len_out = s_len;
                return 1u;
            }
            /* CRC 不匹配：丢帧，继续扫描下一帧 */
            s_rx_bad_crc++;
            break;
        }
        }
    }
    return 0u;
}
