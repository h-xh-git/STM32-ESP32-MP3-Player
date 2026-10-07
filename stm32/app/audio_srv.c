/*
 * audio_srv.c - STM32 侧音频服务：SD 取流 + 组帧 + 信用窗口（机制与字段见 audio_srv.h）
 *
 * 依赖：FatFS（f_read 每次 1 KB）、bsp_proto/bsp_uart2（USART2 成帧收发）、
 *       bsp_tick（计时）、app/playlist（曲目路径）。
 * 调用：app/main.c 主循环里 audio_srv_poll()/audio_srv_on_frame()/audio_srv_notify_state()；
 *       切歌与 seek 用 audio_srv_open()/audio_srv_seek_ms()/audio_srv_set_playing()。
 * 回调：ESP32 发来的控制命令经注册的 s_cmd_cb 交给 main.c 在主循环里执行。
 */
#include "audio_srv.h"
#include "bsp_proto.h"
#include "bsp_uart2.h"
#include "bsp_tick.h"
#include "playlist.h"
#include "ff.h"

#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
static FIL      s_f;
static uint8_t  s_open    = 0u;
static uint8_t  s_playing = 0u;
static uint8_t  s_eof     = 0u;

static uint16_t s_track   = 0u;          /* 0 起 */
static uint32_t s_seq     = 0u;
static uint32_t s_total_ms    = 0u;
static uint32_t s_total_bytes = 0u;
static uint32_t s_consumed    = 0u;      /* 已送出的文件字节 */
static uint16_t s_credit      = 0u;
static uint32_t s_frames      = 0u;
static uint8_t  s_rd_fail     = 0u;      /* f_read 连续失败次数（卡抖动重试） */
static uint32_t s_wait_ms     = 0u;
static uint32_t s_end_sent    = 0u;
static uint32_t s_sync_rx     = 0u;
static uint32_t s_req_rx      = 0u;      /* 收到的 CMD_AUDIO_REQ 帧数(诊断) */
static uint32_t s_req_rx_play = 0u;      /* 其中播放态下被采纳的帧数(诊断)  */
static uint32_t s_sync_ms     = 0u;      /* ESP32 回报的真实播放位置(ms) */
static uint32_t s_sync_t      = 0u;      /* 上次回报时刻(tick)，2.5s 内有效 */
static uint32_t s_sync_local  = 0u;      /* ESP32 本曲已播毫秒(复位后回落) */
static uint8_t  s_sync_wait_drop = 0u;   /* seek/换曲后等 ESP32 复位信号 */
static uint32_t s_sync_drop_t = 0u;

static uint32_t s_time_ms     = 0u;
static uint32_t s_poll_ms     = 0u;

static uint8_t  s_finished = 0u;
static uint16_t s_fin_track = 0u;

static uint8_t  s_pay[AUDIO_CHUNK + 8u];
static uint8_t  s_rd[AUDIO_CHUNK];

static void (*s_cmd_cb)(uint8_t cmd, uint32_t arg) = 0;

/* RESP_STATE 变化检测缓存（初值不可能命中，保证上电先推一次） */
static uint16_t s_c_track = 0xFFFFu;
static uint16_t s_c_total = 0xFFFFu;
static uint8_t  s_c_state = 0xFFu;
static uint8_t  s_c_mode  = 0xFFu;
static uint8_t  s_c_vol   = 0xFFu;
static uint32_t s_state_t = 0u;          /* 上次 RESP_STATE 发送时刻(周期重发) */

/* ------------------------------------------------------------------ */
static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

/* 按小端写 u32（板间协议统一小端） */
static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
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

/* ------------------------------------------------------------------ */
uint32_t audio_srv_pos_ms(void)
{
    unsigned long long v;

    /* ESP32 回报的"真实出声位置"优先（2.5s 内有效），否则退回字节线性估计 */
    if ((s_sync_t != 0u) && (tick_elapsed(s_sync_t) < 2500u))
    {
        v = (unsigned long long)s_sync_ms;
        if (v > (unsigned long long)s_total_ms) v = (unsigned long long)s_total_ms;
        return (uint32_t)v;
    }

    if ((s_total_ms == 0u) || (s_total_bytes == 0u)) return 0u;
    v = ((unsigned long long)s_consumed * (unsigned long long)s_total_ms) /
        (unsigned long long)s_total_bytes;
    if (v > (unsigned long long)s_total_ms) v = (unsigned long long)s_total_ms;
    return (uint32_t)v;
}

/* 注册 ESP32 控制命令回调（由 main.c 提供，命令在主循环里执行） */
void audio_srv_set_cmd_cb(void (*cb)(uint8_t cmd, uint32_t arg))
{
    s_cmd_cb = cb;
}

/* ------------------------------------------------------------------ */
static void send_state(void)
{
    uint8_t p[7u];

    put_u16(p, s_c_track);
    put_u16(p + 2u, s_c_total);
    p[4] = s_c_state;
    p[5] = s_c_mode;
    p[6] = s_c_vol;
    proto_send(RESP_STATE, p, 7u);
}

void audio_srv_notify_state(uint16_t track0, uint16_t total,
                            uint8_t playing, uint8_t mode, uint8_t vol)
{
    /* 内容未变且距上次发送不足 2s 就不重发；超过 2s 强制重发一次，
       这样后上电/掉线重连的 ESP32 也能拿到当前播放状态（自愈） */
    if ((track0  == s_c_track) && (total == s_c_total) &&
        (playing == s_c_state) && (mode  == s_c_mode)  &&
        (vol     == s_c_vol))
    {
        if ((s_state_t != 0u) && (tick_elapsed(s_state_t) < 2000u)) return;
    }

    s_c_track = track0;
    s_c_total = total;
    s_c_state = playing;
    s_c_mode  = mode;
    s_c_vol   = vol;
    s_state_t = tick_ms();
    send_state();
}

/* 上报 RESP_TIME：当前播放位置与总时长 */
static void send_time(void)
{
    uint8_t p[8u];

    put_u32(p, audio_srv_pos_ms());
    put_u32(p + 4u, s_total_ms);
    proto_send(RESP_TIME, p, 8u);
}

/* 回 RESP_PONG：带回请求里的 echo 与累计丢弃字节数 */
static void send_pong(uint32_t echo)
{
    uint8_t p[6u];

    put_u32(p, echo);
    put_u16(p + 4u, (uint16_t)(proto_drops() & 0xFFFFu));
    proto_send(RESP_PONG, p, 6u);
}

/* 歌单分页：每次最多 8 首，idx=0xFFFF 表示结束 */
static void send_list(uint16_t from, uint16_t count)
{
    uint8_t  p[4u + PL_NAME_LEN];
    uint16_t n = pl_count();
    uint16_t i;

    if (count > 8u) count = 8u;

    for (i = 0u; i < count; i++)
    {
        uint16_t idx = (uint16_t)(from + i);
        const pl_entry_t *e;
        uint16_t nl;

        if (idx >= n)
        {
            put_u16(p, 0xFFFFu);
            put_u16(p + 2u, n);
            proto_send(RESP_LIST, p, 4u);
            return;
        }

        e = pl_get(idx);
        if (e == 0) break;

        put_u16(p, idx);
        put_u16(p + 2u, n);
        nl = (uint16_t)strlen(e->name);
        if (nl > (uint16_t)PL_NAME_LEN) nl = (uint16_t)PL_NAME_LEN;
        memcpy(p + 4u, e->name, (size_t)nl);
        proto_send(RESP_LIST, p, (uint16_t)(4u + nl));
    }
}

/* ------------------------------------------------------------------ */
static void audio_srv_end(uint8_t reason)
{
    uint8_t p[4u];

    if (s_end_sent != 0u) return;
    s_end_sent = 1u;
    s_eof      = 1u;
    s_playing  = 0u;
    s_finished = 1u;
    s_fin_track = s_track;

    put_u16(p, s_track);
    p[2] = reason;
    p[3] = 0u;
    proto_send(RESP_END, p, 4u);

    printf("[AUD] end track %u reason %u frames=%lu bytes=%lu\r\n",
           (unsigned)(s_track + 1u), (unsigned)reason,
           (unsigned long)s_frames, (unsigned long)s_consumed);
}

/* seek 后向前找帧同步头，把文件指针对齐到帧边界 */
static void audio_srv_align(void)
{
    UINT br = 0u;
    uint16_t i;

    if (s_consumed >= s_total_bytes) return;
    if (f_read(&s_f, s_rd, AUDIO_SYNC_WIN, &br) != FR_OK) return;

    for (i = 0u; (uint16_t)(i + 1u) < (uint16_t)br; i++)
    {
        if ((s_rd[i] == 0xFFu) && ((s_rd[i + 1u] & 0xE0u) == 0xE0u))
        {
            s_consumed += i;
            (void)f_lseek(&s_f, (FSIZE_t)s_consumed);
            return;
        }
    }
    /* 窗口内没找到：退回原位，让 ESP32 的解码器自行重同步 */
    (void)f_lseek(&s_f, (FSIZE_t)s_consumed);
}

/* 按毫秒定位：折算成字节偏移后向前找 MP3 帧同步头，并清额度、让 seq 跳变 */
void audio_srv_seek_ms(uint32_t ms)
{
    unsigned long long off;

    if ((s_open == 0u) || (s_total_ms == 0u)) return;
    if (ms > s_total_ms) ms = s_total_ms;

    off = ((unsigned long long)ms * (unsigned long long)s_total_bytes) /
          (unsigned long long)s_total_ms;
    if (off > (unsigned long long)s_total_bytes) off = (unsigned long long)s_total_bytes;

    s_consumed = (uint32_t)off;
    if (f_lseek(&s_f, (FSIZE_t)s_consumed) != FR_OK) return;

    audio_srv_align();

    s_seq += 2u;                 /* 跳过 1 个数：ESP32 用"期望值不连续"识别 seek */
    s_credit = 0u;               /* 旧额度作废，等 ESP32 重新请求  */
    s_eof = 0u;
    s_end_sent = 0u;
    s_sync_t = 0u;               /* 旧的 ESP32 位置作废，否则 RESP_TIME 会报旧位置 */
    s_sync_wait_drop = 1u;       /* 等 ESP32 复位后再采纳它回报的位置 */
    s_sync_drop_t = tick_ms();
    send_time();

    printf("[AUD] seek %lu ms -> byte %lu\r\n",
           (unsigned long)ms, (unsigned long)s_consumed);
}

/* ------------------------------------------------------------------ */
/* 跳过文件开头的 ID3v2 标签：只把真正的 MP3 音频数据送给 ESP32。
   否则标签里的封面(APIC/JPEG)会被当成音频喂进解码器，开曲头会出爆音/噪声。 */
static uint32_t id3v2_skip(FIL *fp)
{
    uint8_t h[10];
    UINT    br = 0u;
    uint32_t len;

    if (fp == 0) return 0u;
    if (f_read(fp, h, 10u, &br) != FR_OK || br != 10u) { (void)f_lseek(fp, 0); return 0u; }
    if ((h[0] != 'I') || (h[1] != 'D') || (h[2] != '3')) { (void)f_lseek(fp, 0); return 0u; }

    len = 10u + ((((uint32_t)h[6] & 0x7Fu) << 21) |
                 (((uint32_t)h[7] & 0x7Fu) << 14) |
                 (((uint32_t)h[8] & 0x7Fu) <<  7) |
                  ((uint32_t)h[9] & 0x7Fu));
    if ((h[5] & 0x10u) != 0u) len += 10u;                 /* footer present */
    if ((s_total_bytes != 0u) && (len >= s_total_bytes)) {    /* 异常保护 */
        (void)f_lseek(fp, 0);
        return 0u;
    }
    if (f_lseek(fp, (FSIZE_t)len) != FR_OK) { (void)f_lseek(fp, 0); return 0u; }
    return len;
}

/* 打开第 idx0 首并复位取流状态；成功 1，失败 0 */
uint8_t audio_srv_open(uint16_t idx0, uint32_t dur_ms)
{
    char  path[96];
    const pl_entry_t *e = pl_get(idx0);

    audio_srv_close();

    s_track       = idx0;
    s_total_ms    = dur_ms;
    s_total_bytes = (e != 0) ? e->size : 0u;
    s_seq         = 0u;
    s_consumed    = 0u;
    s_credit      = 0u;
    s_eof         = 0u;
    s_end_sent    = 0u;
    s_frames      = 0u;
    s_wait_ms     = 0u;
    s_time_ms     = tick_ms();
    s_poll_ms     = tick_ms();
    s_sync_t      = 0u;
    s_sync_local  = 0u;
    s_sync_wait_drop = 1u;
    s_sync_drop_t = tick_ms();

    if (e == 0) return 0u;

    pl_path(idx0, path, (uint32_t)sizeof(path));
    if (f_open(&s_f, path, FA_READ) != FR_OK)
    {
        printf("[AUD] open FAIL: %s\r\n", path);
        return 0u;
    }

    s_open = 1u;
    s_consumed = id3v2_skip(&s_f);        /* 跳过 ID3v2，只送音频数据 */
    printf("[AUD] open track %u  %lu B  %lu ms  id3=%lu\r\n",
           (unsigned)(idx0 + 1u), (unsigned long)s_total_bytes,
           (unsigned long)s_total_ms, (unsigned long)s_consumed);
    return 1u;
}

/* 关闭当前文件、停止发送并清空额度 */
void audio_srv_close(void)
{
    if (s_open != 0u)
    {
        (void)f_close(&s_f);
        s_open = 0u;
    }
    s_playing = 0u;
    s_eof     = 0u;
}

/* 播放/暂停开关 */
void audio_srv_set_playing(uint8_t on)
{
    /* 暂停即清空额度：ESP32 暂停期间不再申请，避免恢复时一次性爆发 */
    if ((on == 0u) && (s_playing != 0u)) s_credit = 0u;
    s_playing = (on != 0u) ? 1u : 0u;
    s_poll_ms = tick_ms();
}

/* 初始化音频服务状态（上电调用一次） */
void audio_srv_init(void)
{
    memset(&s_f, 0, sizeof(s_f));
    s_open = 0u; s_playing = 0u; s_eof = 0u;
    s_track = 0u; s_seq = 0u; s_total_ms = 0u; s_total_bytes = 0u;
    s_consumed = 0u; s_credit = 0u; s_frames = 0u; s_wait_ms = 0u;
    s_end_sent = 0u; s_finished = 0u; s_fin_track = 0u; s_sync_rx = 0u;
    s_rd_fail = 0u;
    s_sync_ms = 0u; s_sync_t = 0u; s_sync_local = 0u;
    s_sync_wait_drop = 0u; s_sync_drop_t = 0u; s_state_t = 0u;
    s_c_track = 0xFFFFu; s_c_total = 0xFFFFu;
    s_c_state = 0xFFu; s_c_mode = 0xFFu; s_c_vol = 0xFFu;
}

/* ------------------------------------------------------------------ */
void audio_srv_poll(void)
{
    uint32_t now = tick_ms();
    uint32_t dt  = tick_elapsed(s_poll_ms);
    FRESULT  fr;
    UINT     br = 0u;

    s_poll_ms = now;
    if (dt > 1000u) dt = 1000u;         /* 异常长的间隔不计入统计 */

    /* 播放中定期推时间，供 ESP32 侧显示 */
    if ((s_open != 0u) && (s_playing != 0u) &&
        ((uint32_t)(now - s_time_ms) >= AUDIO_TIME_MS))
    {
        s_time_ms = now;
        send_time();
    }

    if ((s_open == 0u) || (s_playing == 0u) || (s_eof != 0u)) return;

    /* 没有额度（ESP32 还没要）或串口环装不下 -> 本毫秒先不发 */
    if (s_credit == 0u)
    {
        s_wait_ms += dt;
        return;
    }
    if (uart2_tx_free() < (uint16_t)(AUDIO_CHUNK + 16u))
    {
        s_wait_ms += dt;
        return;
    }

    fr = f_read(&s_f, s_rd, AUDIO_CHUNK, &br);
    if (fr != FR_OK)
    {
        /* SD 偶发读错不要让整首歌结束：把文件指针拨回已送出的位置重试 */
        if (s_rd_fail < 3u)
        {
            s_rd_fail++;
            printf("[AUD] f_read err %d -> retry %u\r\n", (int)fr, (unsigned)s_rd_fail);
            (void)f_lseek(&s_f, s_consumed);
            return;
        }
        printf("[AUD] f_read err %d (give up)\r\n", (int)fr);
        s_rd_fail = 0u;
        audio_srv_end(2u);
        return;
    }
    s_rd_fail = 0u;
    if (br == 0u)
    {
        audio_srv_end(0u);
        return;
    }

    put_u32(s_pay, s_seq);
    put_u16(s_pay + 4u, (uint16_t)br);
    memcpy(s_pay + 6u, s_rd, (size_t)br);
    proto_send(RESP_AUDIO, s_pay, (uint16_t)(6u + br));

    s_seq++;
    s_consumed += br;
    s_frames++;
    s_credit--;

    if ((uint32_t)br < AUDIO_CHUNK) audio_srv_end(0u);   /* 到文件尾 */
}

/* ------------------------------------------------------------------ */
void audio_srv_on_frame(uint8_t type, const uint8_t *pl, uint16_t len)
{
    switch (type)
    {
    case CMD_AUDIO_REQ:                       /* u16 blocks -> 加额度 */
        s_req_rx++;
        if ((len >= 2u) && (s_playing != 0u))
        {
            uint16_t want = get_u16(pl);
            s_req_rx_play++;
            uint32_t c = (uint32_t)s_credit + (uint32_t)want;
            if (c > (uint32_t)AUDIO_CREDIT_MAX) c = (uint32_t)AUDIO_CREDIT_MAX;
            s_credit = (uint16_t)c;
        }
        break;

    case CMD_SEEK:
        if (len >= 4u) audio_srv_seek_ms(get_u32(pl));
        break;

    case CMD_PING:
        send_pong((len >= 4u) ? get_u32(pl) : 0u);
        break;

    case CMD_LIST_REQ:
        send_list((len >= 4u) ? get_u16(pl) : 0u, (len >= 4u) ? get_u16(pl + 2u) : 8u);
        break;

    case CMD_SYNC_POS:                        /* ESP32 回报真实出声位置 */
        if (len >= 10u)
        {
            uint16_t tr  = get_u16(pl);
            uint32_t om  = get_u32(pl + 2u);
            uint32_t loc = get_u32(pl + 6u);   /* ESP32 本曲已播毫秒 */
            uint8_t  ok  = 0u;

            if (tr == s_track)
            {
                if (s_sync_wait_drop != 0u)
                {
                    /* 等 ESP32 复位（它的本曲计数会回落）：回落即认可 */
                    if (loc <= s_sync_local) { s_sync_wait_drop = 0u; ok = 1u; }
                    else if (tick_elapsed(s_sync_drop_t) > 5000u)
                    { s_sync_wait_drop = 0u; ok = 1u; }   /* 超时兜底 */
                }
                else ok = 1u;
            }

            if (ok != 0u)
            {
                if (om > s_total_ms) om = s_total_ms;
                s_sync_ms    = om;
                s_sync_local = loc;
                s_sync_t     = tick_ms();
                s_sync_rx++;
            }
        }
        break;

    default:                                  /* 传输控制命令交给 main */
        if (s_cmd_cb != 0)
        {
            uint32_t arg = 0u;
            if (len >= 4u)      arg = get_u32(pl);
            else if (len >= 2u) arg = (uint32_t)get_u16(pl);
            else if (len >= 1u) arg = (uint32_t)pl[0];
            s_cmd_cb(type, arg);
        }
        break;
    }
}

/* 取走「一首播完」事件（每个事件只返回一次）；有事件 1，否则 0 */
uint8_t audio_srv_take_finished(uint16_t *idx0)
{
    if (s_finished == 0u) return 0u;
    s_finished = 0u;
    if (idx0 != 0) *idx0 = s_fin_track;
    return 1u;
}
