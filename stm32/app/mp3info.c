/*
 * mp3info.c - 解析 MP3 时长与流参数（比特率 / 采样率 / 是否 VBR）
 *
 * 步骤：跳过 ID3v2 标签 -> 找第一帧同步字 -> 解析 MPEG 版本/层/比特率/采样率
 *       -> 有 Xing/Info/VBRI 帧数就按帧数算（VBR 精确），否则按首帧比特率做 CBR 估算。
 * 只读文件头几 KB。解析出的参数同时缓存，供串口打印曲目信息（mp3_last_*）。
 * 依赖：FatFS（f_open/f_lseek/f_read）。
 * 调用：app/main.c（切歌时算总时长 -> 进度条与断点续播换算）。
 */
#include "mp3info.h"
#include "ff.h"
#include <string.h>

/* MPEG1 Layer3 比特率表 (kbps), 索引 0 与 15 非法 */
static const uint16_t s_br_v1[16] =
    { 0u, 32u, 40u, 48u, 56u, 64u, 80u, 96u, 112u, 128u, 160u, 192u, 224u, 256u, 320u, 0u };
/* MPEG2 / MPEG2.5 Layer3 比特率表 (kbps) */
static const uint16_t s_br_v2[16] =
    { 0u, 8u, 16u, 24u, 32u, 40u, 48u, 56u, 64u, 80u, 96u, 112u, 128u, 144u, 160u, 0u };
/* 采样率表, 第一维 = 版本位(0=2.5, 1=保留, 2=MPEG2, 3=MPEG1) */
static const uint32_t s_sr[4][4] =
{
    { 11025u, 12000u,  8000u, 0u },
    {     0u,     0u,     0u, 0u },
    { 22050u, 24000u, 16000u, 0u },
    { 44100u, 48000u, 32000u, 0u }
};

static uint32_t s_br = 0u;
static uint32_t s_sr_hz = 0u;
static uint32_t s_vbr = 0u;

/* 读大端 u32（MP3/Xing 头部都是大端） */
static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

/* FIL 与读缓冲做成静态: 单次调用只发生在换曲时, 避免占用约 1.6 KB 栈 */
static FIL     s_f;
static uint8_t s_win[512];

/* 找 Xing/Info 帧数并折算成毫秒（VBR 精确）；没有则返回 0 */
static uint32_t xing_frames(const uint8_t *w, uint32_t avail, uint32_t side_len, uint32_t samples)
{
    uint32_t frames = 0u;

    if (((side_len + 12u) <= avail) &&
        ((memcmp(w + side_len, "Xing", 4u) == 0) || (memcmp(w + side_len, "Info", 4u) == 0)))
    {
        if (be32(w + side_len + 4u) & 1u) frames = be32(w + side_len + 8u);
    }
    else if ((50u <= avail) && (memcmp(w + 32u, "VBRI", 4u) == 0))
    {
        frames = be32(w + 32u + 14u);
    }

    if ((frames == 0u) || (frames >= 2000000u)) return 0u;   /* 防溢出 / 明显异常 */
    s_vbr = 1u;
    return (uint32_t)((frames * samples) / s_sr_hz) * 1000u;
}

/* 解析 MP3 总时长(ms)；打不开或不是 MP3 返回 0 */
uint32_t mp3_duration_ms(const char *path)
{
    uint8_t  hdr[10];
    UINT     br = 0u;
    uint32_t fsz, audio, base, found = 0u;
    uint32_t i, ver, layer, br_idx, sr_idx, mode, brate, srate, samples, side_len;

    s_br = 0u; s_sr_hz = 0u; s_vbr = 0u;

    if ((path == (const char *)0) || (f_open(&s_f, path, FA_READ) != FR_OK)) return 0u;
    fsz = (uint32_t)f_size(&s_f);
    if (fsz < 128u) { f_close(&s_f); return 0u; }

    /* ---- ID3v2 标签长度(每字节 7 bit) ---- */
    audio = 0u;
    if ((f_read(&s_f, hdr, 10u, &br) == FR_OK) && (br == 10u) &&
        (hdr[0] == 'I') && (hdr[1] == 'D') && (hdr[2] == '3'))
    {
        audio = 10u + ((((uint32_t)hdr[6] & 0x7Fu) << 21) |
                       (((uint32_t)hdr[7] & 0x7Fu) << 14) |
                       (((uint32_t)hdr[8] & 0x7Fu) << 7)  |
                        ((uint32_t)hdr[9] & 0x7Fu));
        if (hdr[5] & 0x10u) audio += 10u;          /* footer */
    }

    /* ---- 扫描帧同步字, 最多向后 64 KB ---- */
    base = audio;
    while ((found == 0u) && ((base - audio) < 65536u) && ((base + 4u) < fsz))
    {
        if ((f_lseek(&s_f, base) != FR_OK) ||
            (f_read(&s_f, s_win, sizeof(s_win), &br) != FR_OK) || (br < 4u)) break;

        for (i = 0u; (i + 3u) < br; i++)
        {
            if ((s_win[i] == 0xFFu) && ((s_win[i + 1u] & 0xE0u) == 0xE0u))
            {
                uint32_t b1 = s_win[i + 1u];
                uint32_t b2 = s_win[i + 2u];
                uint32_t v  = (b1 >> 3) & 3u;
                uint32_t L  = (b1 >> 1) & 3u;
                if ((v != 1u) && (L == 1u) && (((b2 >> 4) & 0xFu) != 0u) && (((b2 >> 2) & 3u) != 3u))
                {
                    found = base + i;
                    break;
                }
            }
        }
        if (found == 0u) base += (br >= 3u) ? (br - 3u) : br;   /* 重叠 3 字节, 防跨块漏检 */
    }
    if (found == 0u) { f_close(&s_f); return 0u; }

    /* ---- 解析帧头 ---- */
    if ((f_lseek(&s_f, found) != FR_OK) ||
        (f_read(&s_f, hdr, 4u, &br) != FR_OK) || (br != 4u)) { f_close(&s_f); return 0u; }

    ver    = (hdr[1] >> 3) & 3u;
    layer  = (hdr[1] >> 1) & 3u;
    br_idx = (hdr[2] >> 4) & 0xFu;
    sr_idx = (hdr[2] >> 2) & 3u;
    mode   = (hdr[3] >> 6) & 3u;          /* 3 = 单声道 */

    if ((ver == 1u) || (layer != 1u)) { f_close(&s_f); return 0u; }

    brate = (ver == 3u) ? s_br_v1[br_idx] : s_br_v2[br_idx];
    srate = s_sr[ver][sr_idx];
    if ((brate == 0u) || (srate == 0u)) { f_close(&s_f); return 0u; }

    samples  = (ver == 3u) ? 1152u : 576u;
    side_len = (ver == 3u) ? ((mode == 3u) ? 17u : 32u) : ((mode == 3u) ? 9u : 17u);
    s_br     = brate;
    s_sr_hz  = srate;

    /* ---- Xing / Info / VBRI 帧数 ---- */
    if ((f_lseek(&s_f, found + 4u) == FR_OK) &&
        (f_read(&s_f, s_win, 64u, &br) == FR_OK) && (br >= 50u))
    {
        uint32_t d = xing_frames(s_win, br, side_len, samples);
        if (d != 0u) { f_close(&s_f); return d; }
    }

    f_close(&s_f);

    /* ---- CBR 估算: 剩余字节 / 每秒字节数 ---- */
    {
        uint32_t bytes = fsz - found;
        uint32_t bps   = (uint32_t)brate * 125u;       /* kbps -> 字节/秒 */
        uint32_t sec   = bytes / bps;
        uint32_t rem   = bytes % bps;
        return sec * 1000u + (rem * 1000u) / bps;
    }
}

uint32_t mp3_last_bitrate(void)    { return s_br; }
uint32_t mp3_last_samplerate(void) { return s_sr_hz; }
uint32_t mp3_last_vbr(void)        { return s_vbr; }
