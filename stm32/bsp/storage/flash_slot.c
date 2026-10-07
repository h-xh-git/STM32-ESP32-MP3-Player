/*
 * flash_slot.c - F407 片内 Flash 断点记录存储（本工程唯一碰片内 Flash 的地方）
 *
 * ==================== 形态：一个只追加的环形条目区 ====================
 * 整个 Sector 7（0x08060000..0x0807FFFF = 128 KB）按 96 B 一条切成
 * FLS_ENT_MAX = 131072 / 96 = 1365 条，当成一个只追加的环形区：
 * 一次检查点只追加一条，从不回头改，写满 1365 条才整区擦一次。
 *
 *   条目 96 B
 *     off  0 : u32 magic  = 0x504F5332（POS2）
 *     off  4 : u32 seq    （递增；只用于诊断和排序）
 *     off  8 : u16 len    （payload 字节数，4 的倍数，16..76）
 *     off 10 : u16 kind   （1 = 断点快照）
 *     off 12 : u32 pos_ms （写这条时该曲已播毫秒）
 *     off 16 : payload[len]
 *     off 16+len : u32 crc16（覆盖 off0..off(16+len-1)，CCITT-FALSE）
 *
 * 设计取舍：不做「曲目区 + 位置区」两区分开存。
 * 擦除粒度是整个扇区，分区后擦一个区会连带清掉另一个区，跨区一致性又多一套
 * 失效模式；分区还会逼着 payload 与指针字段复用偏移，而偏移一旦复用，
 * 「内容变了没有」的比较就可能永远成立，退化成每秒擦一次扇区。
 * 整条快照一起追加后，逻辑只剩 追加 / 扫描 / 整区擦 三种操作，没有跨区
 * 一致性问题。代价是每个检查点写 96 B（24 个字，几百 us）而不是 16 B，
 * 换来擦除频率降到约 22.7 分钟一次。
 *
 * ==================== 掉电安全 ====================
 * 1) 追加写 + magic/CRC 双重校验：撕裂条目一定校验不过，会被跳过；上电只
 *    采纳最后一条完好快照（最坏退回 1 条检查点，约 1 s 音频）。
 * 2) 上电扫描定写指针：从 0 号条目顺序读，第一条全 0xFF 的条目就是写指针。
 *    不需要额外保存指针，也就没有 指针与数据不一致 的失效模式。
 * 3) 唯一的整区擦除在条目写满时或 prime 提前滚时：擦之前先把最新快照拷到
 *    RAM，擦完立刻写回。若这一步掉电，这次上电就没有快照（当成首次上电），
 *    不会采用脏数据，也不动 SD 卡。
 *
 * 注意：本模块不关中断。擦除约 1 s 期间 CPU 取指会被 Flash 控制器 stall，
 * 主循环卡住，但 USART2 的 RX 环（2 KB）够缓冲，ESP32 侧还有 600 ms 数据
 * 看门狗会自愈。
 */

#include "flash_slot.h"
#include "stm32f4xx.h"
#include <string.h>
#include <stdio.h>

/* ---------------- 布局常量 ---------------- */
#define FLS_BASE_ADDR   0x08060000u                 /* Sector 7 起始 */
#define FLS_SECTOR      FLASH_Sector_7
#define FLS_VOLTAGE     VoltageRange_3

#define FLS_ENT         96u                         /* 一条 96 B */
#define FLS_ENT_MAX     (0x20000u / FLS_ENT)        /* 1365 条 */
#define FLS_ENT_WORDS   (FLS_ENT / 4u)              /* 24 个字，程序只需写前 (16+len+4)/4 个 */
#define FLS_LIMIT       (FLS_ENT_MAX * FLS_ENT)     /* 131040 B，末尾 32 B 不用 */

#define FLS_MAGIC       0x504F5332u                 /* POS2 */

#define FLS_OFF_MAGIC   0u
#define FLS_OFF_SEQ     4u
#define FLS_OFF_LEN     8u
#define FLS_OFF_KIND    10u
#define FLS_OFF_POS     12u
#define FLS_OFF_PAY     16u

/* ---------------- 内部状态 ---------------- */
typedef union {
    uint8_t  b[FLS_ENT];
    uint32_t w[FLS_ENT_WORDS];
} fls_ent_u;

static fls_ent_u s_ent;                             /* 组装待写条目（扇区是 byte 可寻址的） */
static uint8_t   s_pay[FLS_DATA_MAX];               /* 最新快照 payload */
static uint8_t   s_have;                            /* 是否已有有效快照 */
static uint8_t   s_scanned;                         /* 是否已扫描过扇区 */
static uint8_t   s_plen;                            /* payload 长度 */
static uint32_t  s_pos;                             /* 最新快照的位置 ms */
static uint32_t  s_seq;                             /* 最新快照的 seq */
static uint32_t  s_off;                             /* 写指针（扇区内字节偏移） */
static uint32_t  s_cnt;                             /* 当前有效条目数 */
static uint32_t  s_erases;
static uint32_t  s_writes;
static uint32_t  s_rolls;

/* ---------------- 小工具 ---------------- */
/* CRC-16/CCITT-FALSE：poly 0x1021、init 0xFFFF、不反射（协议层同一套） */
static uint16_t fls_crc16(const uint8_t *p, uint32_t n)
{
    uint16_t crc = 0xFFFFu;
    uint32_t i;
    uint8_t  b;
    for (i = 0u; i < n; i++) {
        crc ^= (uint16_t)((uint16_t)p[i] << 8);
        for (b = 0u; b < 8u; b++) {
            if ((crc & 0x8000u) != 0u) {
                crc = (uint16_t)((uint16_t)(crc << 1) ^ 0x1021u);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}

/* 小端读 u16 */
static uint16_t fls_get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* 小端读 u32 */
static uint32_t fls_get_u32(const uint8_t *p)
{
    return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* 小端写 u16 */
static void fls_put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

/* 小端写 u32 */
static void fls_put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/* 第 idx 条条目的片内 Flash 绝对地址 */
static uint32_t fls_ent_addr(uint32_t idx)
{
    return FLS_BASE_ADDR + (idx * FLS_ENT);
}

/* 校验一条条目：magic + kind + len + CRC。通过则输出 len / pos / seq */
static uint8_t fls_ent_check(const uint8_t *p, uint16_t *plen,
                             uint32_t *ppos, uint32_t *pseq)
{
    uint16_t n;
    uint16_t crc;
    uint32_t want;

    if (fls_get_u32(p + FLS_OFF_MAGIC) != FLS_MAGIC) {
        return 0u;
    }
    if (fls_get_u16(p + FLS_OFF_KIND) != 1u) {
        return 0u;
    }
    n = fls_get_u16(p + FLS_OFF_LEN);
    if ((n == 0u) || (n > (uint16_t)FLS_DATA_MAX) || ((n & 3u) != 0u)) {
        return 0u;
    }
    crc  = fls_crc16(p, (uint32_t)FLS_OFF_PAY + (uint32_t)n);
    want = fls_get_u32(p + (uint32_t)FLS_OFF_PAY + (uint32_t)n);
    if ((uint32_t)crc != want) {
        return 0u;
    }
    if (plen != 0) { *plen = n; }
    if (ppos != 0) { *ppos = fls_get_u32(p + FLS_OFF_POS); }
    if (pseq != 0) { *pseq = fls_get_u32(p + FLS_OFF_SEQ); }
    return 1u;
}

/* 上电扫描：顺序读条目定写指针，同时记住最后一条有效快照 */
static void fls_scan(void)
{
    uint32_t i;
    uint32_t blank = FLS_ENT_MAX;
    uint16_t n = 0u;
    uint32_t pos = 0u;
    uint32_t seq = 0u;

    s_scanned = 1u;
    s_have    = 0u;
    s_cnt     = 0u;
    s_seq     = 0u;
    s_plen    = 0u;
    s_pos     = 0u;

    for (i = 0u; i < FLS_ENT_MAX; i++) {
        const uint8_t *p = (const uint8_t *)fls_ent_addr(i);
        if (fls_get_u32(p + FLS_OFF_MAGIC) == 0xFFFFFFFFu) {
            blank = i;
            break;
        }
        if (fls_ent_check(p, &n, &pos, &seq) != 0u) {
            memcpy(s_pay, p + FLS_OFF_PAY, (size_t)n);
            s_plen = (uint8_t)n;
            s_pos  = pos;
            s_seq  = seq;
            s_have = 1u;
            s_cnt++;
        }
    }
    s_off = blank * FLS_ENT;

    printf("[FLS] scan: valid=%lu wp=%lu/%lu seq=%lu have=%u\r\n",
           (unsigned long)s_cnt, (unsigned long)s_off, (unsigned long)FLS_LIMIT,
           (unsigned long)s_seq, (unsigned)s_have);
}

/* 擦掉整个 Sector 7。擦完扇区里什么都没有，RAM 里的快照由调用方写回。 */
static uint8_t fls_erase(void)
{
    FLASH_Status st;

    FLASH_Unlock();
    st = FLASH_EraseSector(FLS_SECTOR, FLS_VOLTAGE);
    FLASH_Lock();
    s_erases++;

    if (st != FLASH_COMPLETE) {
        printf("[FLS] !! erase FAILED status=%u\r\n", (unsigned)st);
        return 0u;
    }
    s_off  = 0u;
    s_cnt  = 0u;
    s_have = 0u;
    s_plen = 0u;
    s_pos  = 0u;
    return 1u;
}

/* 在 s_off 处追加一条；成功则推进写指针并更新 RAM 快照 */
static uint8_t fls_append(const uint8_t *data, uint16_t len, uint32_t pos_ms)
{
    uint32_t words;
    uint32_t i;
    uint32_t seq;
    FLASH_Status st = FLASH_COMPLETE;

    if ((s_off + FLS_ENT) > FLS_LIMIT) {
        return 0u;                                  /* 满了，调用方先 fls_erase() */
    }

    seq = s_seq + 1u;
    if (seq == 0u) {
        seq = 1u;
    }

    memset(s_ent.b, 0xFF, (size_t)FLS_ENT);
    fls_put_u32(s_ent.b + FLS_OFF_MAGIC, FLS_MAGIC);
    fls_put_u32(s_ent.b + FLS_OFF_SEQ, seq);
    fls_put_u16(s_ent.b + FLS_OFF_LEN, len);
    fls_put_u16(s_ent.b + FLS_OFF_KIND, 1u);
    fls_put_u32(s_ent.b + FLS_OFF_POS, pos_ms);
    memcpy(s_ent.b + FLS_OFF_PAY, data, (size_t)len);
    fls_put_u32(s_ent.b + (uint32_t)FLS_OFF_PAY + (uint32_t)len,
                (uint32_t)fls_crc16(s_ent.b,
                                    (uint32_t)FLS_OFF_PAY + (uint32_t)len));

    words = ((uint32_t)FLS_OFF_PAY + (uint32_t)len + 4u) / 4u;

    FLASH_Unlock();
    for (i = 0u; i < words; i++) {
        st = FLASH_ProgramWord(FLS_BASE_ADDR + s_off + (i * 4u), s_ent.w[i]);
        if (st != FLASH_COMPLETE) {
            break;
        }
    }
    FLASH_Lock();

    if (st != FLASH_COMPLETE) {
        printf("[FLS] !! program FAILED status=%u word=%lu\r\n",
               (unsigned)st, (unsigned long)i);
        return 0u;
    }

    /* 回读整条校验（同时验证地址没算错） */
    {
        uint16_t n = 0u;
        uint32_t p2 = 0u;
        uint32_t q2 = 0u;
        if (fls_ent_check((const uint8_t *)fls_ent_addr(s_off / FLS_ENT),
                          &n, &p2, &q2) == 0u) {
            printf("[FLS] !! verify FAILED off=%lu\r\n", (unsigned long)s_off);
            return 0u;
        }
    }

    memcpy(s_pay, data, (size_t)len);
    s_plen = (uint8_t)len;
    s_pos  = pos_ms;
    s_seq  = seq;
    s_have = 1u;
    s_cnt++;
    s_writes++;
    s_off += FLS_ENT;
    return 1u;
}

/* ---------------- 对外 API ---------------- */
/* 上电调用：扫一遍扇区建立写指针，返回 1 = 读到可用快照 */
uint8_t flash_slot_init(void)
{
    fls_scan();
    return s_have;
}

/* 取最新快照：内容拷到 buf，长度/位置从出参返回；没快照返回 0 */
uint8_t flash_slot_load(uint8_t *buf, uint16_t cap, uint16_t *len,
                        uint32_t *pos_ms)
{
    if (s_scanned == 0u) {
        fls_scan();
    }
    if ((s_have == 0u) || (s_plen == 0u)) {
        return 0u;
    }
    if ((uint32_t)s_plen > (uint32_t)cap) {
        return 0u;
    }
    if (buf != 0) {
        memcpy(buf, s_pay, (size_t)s_plen);
    }
    if (len != 0) {
        *len = (uint16_t)s_plen;
    }
    if (pos_ms != 0) {
        *pos_ms = s_pos;
    }
    return 1u;
}

/* 追加一条快照；与最新一条逐字节相同（含 pos_ms）则不写 Flash */
uint8_t flash_slot_save(const void *data, uint16_t len, uint32_t pos_ms)
{
    const uint8_t *src = (const uint8_t *)data;

    if ((data == 0) || (len < FLS_DATA_MIN) || (len > FLS_DATA_MAX) ||
        ((len & 3u) != 0u)) {
        return FLS_ERR;
    }
    if (s_scanned == 0u) {
        fls_scan();
    }
    if ((s_have != 0u) && (s_plen == (uint8_t)len) && (s_pos == pos_ms) &&
        (memcmp(src, s_pay, (size_t)len) == 0)) {
        return FLS_UNCHANGED;
    }
    if ((s_off + FLS_ENT) > FLS_LIMIT) {
        if (fls_erase() == 0u) {
            return FLS_ERR;
        }
        s_rolls++;
    }
    if (fls_append(src, len, pos_ms) == 0u) {
        return FLS_ERR;
    }
    return FLS_OK;
}

/* 写指针已到条目区末尾：下次 save 前必须先阻塞擦一次扇区 */
uint8_t flash_slot_have_uncommitted(void)
{
    if (s_scanned == 0u) {
        fls_scan();
    }
    return ((s_off + FLS_ENT) > FLS_LIMIT) ? 1u : 0u;
}

/* 空闲时调用：提前滚动扇区，把约 1 s 的阻塞从播放中挪走 */
uint8_t flash_slot_prime(void)
{
    uint8_t  tmp[FLS_DATA_MAX];
    uint16_t l;
    uint32_t p;

    if (s_scanned == 0u) {
        fls_scan();
    }
    if ((s_off + FLS_ENT) <= FLS_LIMIT) {
        return FLS_OK;
    }
    l = (uint16_t)s_plen;
    p = s_pos;
    if (s_have != 0u) {
        memcpy(tmp, s_pay, (size_t)l);
    }
    if (fls_erase() == 0u) {
        return FLS_ERR;
    }
    s_rolls++;
    if (s_have != 0u) {
        if (fls_append(tmp, l, p) == 0u) {
            return FLS_ERR;
        }
    }
    return FLS_OK;
}

/* 上次擦/写失败后的恢复：作废缓存并重新扫描扇区 */
void flash_slot_note_failed(void)
{
    s_scanned = 0u;
    fls_scan();
}

/* ---------------- 诊断计数（供串口/上位机查询） ---------------- */
uint32_t flash_slot_erases(void)  { return s_erases; }
uint32_t flash_slot_writes(void)  { return s_writes; }
uint32_t flash_slot_rolls(void)   { return s_rolls; }
uint32_t flash_slot_entries(void) { return s_cnt; }
