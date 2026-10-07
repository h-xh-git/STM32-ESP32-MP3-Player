/*
 * lyrics.c - .lrc 歌词载入与索引：编码识别（UTF-8 / GBK）+ [mm:ss.xx] 时间标签解析
 *
 * 输入：与 mp3 同目录同名的 .lrc（路径由调用方给出，见 lyrics.h 的约定说明）。
 * 输出：静态数组里的 时间 -> 文本 表，界面按当前播放位置取行显示。
 * 依赖：FatFS（读文件）、app/gbk2312_map.h（内置 GB2312 码表，约 16 KB Flash）。
 * 调用：app/main.c（切歌时 lrc_clear()/lrc_load()），bsp/ui/ui_player.c
 *       （每帧用 lrc_index()/lrc_line()/lrc_count() 取要显示的行）。
 * 内存：LRC_MAX_LINES * LRC_LINE_LEN 静态数组（128 * 96 = 12 KB），不用 malloc。
 */
#include "lyrics.h"
#include "gbk2312_map.h"   /* GB2312 码表(约 16 KB Flash), 替代 ff_oem2uni(936) */
#include "ff.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define LRC_READ_CHUNK  256
#define LRC_MAX_TAG     8               /* 一行开头最多连着几个 [time] */
#define RAW_LINE_LEN    96              /* 原始字节行(GBK 下 96 B 约 48 字) */

static uint32_t s_time[LRC_MAX_LINES];
static char     s_text[LRC_MAX_LINES][LRC_LINE_LEN];
static uint16_t s_n    = 0;
static int32_t  s_off  = 0;             /* [offset:+-ms], 载入结束后统一施加 */
static uint8_t  s_gbk  = 0;             /* 1 = 本文件按 GBK 解码 */
static uint8_t  s_enc_known = 0;        /* 1 = 编码已判定(首个非 ASCII 字节) */
static char     s_path[160];            /* 实际尝试打开的 .lrc 路径 */
static char     s_raw[RAW_LINE_LEN + 1];
static char     s_u8[LRC_LINE_LEN * 3]; /* 一行转成 UTF-8 之后的工作缓冲 */

/* ------------------------------------------------------------------ 转码 */

/* UTF-8 合法性粗判: 只看完整字符, 末尾可能是被截断的半个字符时算合法 */
static uint8_t utf8_looks(const unsigned char *p, uint32_t n)
{
    uint32_t i = 0;

    while (i < n) {
        unsigned char c = p[i];

        if (c < 0x80) { i++; continue; }
        if (c >= 0xC2 && c <= 0xDF) {
            if (i + 1 >= n) return 1;
            if ((p[i + 1] & 0xC0) != 0x80) return 0;
            i += 2;
        } else if (c >= 0xE0 && c <= 0xEF) {
            if (i + 2 >= n) return 1;
            if ((p[i + 1] & 0xC0) != 0x80 || (p[i + 2] & 0xC0) != 0x80) return 0;
            i += 3;
        } else if (c >= 0xF0 && c <= 0xF4) {
            if (i + 3 >= n) return 1;
            if ((p[i + 1] & 0xC0) != 0x80 || (p[i + 2] & 0xC0) != 0x80 ||
                (p[i + 3] & 0xC0) != 0x80) return 0;
            i += 4;
        } else {
            return 0;                   /* 0x80..0xC1 / 0xF5.. 都不是合法 UTF-8 首字节 */
        }
    }
    return 1;
}

/* 把一个 Unicode 码点写成 UTF-8, 保证 dst 以 0 结尾, 空间不够就丢弃 */
static void u8_put(char *dst, uint32_t dstsz, uint32_t *pn, uint32_t cp)
{
    uint32_t n = *pn;

    if (cp < 0x80u) {
        if (n + 1u < dstsz) dst[n++] = (char)cp;
    } else if (cp < 0x800u) {
        if (n + 2u < dstsz) {
            dst[n++] = (char)(0xC0u | (cp >> 6));
            dst[n++] = (char)(0x80u | (cp & 0x3Fu));
        }
    } else if (cp < 0x10000u) {
        if (n + 3u < dstsz) {
            dst[n++] = (char)(0xE0u | (cp >> 12));
            dst[n++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
            dst[n++] = (char)(0x80u | (cp & 0x3Fu));
        }
    } else {
        if (n + 4u < dstsz) {
            dst[n++] = (char)(0xF0u | (cp >> 18));
            dst[n++] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
            dst[n++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
            dst[n++] = (char)(0x80u | (cp & 0x3Fu));
        }
    }
    *pn = n;
}

/* GB2312 双字节 -> Unicode 码点, 超出 GB2312(GBK 扩展区) 返回 0 */
static WCHAR gbk2uni(unsigned char b1, unsigned char b2)
{
    uint32_t idx;

    if (b1 < 0xA1u || b1 > 0xF7u) return 0;
    if (b2 < 0xA1u || b2 > 0xFEu) return 0;
    idx = (uint32_t)(b1 - 0xA1u) * GBK2312_COLS + (uint32_t)(b2 - 0xA1u);
    if (idx >= GBK2312_CODES) return 0;
    return (WCHAR)gbk2312_uni[idx];
}
/* src(可能是 GBK 或 UTF-8) -> 一律转成 UTF-8 放进 dst */
static void to_utf8(char *dst, uint32_t dstsz, const char *src, uint32_t srclen)
{
    uint32_t i = 0;
    uint32_t n = 0;

    if (dstsz == 0) return;
    dst[0] = 0;

    while (i < srclen) {
        unsigned char c = (unsigned char)src[i];

        if (c == 0) break;

        if (!s_gbk) {                   /* 本来就是 UTF-8, 字节照搬 */
            dst[n] = 0;
            if (n + 1u >= dstsz) break;
            dst[n++] = (char)c;
            i++;
            continue;
        }

        if (c < 0x80u) {
            u8_put(dst, dstsz, &n, c);
            i++;
        } else if (i + 1u < srclen) {   /* GBK 双字节 */
            WCHAR uni = gbk2uni(c, (unsigned char)src[i + 1u]);
            u8_put(dst, dstsz, &n, (uni == 0) ? (uint32_t)'?' : (uint32_t)uni);
            i += 2;
        } else {
            i++;                        /* 行尾落单的半个汉字, 丢掉 */
        }
    }

    dst[n] = 0;
}

/* ------------------------------------------------------------------ 解析 */

/* [mm:ss.xx] / [mm:ss.xxx] / [mm:ss] -> 毫秒 */
static uint8_t parse_time(const char *p, const char *end, uint32_t *out)
{
    uint32_t mm = 0, ss = 0, frac = 0, nd = 0;

    if (p >= end || *p < '0' || *p > '9') return 0;

    while (p < end && *p >= '0' && *p <= '9') { mm = mm * 10u + (uint32_t)(*p - '0'); p++; }
    if (p >= end || *p != ':') return 0;
    p++;
    while (p < end && *p >= '0' && *p <= '9') { ss = ss * 10u + (uint32_t)(*p - '0'); p++; }

    if (p < end && (*p == '.' || *p == ':')) {
        p++;
        while (p < end && *p >= '0' && *p <= '9') {
            if (nd < 3u) { frac = frac * 10u + (uint32_t)(*p - '0'); nd++; }
            p++;
        }
        if (nd == 1u) frac *= 100u;
        else if (nd == 2u) frac *= 10u;
    }

    *out = (mm * 60u + ss) * 1000u + frac;
    return 1;
}

/* 追加一行歌词（已超出上限则丢弃） */
static void lrc_add(uint32_t t, const char *txt)
{
    if (s_n >= LRC_MAX_LINES) return;

    s_time[s_n] = t;
    strncpy(s_text[s_n], txt, LRC_LINE_LEN - 1);
    s_text[s_n][LRC_LINE_LEN - 1] = 0;
    s_n++;
}

/* 处理一整行(尚未转码) */
static void lrc_handle_line(const char *raw, uint32_t rawlen)
{
    uint32_t ts[LRC_MAX_TAG];
    uint32_t nts = 0;
    uint32_t i;
    const char *p;
    const char *e;
    char tmp[LRC_LINE_LEN];

    to_utf8(s_u8, sizeof(s_u8), raw, rawlen);
    p = s_u8;

    while (*p == '[') {                 /* 一次吃掉行首所有 [tag] */
        const char *q = strchr(p, ']');

        if (q == NULL) break;

        if (p[1] >= '0' && p[1] <= '9') {
            uint32_t ms;
            if (!parse_time(p + 1, q, &ms)) break;
            if (nts < LRC_MAX_TAG) ts[nts++] = ms;
        } else if (strncmp(p + 1, "offset:", 7) == 0) {
            s_off = (int32_t)atoi(p + 8);       /* 元数据, 行尾统一施加 */
        }
        p = q + 1;
    }

    if (nts == 0) return;               /* 元数据行 / 没有时间标签的行 */

    while (*p == ' ' || *p == '\t') p++;
    e = p + strlen(p);
    while (e > p && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) e--;

    i = (uint32_t)(e - p);
    if (i > LRC_LINE_LEN - 1u) i = LRC_LINE_LEN - 1u;
    memcpy(tmp, p, i);
    tmp[i] = 0;

    for (i = 0; i < nts; i++)
        lrc_add(ts[i], tmp);
}

/* 按时间排序(插入排序, 128 行以内足够快; 绝大多数文件本来就有序) */
static void lrc_sort(void)
{
    uint16_t i;
    int32_t  j;

    for (i = 1; i < s_n; i++) {
        uint32_t t = s_time[i];
        char     b[LRC_LINE_LEN];
        memcpy(b, s_text[i], LRC_LINE_LEN);
        j = (int32_t)i - 1;
        while (j >= 0 && s_time[j] > t) {
            s_time[j + 1] = s_time[j];
            memcpy(s_text[j + 1], s_text[j], LRC_LINE_LEN);
            j--;
        }
        s_time[j + 1] = t;
        memcpy(s_text[j + 1], b, LRC_LINE_LEN);
    }
}

/* ------------------------------------------------------------------ 对外 */

void lrc_clear(void)
{
    s_n   = 0;
    s_off = 0;
    s_gbk = 0;
    s_enc_known = 0;
    s_path[0] = 0;
}

/* 当前已载入的歌词行数 */
uint16_t lrc_count(void)
{
    return s_n;
}

/* 取第 idx 行歌词文本（UTF-8）；越界返回空串 */
const char *lrc_line(uint16_t idx)
{
    if (idx >= s_n) return NULL;
    return s_text[idx];
}

/* 二分查找 pos_ms 对应的歌词行号；还没到第一行返回 -1 */
int16_t lrc_index(uint32_t pos_ms)
{
    int16_t lo = 0;
    int16_t hi = (int16_t)s_n - 1;
    int16_t ans = -1;

    while (lo <= hi) {
        int16_t mid = (int16_t)(lo + (hi - lo) / 2);
        if (s_time[mid] <= pos_ms) {
            ans = mid;
            lo = (int16_t)(mid + 1);
        } else {
            hi = (int16_t)(mid - 1);
        }
    }
    return ans;
}

/* /MUSIC/a.mp3 -> /MUSIC/a.lrc (把最后一个扩展名换掉) */
static void lrc_make_path(const char *mp3, char *out, uint32_t outsz)
{
    uint32_t n = (uint32_t)strlen(mp3);
    char    *dot;
    char    *slash;

    if (n > outsz - 1u) n = outsz - 1u;
    memcpy(out, mp3, n);
    out[n] = 0;

    dot   = strrchr(out, '.');
    slash = strrchr(out, '/');
    if (dot != NULL && (slash == NULL || dot > slash))
        *dot = 0;

    n = (uint32_t)strlen(out);
    if (n + 4u < outsz) {
        out[n]     = '.';
        out[n + 1] = 'l';
        out[n + 2] = 'r';
        out[n + 3] = 'c';
        out[n + 4] = 0;
    }
}

/* 载入与 mp3 同名的 .lrc；成功 1，无文件/解析失败 0 */
uint8_t lrc_load(const char *mp3_path)
{
    FIL     f;
    FRESULT fr;
    UINT    br = 0;
    uint8_t buf[LRC_READ_CHUNK];
    uint32_t rl = 0;
    uint16_t i;
    uint8_t  first_chunk = 1;
    uint8_t  skip_head   = 0;

    lrc_clear();
    if (mp3_path == NULL || mp3_path[0] == 0) return 0;

    lrc_make_path(mp3_path, s_path, sizeof(s_path));

    fr = f_open(&f, s_path, FA_READ);
    if (fr != FR_OK) {
        s_path[0] = 0;                  /* 没这首歌的歌词, 不算错误 */
        return 0;
    }

    for (;;) {
        fr = f_read(&f, buf, sizeof(buf), &br);
        if (fr != FR_OK || br == 0) break;

        if (first_chunk) {
            first_chunk = 0;
            if (br >= 3u && buf[0] == 0xEFu && buf[1] == 0xBBu && buf[2] == 0xBFu) {
                s_gbk = 0;
                s_enc_known = 1;        /* BOM: 明确 UTF-8 */
                skip_head = 3;
            } else if (br >= 2u && ((buf[0] == 0xFFu && buf[1] == 0xFEu) ||
                                    (buf[0] == 0xFEu && buf[1] == 0xFFu))) {
                f_close(&f);            /* UTF-16 歌词不支持 */
                s_path[0] = 0;
                return 0;
            }
        }

        /* 编码判定推迟到第一个非 ASCII 字节: 之前的行全是 ASCII,
           UTF-8 与 GBK 逐字节相同, 先按哪种解析都不影响结果。
           (只在首个 chunk 上判会有漏洞: 元数据头很长、第一行中文
            出现在第 2 个 chunk 时会被误判成 UTF-8) */
        if (!s_enc_known) {
            uint16_t k = 0u;
            while ((k < (uint16_t)br) && ((unsigned char)buf[k] < 0x80u)) k++;
            if (k < (uint16_t)br) {
                s_gbk = (uint8_t)(utf8_looks(buf + k, (uint32_t)(br - k)) ? 0u : 1u);
                s_enc_known = 1;
            }
        }

        for (i = skip_head; i < (uint16_t)br; i++) {
            char c = (char)buf[i];

            if (c == '\n') {
                s_raw[rl] = 0;
                lrc_handle_line(s_raw, rl);
                rl = 0;
            } else if (c != '\r') {
                if (rl < RAW_LINE_LEN) s_raw[rl++] = c;
            }
        }
        skip_head = 0;

        if (br < sizeof(buf)) break;
    }

    if (rl > 0u) {
        s_raw[rl] = 0;
        lrc_handle_line(s_raw, rl);
    }

    f_close(&f);
    lrc_sort();

    if (s_off != 0) {                   /* LRC 约定: 正值让歌词提前 */
        for (i = 0; i < s_n; i++) {
            int32_t t = (int32_t)s_time[i] - s_off;
            s_time[i] = (t < 0) ? 0u : (uint32_t)t;
        }
    }

    printf("[LRC] %u line(s) %s\r\n", (unsigned)s_n, s_gbk ? "GBK" : "UTF-8");
    return (uint8_t)s_n;
}
