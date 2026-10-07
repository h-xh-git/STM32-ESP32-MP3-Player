#ifndef __PLAY_RESUME_H
#define __PLAY_RESUME_H

/*
 * play_resume.h - 断点续播（把当前歌曲 + 文件内偏移存到 F407 片内 Flash）
 *
 * 存储落在 F407 片内 Flash 最后一个扇区（Sector 7: 0x08060000..0x0807FFFF，
 * 128 KB），不占 SD 卡、与代码区完全隔离（本工程 ROM 约 376 KB，见
 * project/MDK(V5)/Listings/Project.map）。
 *
 * 实际搬运由 bsp/storage/flash_slot.c 负责：整个扇区是一个只追加的 96 B
 * 环形条目区，一条快照 = 条目头（magic/seq/len/pos_ms/CRC）+ 本文件的
 * 76 B payload。播放中每 1 s 追加一条，写满 1365 条（≈22.7 分钟）整区擦一次。
 *
 * 记录内容：曲目序号 + 文件名 + 文件大小（稳定部分，76 B payload）
 *           + 该曲已经播到的毫秒数（在条目头里，随每条检查点一起变）
 *
 * ★关键取舍：存的是正在出声的位置（audio_srv 采纳 ESP32 每 500 ms 回报的
 *   真实播放位置），而不是 STM32 已经预读送出的字节数。播放时 ESP32 侧有
 *   64 KB 压缩环 + 128 KB PCM 环，预读位置比出声位置超前 2~3 s；存预读位置
 *   会让每次续播都往前跳一段。续播时用音频总时长把 ms 折算回文件字节偏移，
 *   再向前找 MP3 帧同步头对齐，效果等价于按文件偏移续播，VBR 文件也不跑偏。
 *
 * 调用顺序（main.c）：
 *   pl_scan() 之后             -> play_resume_init()       扫扇区，读回上次快照
 *   play_resume_init() 之后    -> play_resume_find_saved() 按文件名核对曲目
 *                                                          （返回 1 则恢复该曲）
 *   set_track() 里             -> play_resume_set_track()  登记当前曲目
 *   主循环每轮                 -> play_resume_tick(ms)      周期检查点(默认 1 s)
 *   切歌 / 暂停 / 收到停止命令  -> play_resume_save(ms, 1)   立即落盘
 *   空闲(暂停)时               -> play_resume_prime(ms)     提前滚动条目区
 *   歌单为空 / 无有效曲目       -> play_resume_clear()       写墓碑，下次不恢复
 *
 * 掉电最坏情况：位置退一条检查点（约 1 s 音频）。只有整区擦除那一次
 * （约 22.7 分钟一次）若正好在 擦完/写回前 掉电，会退到歌首。
 * 曲目号/文件名不会与位置错位（它们在同一条快照里）。
 */
#include <stdint.h>

/* 文件名缓冲长度（与 playlist.h 的 PL_NAME_LEN 一致；不存在循环包含） */
#define PR_NAME_LEN   64u

#define PR_OK         0u   /* 保存成功 */
#define PR_UNCHANGED  1u   /* 与 Flash 里最新一条完全相同，没写 */
#define PR_SKIP       2u   /* 当前没有有效曲目 / 记录还没读回来 */
#define PR_ERR        3u   /* 擦除或编程失败（写保护等） */

/* 一次断点快照的内容，可从 SD 卡歌单重建；丢了只是回到第 1 首 */
typedef struct
{
    uint32_t track;                    /* 曲目序号(0 起)，对应 playlist 下标 */
    uint32_t pos_ms;                   /* 该曲已经播到的位置(ms)，按出声位置算 */
    char     name[PR_NAME_LEN];        /* 文件名(GBK 字节，含结尾 0) */
    uint8_t  valid;                    /* 1 = 记录有效（有曲目 + 位置可信） */
    uint8_t  slots;                    /* 诊断：扇区里现有有效条目数(封顶 255) */
    uint8_t  src;                      /* 诊断：保留，恒为 0xFF */
} play_resume_snapshot_t;

/* 上电调用：扫扇区并读回上次快照；即使没写过也返回 0，可无脑调用 */
uint8_t  play_resume_init(void);

/* 取上次快照（内部缓存，play_resume_init() 之后恒定有效） */
const play_resume_snapshot_t *play_resume_get(void);

/* 按文件名 + 文件大小在当前歌单里核对快照的曲目；返回 1 表示可以恢复，
   并把 0 起的曲目序号与已播毫秒写到 *track0 / *pos_ms。
   核对不过（换卡、改名、删文件）返回 0，此时应按默认行为从第 1 首开始。 */
uint8_t  play_resume_find_saved(uint16_t *track0, uint32_t *pos_ms);

/* 每首曲子打开成功后调用：登记当前曲目，并复位位置跟踪 */
void     play_resume_set_track(uint16_t idx0);

/* 切成没有有效曲目（歌单空 / 卡坏）时调用：清掉跟踪状态 */
void     play_resume_open_track(void);

/* 立即写一条快照；pos_ms 一般传 audio_srv_pos_ms()（真实出声位置）。
   force 只是调用方语义（当前两条路径都直接写），保留以便将来降频。 */
uint8_t  play_resume_save(uint32_t pos_ms, uint8_t force);

/* 主循环调用：轮到检查点了就写，返回下一次检查点的绝对时刻(tick_ms 时间基) */
uint32_t play_resume_tick(uint32_t now_ms);

/* 空闲(暂停/卡在等数据)时调用：条目区写满就提前滚动，避免播放中阻塞 */
void     play_resume_prime(uint32_t now_ms);

/* 写墓碑快照（track = 0xFFFFFFFF）：下次上电不会误恢复 */
void     play_resume_clear(void);


#endif /* __PLAY_RESUME_H */
