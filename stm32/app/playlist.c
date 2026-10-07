/*
 * playlist.c - 扫描 SD 卡生成播放列表：MP3 过滤 + 自然排序 + 静态存储
 *
 * 扫描顺序：先 /MUSIC；不存在则扫根目录；只收 *.mp3（大小写不敏感），跳过子目录、
 *           隐藏文件与 0 字节文件；按自然排序使 01/02/10 的顺序正确。
 * 依赖：FatFS（f_opendir/f_readdir，长名以 UTF-8 字节给出，FF_LFN_UNICODE=2）。
 * 调用：app/main.c（上电与下载完成后 pl_scan()）；app/audio_srv.c、app/mp3info.c
 *       用 pl_get()/pl_path() 取曲目，app/netdl.c 用 pl_base() 决定新歌落盘目录。
 * 内存：PL_MAX_SONGS * sizeof(pl_entry_t) 静态数组（300 * 68 B ≈ 20 KB），不用 malloc。
 */
#include "playlist.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>

static pl_entry_t s_list[PL_MAX_SONGS];
static uint16_t   s_count = 0;
static char       s_base[16] = "/MUSIC";   /* 扫描基准目录: "/MUSIC" 或 "" */

/* ------------------------------------------------------------------ */
/* 判断是否以 .mp3 结尾（大小写不敏感） */
static uint8_t endswith_mp3(const char *n)
{
    size_t L = strlen(n);
    const char *p;
    if (L < 4u) return 0u;
    p = n + L - 4u;
    return (uint8_t)(p[0] == '.' &&
                    (p[1] == 'm' || p[1] == 'M') &&
                    (p[2] == 'p' || p[2] == 'P') &&
                    (p[3] == '3'));
}

/* 自然比较：连续数字段按数值比，其余按字节比。
 * GBK 引导字节(0x81..0xFE) 不会与 ASCII 数字(0x30..0x39) 冲突，安全。 */
static int natcmp(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0')
    {
        int da = isdigit((unsigned char)*a) != 0;
        int db = isdigit((unsigned char)*b) != 0;
        if (da && db)
        {
            unsigned long va = 0u, vb = 0u;
            while (isdigit((unsigned char)*a)) { va = va * 10u + (unsigned long)(*a - '0'); a++; }
            while (isdigit((unsigned char)*b)) { vb = vb * 10u + (unsigned long)(*b - '0'); b++; }
            if (va != vb) return (va < vb) ? -1 : 1;
        }
        else
        {
            int ca = (unsigned char)*a, cb = (unsigned char)*b;
            if (ca != cb) return ca - cb;
            a++; b++;
        }
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

/* qsort 比较函数：按自然排序比较文件名 */
static int cmp_entry(const void *pa, const void *pb)
{
    const pl_entry_t *ea = (const pl_entry_t *)pa;
    const pl_entry_t *eb = (const pl_entry_t *)pb;
    return natcmp(ea->name, eb->name);
}

/* ------------------------------------------------------------------ */
uint16_t pl_scan(void)
{
    DIR      d;
    FILINFO  fno;
    FRESULT  r;

    s_count = 0;

    /* 优先 /MUSIC，不存在则根目录 */
    r = f_opendir(&d, "/MUSIC");
    if (r == FR_OK)
    {
        strcpy(s_base, "/MUSIC");
    }
    else
    {
        r = f_opendir(&d, "/");
        if (r != FR_OK) return 0u;
        strcpy(s_base, "");
    }

    for (;;)
    {
        fno.fname[0] = '\0';
        r = f_readdir(&d, &fno);
        if (r != FR_OK || fno.fname[0] == '\0') break;   /* 目录读完 */
        if (fno.fattrib & AM_DIR) continue;                /* 跳过子目录 */
        if (fno.fattrib & AM_HID) continue;                /* 跳过隐藏项 */
        if (fno.fsize == 0u) continue;                     /* 跳过 0 字节 */

        /* FatFs R0.16 + FF_USE_LFN>=1：长文件名直接在 fno.fname 里
         * （TCHAR=char；FF_LFN_UNICODE=0 → 当前代码页 936 即 GBK 字节）。
         * 旧版的 fno.lfname / fno.lfsize 字段在 R0.16 已不存在。 */
        if (!endswith_mp3(fno.fname)) continue;
        if (s_count >= PL_MAX_SONGS) break;

        strncpy(s_list[s_count].name, fno.fname, PL_NAME_LEN - 4u);
        s_list[s_count].name[PL_NAME_LEN - 4u] = '\0';
        if (strlen(s_list[s_count].name) > (size_t)(PL_NAME_LEN - 5u))
            strcat(s_list[s_count].name, "...");           /* 超长追加省略号 */
        s_list[s_count].size = (uint32_t)fno.fsize;
        s_count++;
    }

    f_closedir(&d);

    qsort(s_list, s_count, sizeof(pl_entry_t), cmp_entry);
    return s_count;
}

/* 取第 idx 首（0 起）；越界返回 NULL */
const pl_entry_t *pl_get(uint16_t idx)
{
    if (idx >= s_count) return (const pl_entry_t *)0;
    return &s_list[idx];
}

uint16_t pl_count(void) { return s_count; }

/* 扫描基准目录, 供网页上传写卡时对齐歌单目录 */
const char *pl_base(void) { return s_base; }

/* 拼出第 idx 首的完整路径写入 out（供 f_open 用） */
const char *pl_path(uint16_t idx, char *out, uint32_t outsz)
{
    const pl_entry_t *e;
    if (idx >= s_count || out == (char *)0 || outsz < 8u)
    {
        if (out != (char *)0 && outsz > 0u) out[0] = '\0';
        return out;
    }
    e = &s_list[idx];
    snprintf(out, outsz, "%s/%s", s_base, e->name);
    return out;
}
