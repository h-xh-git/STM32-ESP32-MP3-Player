/*
 * netdl.c - 「网页上传存 TF 卡」的 STM32 侧服务（协议解析 + FatFS 写卡）
 *
 * 上游：bsp_spid.c 每收满一帧 2064 B 就置 spid_frame_ready()，本模块在主循环里
 *       取帧、校验、写卡、填好应答，再 spid_frame_done() 放行下一帧。
 * 下游：FatFS（FF_FS_TINY=1，单线程，无锁）。
 *
 * 帧格式、状态码、错误码见 bsp_spid.h。
 *
 * 文件落盘策略（避免歌单扫到半成品）：
 *   T_BEGIN  -> 打开 "<base>/<名字>.part"（FA_CREATE_ALWAYS|FA_WRITE）
 *   T_DATA   -> f_write 每块（2 KB），累积 written
 *   T_END    -> f_sync + f_close + f_unlink(最终名) + f_rename(.part -> 最终名)
 *   T_ABORT  -> f_close + f_unlink(.part)
 *   20 s 无帧 -> 同上按 ABORT 处理（防止卡一直开着半成品）
 */

#include "netdl.h"
#include "bsp_spid.h"
#include "bsp_tick.h"
#include "playlist.h"
#include "ff.h"

#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
#define NETDL_NAME_MAX   96u      /* 文件名上限（含结尾 0） */
#define NETDL_PATH_MAX   128u     /* 完整路径上限 */
#define NETDL_IDLE_MS    20000u   /* 多久没收到帧就放弃 */

/* CRC-32（反射，poly 0xEDB88320，init/xor 0xFFFFFFFF）的 16 项半字节表。
   与 ESP32 侧 netdl.cpp 用同一算法，两边必须保持一致。 */
static const uint32_t s_crc_tab[16] =
{
    0x00000000u, 0x77073096u, 0xEE0E612Cu, 0x990951BAu,
    0x076DC419u, 0x706AF48Fu, 0xE963A535u, 0x9E6495A3u,
    0x0EDB8832u, 0x79DCB8A4u, 0xE0D5E91Eu, 0x97D2D988u,
    0x09B64C2Bu, 0x7EB17CBDu, 0xE7B82D07u, 0x90BF1D91u
};

/* 算 SPI 帧数据段的 CRC32（多项式 0x04C11DB7，与 ESP32 侧一致） */
static uint32_t netdl_crc32(const uint8_t *p, uint32_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t i;

    for (i = 0u; i < n; i++)
    {
        crc ^= (uint32_t)p[i];
        crc = (crc >> 4) ^ s_crc_tab[crc & 0x0Fu];
        crc = (crc >> 4) ^ s_crc_tab[crc & 0x0Fu];
    }
    return crc ^ 0xFFFFFFFFu;
}

/* 读小端 u16 */
static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* 读小端 u32 */
static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* 写小端 u16 */
static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

/* 写小端 u32 */
static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/* ------------------------------------------------------------------ */
static FIL      s_f;
static uint8_t  s_open      = 0u;
static uint8_t  s_fs_ready  = 0u;
static uint8_t  s_state     = NETDL_IDLE;
static uint8_t  s_done_evt  = 0u;
static uint8_t  s_done_ok   = 0u;
static uint16_t s_next_seq  = 0u;
static uint16_t s_ack_type  = 0u;
static uint16_t s_ack_seq   = 0u;
static uint32_t s_total     = 0u;
static uint32_t s_written   = 0u;
static uint32_t s_last_ms   = 0u;
static uint32_t s_frames    = 0u;
static uint32_t s_errs      = 0u;
static uint32_t s_bytes     = 0u;
static uint32_t s_done_size = 0u;
static char     s_name[NETDL_NAME_MAX];
static char     s_done_name[NETDL_NAME_MAX];
static char     s_part[NETDL_PATH_MAX];
static char     s_final[NETDL_PATH_MAX];
static char     s_msg[SPID_S_MSG_LEN];

/* ------------------------------------------------------------------ */
static void msg_set(const char *t)
{
    size_t n = strlen(t);

    if (n > (size_t)(SPID_S_MSG_LEN - 1u)) n = (size_t)(SPID_S_MSG_LEN - 1u);
    memset(s_msg, 0, sizeof(s_msg));
    memcpy(s_msg, t, n);
}

/* 把状态/应答写进 TX 帧（必须在 spid_frame_done() 之前调用） */
static void reply_fill(uint8_t status, uint8_t errcode)
{
    uint8_t *tx = spid_tx_frame();

    memset(tx, 0xFF, (size_t)SPID_FRAME);
    tx[SPID_S_SYNC0]   = (uint8_t)SPID_MISO_SYNC0;
    tx[SPID_S_SYNC1]   = (uint8_t)SPID_MISO_SYNC1;
    tx[SPID_S_STATUS]  = status;
    tx[SPID_S_ERRCODE] = errcode;
    put_u16(tx + SPID_S_ACKTYPE, s_ack_type);
    put_u16(tx + SPID_S_ACKSEQ,  s_ack_seq);
    put_u32(tx + SPID_S_WRITTEN, s_written);
    put_u32(tx + SPID_S_TOTAL,   s_total);
    memcpy(tx + SPID_S_MSG, s_msg, (size_t)SPID_S_MSG_LEN);
}

/* 文件名合法性：不能带路径分隔符/通配符，必须以 .mp3 或 .wav 结尾 */
static uint8_t name_ok(const char *n, uint16_t len)
{
    uint16_t i;

    if ((len == 0u) || (len >= (uint16_t)NETDL_NAME_MAX)) return 0u;

    for (i = 0u; i < len; i++)
    {
        char c = n[i];
        if ((c == '/') || (c == '\\') || (c == ':') || (c == '*') ||
            (c == '?') || (c == '"')  || (c == '<') || (c == '>') ||
            (c == '|') || (c == 0))
        {
            return 0u;
        }
    }

    if (len >= 4u)
    {
        const char *e = n + (len - 4u);
        if ((e[0] == '.') &&
            ((e[1] == 'm') || (e[1] == 'M')) &&
            ((e[2] == 'p') || (e[2] == 'P')) && (e[3] == '3'))
        {
            return 1u;
        }
        if ((e[0] == '.') &&
            ((e[1] == 'w') || (e[1] == 'W')) &&
            ((e[2] == 'a') || (e[2] == 'A')) &&
            ((e[3] == 'v') || (e[3] == 'V')))
        {
            return 1u;
        }
    }
    return 0u;
}

/* 关掉并删掉半成品 */
static void dl_drop_partial(void)
{
    if (s_open != 0u)
    {
        (void)f_close(&s_f);
        s_open = 0u;
    }
    if (s_part[0] != 0) (void)f_unlink(s_part);
}

/* T_BEGIN */
static uint8_t dl_begin(const uint8_t *name, uint16_t len, uint32_t total)
{
    FRESULT fr;
    const char *base;

    if (s_open != 0u) dl_drop_partial();          /* 上一首没收完，先清掉 */

    if (name_ok((const char *)name, len) == 0u)
    {
        msg_set("bad filename (need .mp3/.wav, no path)");
        return SPID_E_BADNAME;
    }

    memset(s_name, 0, sizeof(s_name));
    memcpy(s_name, name, (size_t)len);

    base = pl_base();
    if (base == 0) base = "";

    (void)snprintf(s_part,  sizeof(s_part),  "%s/%s.part", base, s_name);
    (void)snprintf(s_final, sizeof(s_final), "%s/%s",      base, s_name);

    fr = f_open(&s_f, s_part, (BYTE)(FA_CREATE_ALWAYS | FA_WRITE));
    if (fr != FR_OK)
    {
        (void)snprintf(s_msg, sizeof(s_msg), "f_open fail %d", (int)fr);
        return SPID_E_OPEN;
    }

    s_open    = 1u;
    s_state   = NETDL_OPEN;
    s_total   = total;
    s_written = 0u;
    s_next_seq = 0u;
    s_last_ms = tick_ms();

    (void)snprintf(s_msg, sizeof(s_msg), "begin %s (%lu B)", s_name,
                   (unsigned long)total);
    printf("[NETDL] begin %s total=%lu B\r\n", s_part, (unsigned long)total);
    return SPID_E_NONE;
}

/* T_DATA */
static uint8_t dl_data(uint16_t seq, const uint8_t *p, uint16_t len)
{
    UINT bw = 0u;
    FRESULT fr;

    if (s_open == 0u)        return SPID_E_NOFILE;
    if (seq != s_next_seq)   return SPID_E_SEQ;
    if (len == 0u)           return SPID_E_NONE;

    fr = f_write(&s_f, p, (UINT)len, &bw);
    if ((fr != FR_OK) || (bw != (UINT)len))
    {
        (void)snprintf(s_msg, sizeof(s_msg), "f_write fail %d/%u", (int)fr, (unsigned)bw);
        return SPID_E_WRITE;
    }

    s_written += len;
    s_bytes   += len;
    s_next_seq++;
    s_ack_seq  = seq;
    s_last_ms  = tick_ms();
    return SPID_E_NONE;
}

/* T_END：收尾落盘 */
static uint8_t dl_end(void)
{
    FRESULT fr;
    uint32_t size = s_written;

    if (s_open == 0u) return SPID_E_NOFILE;

    fr = f_sync(&s_f);
    if (fr == FR_OK) fr = f_close(&s_f);
    s_open = 0u;

    if (fr != FR_OK)
    {
        (void)f_unlink(s_part);
        s_state = NETDL_ERROR;
        (void)snprintf(s_msg, sizeof(s_msg), "close/sync fail %d", (int)fr);
        return SPID_E_WRITE;
    }

    (void)f_unlink(s_final);                     /* 允许覆盖同名旧文件 */
    fr = f_rename(s_part, s_final);
    if (fr != FR_OK)
    {
        (void)f_unlink(s_part);
        s_state = NETDL_ERROR;
        (void)snprintf(s_msg, sizeof(s_msg), "rename fail %d", (int)fr);
        return SPID_E_RENAME;
    }

    s_total     = size;      /* 主机可能不知道总大小（网页上传），以实际落盘字节数为准 */
    s_done_evt  = 1u;
    s_done_ok   = 1u;
    s_done_size = size;
    memset(s_done_name, 0, sizeof(s_done_name));
    memcpy(s_done_name, s_name, sizeof(s_done_name) - 1u);
    s_state = NETDL_IDLE;

    (void)snprintf(s_msg, sizeof(s_msg), "done %s (%lu B)", s_name,
                   (unsigned long)size);
    printf("[NETDL] done %s  %lu B\r\n", s_final, (unsigned long)size);
    return SPID_E_NONE;
}

/* ------------------------------------------------------------------ */
void netdl_init(void)
{
    memset(&s_f, 0, sizeof(s_f));
    s_open = 0u; s_fs_ready = 0u; s_state = NETDL_IDLE;
    s_done_evt = 0u; s_done_ok = 0u;
    s_next_seq = 0u; s_ack_type = 0u; s_ack_seq = 0u;
    s_total = 0u; s_written = 0u; s_last_ms = 0u;
    s_frames = 0u; s_errs = 0u; s_bytes = 0u; s_done_size = 0u;
    s_name[0] = 0; s_done_name[0] = 0; s_part[0] = 0; s_final[0] = 0;
    msg_set("idle");
    reply_fill(SPID_ST_OK, SPID_E_NONE);
    printf("[NETDL] ready (SPI slave -> FATFS)\r\n");
}

/* 标记卡/文件系统是否就绪；未就绪时下载请求一律回 NOCARD */
void netdl_set_fs_ready(uint8_t ready)
{
    s_fs_ready = (ready != 0u) ? 1u : 0u;
    if (s_fs_ready != 0u) msg_set("fs ready");
    else                  msg_set("no card / fs not mounted");
}

/* ------------------------------------------------------------------ */
void netdl_poll(void)
{
    const uint8_t *rx;
    const uint8_t *pay;
    uint16_t len   = 0u;
    uint16_t seq   = 0u;
    uint8_t  type  = 0u;
    uint32_t total = 0u;
    uint32_t crc   = 0u;
    uint8_t  err   = SPID_E_NONE;

    /* 空闲超时：卡一直开着半成品文件，超时后清掉 */
    if ((s_open != 0u) && (tick_elapsed(s_last_ms) > NETDL_IDLE_MS))
    {
        dl_drop_partial();
        s_state = NETDL_ERROR;
        s_done_evt = 1u;
        s_done_ok  = 0u;
        s_done_size = 0u;
        msg_set("timeout, partial deleted");
        printf("[NETDL] timeout -> drop partial\r\n");
    }

    if (spid_frame_ready() == 0u) return;

    rx = spid_rx_frame();
    s_frames++;

    if ((rx[SPID_M_SYNC0] != (uint8_t)SPID_MOSI_SYNC0) ||
        (rx[SPID_M_SYNC1] != (uint8_t)SPID_MOSI_SYNC1))
    {
        err = SPID_E_BADFRAME;
        msg_set("bad sync");
    }
    else
    {
        type  = rx[SPID_M_TYPE];
        seq   = get_u16(rx + SPID_M_SEQ);
        len   = get_u16(rx + SPID_M_LEN);
        total = get_u32(rx + SPID_M_TOTAL);
        crc   = get_u32(rx + SPID_M_CRC);
        pay   = rx + SPID_M_PAYLOAD;

        if (len > (uint16_t)SPID_PAYLOAD_MAX)
        {
            err = SPID_E_BADFRAME;
            msg_set("len too big");
        }
        else if ((crc != 0u) && (netdl_crc32(pay, (uint32_t)len) != crc))
        {
            err = SPID_E_BADFRAME;
            msg_set("payload crc mismatch");
        }
        else if ((s_fs_ready == 0u) && (type != (uint8_t)SPID_T_PING))
        {
            err = SPID_E_NOCARD;
            msg_set("no card / fs not mounted");
        }
        else
        {
            switch (type)
            {
            case SPID_T_BEGIN:
                err = dl_begin(pay, len, total);
                break;
            case SPID_T_DATA:
                err = dl_data(seq, pay, len);
                break;
            case SPID_T_END:
                err = dl_end();
                break;
            case SPID_T_ABORT:
                dl_drop_partial();
                s_state = NETDL_IDLE;
                s_total = 0u; s_written = 0u;
                msg_set("aborted");
                printf("[NETDL] abort by master\r\n");
                break;
            case SPID_T_PING:
                break;
            default:
                err = SPID_E_BADFRAME;
                msg_set("unknown type");
                break;
            }
        }
    }

    if (err != SPID_E_NONE)
    {
        s_errs++;
    }
    else
    {
        s_ack_type = (uint16_t)type;
    }

    reply_fill((uint8_t)((err == SPID_E_NONE) ? SPID_ST_OK : SPID_ST_ERROR), err);
    spid_frame_done();
}

/* ------------------------------------------------------------------ */
uint8_t  netdl_state(void)   { return s_state; }
uint8_t  netdl_busy(void)    { return (uint8_t)((s_state == NETDL_OPEN) ? 1u : 0u); }
uint32_t netdl_written(void) { return s_written; }
const char *netdl_name(void) { return s_name; }
const char *netdl_msg(void)  { return s_msg; }

/* 本次下载进度 0..100；总大小未知或不在下载中返回 0 */
uint16_t netdl_percent(void)
{
    if ((s_total == 0u) || (s_state != NETDL_OPEN)) return 0u;
    if (s_written >= s_total) return 100u;
    return (uint16_t)((s_written * 100u) / s_total);
}

/* 取走「下载结束」事件：1 = 成功，2 = 失败/取消，0 = 无事件 */
uint8_t netdl_take_done(const char **name, uint32_t *size)
{
    uint8_t evt = s_done_evt;

    if (evt == 0u) return 0u;
    s_done_evt = 0u;

    if (evt == 1u)
    {
        if (name != 0) *name = s_done_name;
        if (size != 0) *size = s_done_size;
        return (uint8_t)((s_done_ok != 0u) ? 1u : 2u);
    }
    return 2u;
}
