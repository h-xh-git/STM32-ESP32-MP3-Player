#ifndef __MP3INFO_H
#define __MP3INFO_H

/*
 * mp3info.h - 从 MP3 文件里读出真实时长(毫秒)
 *
 * 步骤: 跳过 ID3v2 标签 -> 扫描第一帧同步字 -> 解析 MPEG 版本/层/比特率/采样率
 *       -> 查 Xing/Info/VBRI 帧数(VBR 精确) -> 没有就按首帧比特率做 CBR 估算。
 * 只读文件头几 KB, 一次调用约几毫秒。
 */
#include <stdint.h>

/* 解析时长, 单位毫秒; 失败(打不开/不是 MP3)返回 0 */
uint32_t mp3_duration_ms(const char *path);

/* 最近一次解析出的参数, 供串口打印曲目信息 */
uint32_t mp3_last_bitrate(void);      /* kbps, 0 = 未知 */
uint32_t mp3_last_samplerate(void);   /* Hz,   0 = 未知 */
uint32_t mp3_last_vbr(void);          /* 1 = Xing/Info/VBRI 帧数 */

#endif /* __MP3INFO_H */
