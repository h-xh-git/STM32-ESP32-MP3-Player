/*
 * play_resume.c - 断点续播：把 当前曲目 + 该曲已播毫秒 + 文件名 + 文件大小
 *                 写成一条快照，存到 F407 片内 Flash（Sector 7）。
 *
 * 实际搬运交给 bsp/storage/flash_slot.c：整个 Sector 7 是一个只追加的
 * 96 B 环形条目区，一次检查点追加一条，写满 1365 条（约 22.7 分钟）才
 * 整区擦一次。条目头里有 magic/seq/len/pos_ms/CRC，payload 是下面这 76 B。
 *
 * 记录体布局（PR_REC_BYTES = 76 B，是 4 的倍数，正好是 flash_slot 的
 * payload 上限 FLS_DATA_MAX）:
 *   off  0 : u32 track      曲目序号（0 起，playlist 下标）
 *   off  4 : u32 file size  SD 卡上该文件的字节数，用于核对
 *   off  8 : u16 name_len   含结尾 0，<= 64
 *   off 10 : u16 reserved   0
 *   off 12 : char name[64]  文件名（GBK 字节）
 *
 * ★这里刻意**不写位置**：位置（已播毫秒）放在 flash_slot 条目头里，
 *   随每条检查点一起追加。payload 只在换歌时才变，因此
 *   flash_slot_save() 的 内容相同就不写 判断在播放期间总成立，
 *   只会有位置在走 —— 这正是把 快变量 单独放条目头的原因。
 *
 * 为什么存已播毫秒而不是文件偏移字节：
 *   audio_srv_pos_ms() 在 ESP32 每 500 ms 回报真实出声位置时直接用那个值
 *   （2.5 s 内有效），否则才退回已送出字节按总时长线性折算。ESP32 侧有
 *   64 KB 压缩环 + 128 KB PCM 环，STM32 预读位置比出声位置超前约 2~3 s，
 *   存预读字节会让每次续播都往前跳一段。续播时再用
 *   audio_srv_seek_ms(pos_ms) 折算回字节偏移并把文件指针对齐到 MP3 帧头，
 *   等价于按文件偏移续播，VBR 文件也不跑偏。
 *
 * 掉电最坏情况：位置退一条检查点（默认 1 s 音频）。整区擦除那一次
 * （约 22.7 分钟一次）若在 擦完/写回前 掉电，会退到歌首。
 * 曲目号与文件名不会错位（它们和位置在同一条快照里，不存在跨区不一致）。
 */
#include "play_resume.h"
#include "flash_slot.h"
#include "audio_srv.h"
#include "playlist.h"
#include "bsp_tick.h"

#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
#define PR_REC_BYTES     76u
#define PR_TRACK_OFF     0u
#define PR_SIZE_OFF      4u
#define PR_NLEN_OFF      8u
#define PR_RSV_OFF       10u
#define PR_NAME_OFF      12u
#define PR_MAGIC_TRACK   0xFFFFFFFFu

/* 周期检查点间隔 */
#define PR_SAVE_MS       1000u
/* 位置至少要前进这么多才值得再写一条（避免暂停/卡顿时反复写同一位置） */
#define PR_MIN_DELTA_MS  1000u
/* 空闲时两次 prime 之间的最小间隔 */
#define PR_PRIME_GAP_MS  3000u

static play_resume_snapshot_t s_snap;
static uint8_t  s_inited    = 0u;
static uint8_t  s_track_ok  = 0u;       /* 当前是否有一首已打开/正在播的曲目 */
static uint16_t s_cur_track = 0u;
static uint32_t s_next_ms   = 0u;       /* 下一次检查点的绝对时刻(tick_ms 时间基) */
static uint32_t s_min_ms    = 0u;       /* 下一次检查点要求的最小位置(ms) */
static uint32_t s_writes    = 0u;
static uint32_t s_skips     = 0u;
static uint32_t s_fails     = 0u;

/* ------------------------------------------------------------------ */
static void pr_put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

/* 写小端 u32 */
static void pr_put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/* 读小端 u16 */
static uint16_t pr_get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* 读小端 u32 */
static uint32_t pr_get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ------------------------------------------------------------------ */
uint8_t play_resume_init(void)
{
    uint8_t  rec[PR_REC_BYTES];
    uint16_t len   = 0u;
    uint32_t pos   = 0u;
    uint32_t track;
    uint16_t nl;
    uint8_t  have;

    memset(&s_snap, 0, sizeof(s_snap));
    s_snap.src = 0xFFu;
    s_inited   = 1u;

    have   = flash_slot_init();                  /* 扫扇区，定写指针 */
    s_snap.slots = (uint8_t)((flash_slot_entries() > 255u)
                             ? 255u : flash_slot_entries());

    if (flash_slot_load(rec, (uint16_t)sizeof(rec), &len, &pos) == 0u)
    {
        printf("[RSM] init: no saved snapshot (have=%u entries=%lu)\r\n",
               (unsigned)have, (unsigned long)flash_slot_entries());
        return 0u;
    }
    if (len < PR_REC_BYTES)
    {
        printf("[RSM] init: record too short (%u B) -> ignored\r\n",
               (unsigned)len);
        return 0u;
    }

    track = pr_get_u32(rec + PR_TRACK_OFF);
    if (track == PR_MAGIC_TRACK)
    {
        printf("[RSM] init: tombstone (no track was playing)\r\n");
        return 0u;
    }

    nl = pr_get_u16(rec + PR_NLEN_OFF);
    if ((nl == 0u) || (nl > PR_NAME_LEN)) nl = PR_NAME_LEN;
    memcpy(s_snap.name, rec + PR_NAME_OFF, (size_t)(nl - 1u));
    s_snap.name[nl - 1u] = 0;

    s_snap.track  = track;
    s_snap.pos_ms = pos;                 /* 位置和 payload 来自同一条快照 */
    s_snap.valid  = 1u;

    printf("[RSM] init: saved track=%lu pos=%lu ms name=%s\r\n",
           (unsigned long)s_snap.track, (unsigned long)s_snap.pos_ms,
           s_snap.name);
    return 1u;
}

/* 取最近一次快照（只读），供 main.c 显示/核对 */
const play_resume_snapshot_t *play_resume_get(void)
{
    return &s_snap;
}

/* ------------------------------------------------------------------ */
/* 组一条记录到 buf（固定 PR_REC_BYTES 字节）；没有可存的内容则返回 0。
   这里不能写入会随时间变化的量（位置在 flash_slot 条目头里传递）。 */
static uint16_t pr_build(uint8_t *buf)
{
    const pl_entry_t *e;
    uint16_t nl;

    if (s_track_ok == 0u) return 0u;

    e = pl_get(s_cur_track);
    if (e == 0) return 0u;

    memset(buf, 0, PR_REC_BYTES);
    pr_put_u32(buf + PR_TRACK_OFF, (uint32_t)s_cur_track);
    pr_put_u32(buf + PR_SIZE_OFF, e->size);
    pr_put_u16(buf + PR_RSV_OFF, 0u);

    nl = (uint16_t)(strlen(e->name) + 1u);
    if (nl > (uint16_t)PR_NAME_LEN) nl = (uint16_t)PR_NAME_LEN;
    pr_put_u16(buf + PR_NLEN_OFF, nl);
    memcpy(buf + PR_NAME_OFF, e->name, (size_t)(nl - 1u));

    return PR_REC_BYTES;
}

/* 用记录里的文件名 + 大小核对 SD 卡歌单：匹配则返回 1 并给出曲目下标 */
static uint8_t pr_match_track(const uint8_t *rec, uint16_t *out_idx)
{
    uint16_t total = pl_count();
    uint16_t want_len;
    uint16_t i;
    uint32_t want_size;
    const char *want_name = (const char *)(const void *)(rec + PR_NAME_OFF);

    if (total == 0u) return 0u;

    want_len = pr_get_u16(rec + PR_NLEN_OFF);
    if ((want_len == 0u) || (want_len > (uint16_t)PR_NAME_LEN)) return 0u;
    want_size = pr_get_u32(rec + PR_SIZE_OFF);

    for (i = 0u; i < total; i++)
    {
        const pl_entry_t *e = pl_get(i);
        if (e == 0) continue;
        if ((strlen(e->name) + 1u) != (size_t)want_len) continue;
        if (strncmp(e->name, want_name, (size_t)(want_len - 1u)) != 0) continue;
        if ((want_size != 0u) && (e->size != want_size)) continue;

        if (out_idx != 0) *out_idx = i;
        return 1u;
    }
    return 0u;
}

/* 按文件名在当前歌单里核对上次记录的那一首；命中 1 并回填序号与位置，否则 0 */
uint8_t play_resume_find_saved(uint16_t *track0, uint32_t *pos_ms)
{
    uint8_t  rec[PR_REC_BYTES];
    uint16_t len = 0u;
    uint16_t idx = 0u;
    uint32_t pos = 0u;

    if (s_snap.valid == 0u) return 0u;
    if (flash_slot_load(rec, (uint16_t)sizeof(rec), &len, &pos) == 0u) return 0u;
    if (len < PR_REC_BYTES) return 0u;
    if (pr_match_track(rec, &idx) == 0u) return 0u;

    s_snap.track  = (uint32_t)idx;
    s_snap.pos_ms = pos;
    if (track0 != 0) *track0 = idx;
    if (pos_ms != 0) *pos_ms = pos;
    return 1u;
}

/* 换歌登记：把新曲目写进快照模板，并推迟下一个检查点 */
void play_resume_set_track(uint16_t idx0)
{
    s_track_ok    = 1u;
    s_cur_track   = idx0;
    s_snap.track  = (uint32_t)idx0;
    s_snap.pos_ms = 0u;
    s_next_ms     = tick_ms() + PR_SAVE_MS;
    s_min_ms      = PR_MIN_DELTA_MS;
}

/* 歌单为空/无法定位曲目：停止写检查点（避免写入无意义的位置） */
void play_resume_open_track(void)
{
    s_track_ok    = 0u;
    s_cur_track   = 0u;
    s_snap.pos_ms = 0u;
    s_next_ms     = 0u;
    s_min_ms      = 0u;
}

/* ------------------------------------------------------------------ */
uint8_t play_resume_save(uint32_t pos_ms, uint8_t force)
{
    uint8_t  rec[PR_REC_BYTES];
    uint16_t len;
    uint8_t  r;
    uint32_t t0, dt;

    (void)force;

    if (s_inited == 0u) return PR_SKIP;
    if (s_track_ok == 0u) { s_skips++; return PR_SKIP; }

    s_snap.pos_ms = pos_ms;
    len = pr_build(rec);
    if (len == 0u) { s_skips++; return PR_SKIP; }

    t0 = tick_ms();
    r  = flash_slot_save(rec, len, pos_ms);
    dt = tick_elapsed(t0);

    if (r == FLS_UNCHANGED)
    {
        s_skips++;
        return PR_UNCHANGED;
    }
    if (r != FLS_OK)
    {
        s_fails++;
        printf("[RSM] !! save FAILED (writes=%lu erases=%lu)\r\n",
               (unsigned long)s_writes, (unsigned long)flash_slot_erases());
        return PR_ERR;
    }

    s_writes++;
    s_snap.valid = 1u;
    s_snap.track = (uint32_t)s_cur_track;
    if (dt >= 50u)
    {
        printf("[RSM] save #%lu track=%u pos=%lu ms  (%lu ms, erases=%lu)\r\n",
               (unsigned long)s_writes, (unsigned)s_cur_track,
               (unsigned long)s_snap.pos_ms, (unsigned long)dt,
               (unsigned long)flash_slot_erases());
    }
    return PR_OK;
}

/* 主循环调用：到检查点就写一条快照，返回下一个检查点时刻 */
uint32_t play_resume_tick(uint32_t now_ms)
{
    uint32_t pos;

    if (s_inited == 0u) return now_ms + PR_SAVE_MS;
    if (s_track_ok == 0u) { s_next_ms = now_ms + PR_SAVE_MS; return s_next_ms; }

    if ((int32_t)(now_ms - s_next_ms) < 0) return s_next_ms;   /* 还没到点 */

    pos = audio_srv_pos_ms();                     /* 真实出声位置 */
    if (pos < s_min_ms)
    {
        /* 位置还没往前走够：稍后再看，别写重复内容 */
        s_next_ms = now_ms + (PR_SAVE_MS / 2u);
        return s_next_ms;
    }

    s_next_ms = now_ms + PR_SAVE_MS;
    s_min_ms  = pos + PR_MIN_DELTA_MS;
    (void)play_resume_save(pos, 0u);
    return s_next_ms;
}

/* 空闲时提前滚动条目区：写满就擦扇区，避免播放中被整区擦除阻塞 */
void play_resume_prime(uint32_t now_ms)
{
    static uint32_t last_try = 0u;
    static uint8_t  started  = 0u;

    if (s_inited == 0u) return;
    if (flash_slot_have_uncommitted() == 0u) return;   /* 还没写到末尾 */

    if ((started != 0u) && (tick_elapsed(last_try) < PR_PRIME_GAP_MS)) return;
    started  = 1u;
    last_try = now_ms;

    (void)flash_slot_prime();
}

/* 已无可用曲目（歌单空 / 卡坏）：写一条墓碑快照，下次上电不会去恢复 */
void play_resume_clear(void)
{
    uint8_t rec[PR_REC_BYTES];

    if (s_inited == 0u) return;

    s_track_ok  = 0u;
    s_cur_track = 0u;
    memset(rec, 0, sizeof(rec));
    pr_put_u32(rec + PR_TRACK_OFF, PR_MAGIC_TRACK);
    (void)flash_slot_save(rec, PR_REC_BYTES, 0u);
}

