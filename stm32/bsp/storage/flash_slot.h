#ifndef __FLASH_SLOT_H
#define __FLASH_SLOT_H

/*
 * flash_slot.h - F407 片内 Flash 断点记录存储（本工程唯一碰片内 Flash 的地方）
 *
 * ================== 位置 ==================
 * 只用一个扇区：Sector 7 = 0x08060000..0x0807FFFF（128 KB）。
 * 固件实测 Total ROM 224556 B（约 219 KB），LR_IROM1 结束于 0x08036F28（见
 * project/MDK(V5)/Listings/Project.map 里 LR_IROM1 的结束地址），Sector 6/7
 * 都是空的；但分散加载文件 project/MDK(V5)/Project_ccm.sct 的 LR_IROM1 名义
 * 覆盖 0x08000000..0x0807FFFF 整个 512 KB，所以固件一旦增长越过 0x08060000
 * 就会和这个断点区冲突——扩到那之前必须先改这里。
 *
 * ================== 形态：一个只追加的环形条目区 ==================
 * 整个扇区按 96 B 一条切分，共 1365 条；一次检查点只追加一条，从不回头改。
 *
 *   条目 96 B 布局
 *     off  0 : u32 magic  = 0x504F5332（POS2）
 *     off  4 : u32 seq    （递增；只用于诊断和排序）
 *     off  8 : u16 len    （payload 字节数，4 的倍数，<= 76）
 *     off 10 : u16 kind   （1 = 断点快照）
 *     off 12 : u32 pos_ms （写这条时该曲已播毫秒）
 *     off 16 : payload[len]
 *     off 16+len : u32 crc16（覆盖 off0..off(16+len-1)，CCITT-FALSE）
 *
 * 写满 1365 条后整区擦一次。1 s 一条检查点 -> 约 22.7 分钟滚动一次，
 * 每次滚动擦除约 1 s（期间 CPU 取指被 Flash 控制器 stall，主循环卡住；
 * ESP32 侧有 64 KB 压缩环 + 128 KB PCM 环和 600 ms 数据看门狗，不会断音）。
 *
 * ================== 掉电安全 ==================
 * 1) 追加写 + magic/CRC 双重校验：撕裂的条目一定校验不过，会被跳过，
 *    上电时只会采纳最后一条完好快照（最坏退回 1 条检查点，约 1 s 音频）。
 * 2) 上电扫描定写指针：从 0 号条目顺序读，第一条全 0xFF 的条目就是写指针。
 *    这样不需要额外保存指针，也就没有 指针与数据不一致 的失效模式。
 * 3) 唯一的整扇区擦除发生在条目写满时：擦之前先把最新快照拷到 RAM，
 *    擦完立刻写回。若这一步掉电，后果是这次上电没有快照（当成首次上电），
 *    不会采用脏数据，也不会动 SD 卡。
 *
 * ================== API ==================
 * 典型调用顺序（详见 app/play_resume.c）:
 *   flash_slot_init();                                  // 上电扫一遍
 *   if (flash_slot_load(buf, cap, &len, &pos)) {...}    // 取最新快照
 *   flash_slot_save(payload, len, pos_ms);              // 周期/立即写一条
 *   if (flash_slot_have_uncommitted()) flash_slot_prime();  // 空闲时提前滚
 */
#include <stdint.h>

#define FLS_OK          0u   /* 写入成功 */
#define FLS_UNCHANGED   1u   /* 与 Flash 里最新一条逐字节相同，什么都没做 */
#define FLS_ERR         2u   /* 擦除或编程失败 */

/* payload 长度约束（byte）：4 的倍数，16..76 */
#define FLS_DATA_MIN    16u
#define FLS_DATA_MAX    76u

/* 上电扫一遍扇区，建立写指针；返回 1 = 读到有效快照，0 = 没有 */
uint8_t  flash_slot_init(void);

/* 取最新一条快照：buf/cap 是调用方缓冲区，长度写 *len，位置写 *pos_ms。
   返回 1 成功；0 = 没有快照（首次上电 / 刚被擦过）。 */
uint8_t  flash_slot_load(uint8_t *buf, uint16_t cap, uint16_t *len, uint32_t *pos_ms);

/* 追加一条快照：data/len 是内容（len 必须是 4 的倍数，16..76），
   pos_ms 单独传（它会随时间变，所以不进 data）。与最新一条完全相同
   （含 pos_ms）时返回 FLS_UNCHANGED，不做任何 Flash 操作。 */
uint8_t  flash_slot_save(const void *data, uint16_t len, uint32_t pos_ms);

/* 写指针是不是已经到条目区末尾（true = 下次写入前要先阻塞擦一次扇区） */
uint8_t  flash_slot_have_uncommitted(void);

/* 空闲时调用：写指针到末尾就提前滚动（擦区 + 把最新快照写回），
   把一次约 1 s 的阻塞从播放中挪到空闲时。 */
uint8_t  flash_slot_prime(void);

/* 上次编程/擦除失败：作废缓存，下次访问重新扫描 */
void     flash_slot_note_failed(void);

/* 诊断计数 */
uint32_t flash_slot_erases(void);    /* 整区擦除次数 */
uint32_t flash_slot_writes(void);    /* 成功追加的条目数 */
uint32_t flash_slot_rolls(void);     /* 滚动次数（含 prime 提前滚） */
uint32_t flash_slot_entries(void);   /* 当前扇区里有效条目数 */

#endif /* __FLASH_SLOT_H */
