#ifndef __LYRICS_H
#define __LYRICS_H
#include <stdint.h>

/*
 * lyrics.h - .lrc 滚动歌词
 *
 * 歌词文件按"和 mp3 同目录、同主文件名"的约定查找:
 *
 *     /MUSIC/周杰伦 - 晴天.mp3   ->   /MUSIC/周杰伦 - 晴天.lrc
 *
 * 支持标准的 [mm:ss.xx] 行时间标签, 一行可以带多个时间标签(副歌复用),
 * 以及 [offset:+-ms] 整体偏移; [ti:]/[ar:]/[al:]/[by:] 等元数据会被忽略。
 *
 * 编码自动识别: UTF-8 BOM / 无 BOM 的合法 UTF-8 原文照搬, 其余按 GBK
 * (CP936) 解码 —— 中文 .lrc 绝大多数是 GBK。转码走自带的 gbk2312_map.h
 * (约 16 KB Flash); 超出 GB2312 的罕见字显示成 '?'。
 *
 * 存储是静态数组, 不引入 malloc:
 *     LRC_MAX_LINES * LRC_LINE_LEN = 128 * 96 = 12 KB RAM
 * 超出的行会被丢弃(取前 128 行), 超长的行会被截断(96 字节 UTF-8,
 * 约 32 个汉字, 面板一行本来也只放得下 10 来个字)。
 */

#define LRC_MAX_LINES   128
#define LRC_LINE_LEN    96

/* 清空当前歌词(切歌时先调用) */
void        lrc_clear(void);

/* 载入 mp3 对应的 .lrc, 返回解析到的行数; 0 = 没有歌词文件/解析失败 */
uint8_t     lrc_load(const char *mp3_path);

/* 已载入的行数 */
uint16_t    lrc_count(void);

/* pos_ms 时刻应该高亮的行号; -1 = 还没到第一行(前奏) */
int16_t     lrc_index(uint32_t pos_ms);

/* 第 idx 行的文本(UTF-8), 越界返回 NULL */
const char *lrc_line(uint16_t idx);

#endif /* __LYRICS_H */
