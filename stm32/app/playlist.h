#ifndef __PLAYLIST_H
#define __PLAYLIST_H

/*
 * playlist.h - 扫描 SD 卡上的 MP3 曲目, 生成播放列表
 *
 * 扫描顺序: 先 /MUSIC 目录; 不存在则扫根目录 / 。
 * 只收集 *.mp3 (大小写不敏感), 跳过子目录、隐藏文件、0 字节文件。
 * 上限 PL_MAX_SONGS 首, 按"自然排序"(数字段按数值) 升序排列, 保证
 * 01.mp3..02.mp3..10.mp3 的顺序正确。
 *
 * 文件名以 UTF-8 字节存储 (FatFS FF_LFN_UNICODE=2), 直接供 UTF-8 字库
 * 显示; 超长截断并在末尾追加省略号。
 */
#include "ff.h"            /* FatFS: FILINFO / FSIZE / FR_ */

#define PL_MAX_SONGS   300u
#define PL_NAME_LEN    64u        /* GBK 字节: 最多 31 个汉字 + '...' + '\0' */

typedef struct
{
    char     name[PL_NAME_LEN];    /* 曲目文件名 (GBK, basename, 已截断) */
    uint32_t size;                 /* 文件字节大小                      */
} pl_entry_t;

/* 扫描并填充列表, 返回曲目数 (0 = 空目录或失败) */
uint16_t pl_scan(void);

/* 取第 idx 首 (idx 从 0 起); 越界返回 NULL */
const pl_entry_t *pl_get(uint16_t idx);

/* 当前列表曲目数 */
uint16_t pl_count(void);

/* 把第 idx 首的完整路径写入 out (供 f_open 使用), 返回 out */
const char *pl_path(uint16_t idx, char *out, uint32_t outsz);

/* 本次扫描用的基准目录: "/MUSIC" 或 "" (根目录)。
   供网页上传 (app/netdl.c) 把新歌写到与歌单同一个目录里。 */
const char *pl_base(void);

#endif /* __PLAYLIST_H */
