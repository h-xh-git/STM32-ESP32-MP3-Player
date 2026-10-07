/*
 * cover.c —— 迷你 baseline JPEG 解码器：解析 MP3 内嵌 APIC / 同目录封面图
 *
 * 为什么要自己写：板上没有 framebuffer、内部 RAM 只有 128 KB 的 ZI 区，
 * 整帧照片（哪怕 500x500）解出来也要 500 KB RGB。这里的做法是
 *
 *   1) 不存整图：Huffman + 反量化 + 整数 IDCT 只算出 8x8 块的像素；
 *   2) 边解边盒式降采样：每个分量像素按 cover-fit（填满正方形、居中裁剪）
 *      归到 144x144 的某个格子里，累加和 + 计数放在 17 行的滑动窗口里；
 *   3) 一行格子攒满就按平均 -> 补洞 -> YCbCr->RGB565 写进 s_px（41 KB）；
 *   4) 解码完 s_px 就是可以直接 lcd_blit 的封面。
 *
 * 内存：s_px 41472 + 窗口 22302 + 表 ~3 KB ≈ 66 KB，全部静态，不用 malloc。
 * 时间：500x500 约 60 ms，1600x1600（上限）约 0.5 s。
 * 可断点续跑：cover_begin() 只定位源 + 解析到 SOS，剩下的 MCU 扫描由主循环
 * cover_poll(budget_ms) 分片推进，切歌时不会被封面解码挡住出声；
 * （cover_load() 是同步包装 = begin + 循环 poll(0)，语义等价于一次性解完）。
 *
 * 依赖：FatFS（读源文件）、bsp_tick（耗时统计）、bsp/ui/ui_player（消费 cover_pixels）。
 * 调用：app/main.c（cover_begin/cover_poll/cover_busy/cover_ready）。
 */

#include "cover.h"
#include "ff.h"
#include "bsp_tick.h"
#include <string.h>

#define CV_ACC_ROWS   18u          /* 累加窗口行数。JPEG 的 MCU 顺序是行主序，一个 MCU 行带
                                      （8*Vmax 个源行）里各列 MCU 的样本到达顺序被熵流锁死，
                                      所以整带放完之前不能 flush 掉这一带压到的输出行：
                                      窗口必须容得下 ceil((17*Vmax-2)*COVER_PX/(2*m)) + 2 行
                                      （Vmax=2、m=144 时正好 18 行；更小的源图在 cv_map_setup()
                                      里整张拒绝，界面保留占位图）。 */
#define CV_SRC_BUF    1024u        /* 源文件读缓冲 */
#define CV_HUFF_SYM   256u
#define CV_INF        0xFFFFFFFFu

/* ==================================================================
 * 常量表
 * ================================================================== */

/* zigzag 序 -> 自然序（JPEG 规范 Annex K） */
static const uint8_t s_zigzag[64] = {
     0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};

/* 1-D IDCT 矩阵：A[u][x] = round(0.5*C(u)*cos((2x+1)u*pi/16)*4096) */
static const int16_t s_idct[8][8] = {
    { 1448,  1448,  1448,  1448,  1448,  1448,  1448,  1448 },
    { 2009,  1703,  1138,   400,  -400, -1138, -1703, -2009 },
    { 1892,   784,  -784, -1892, -1892,  -784,   784,  1892 },
    { 1703,  -400, -2009, -1138,  1138,  2009,   400, -1703 },
    { 1448, -1448, -1448,  1448,  1448, -1448, -1448,  1448 },
    { 1138, -2009,   400,  1703, -1703,  -400,  2009, -1138 },
    {  784, -1892,  1892,  -784,  -784,  1892, -1892,   784 },
    {  400, -1138,  1703, -2009,  2009, -1703,  1138,  -400 }
};

/* ==================================================================
 * 源：FatFS 文件 + 1 KB 缓冲（可限定可读字节数，用于只吃 APIC 里的 JPEG）
 * ================================================================== */
static FIL      s_f;
static uint8_t  s_sbuf[CV_SRC_BUF];
static uint16_t s_slen;
static uint16_t s_spos;
static uint32_t s_sleft;           /* 还可读字节数，CV_INF = 读到文件尾 */
static uint8_t  s_sopen;

/* 关闭封面源文件并把源缓冲状态清零 */
static void src_close(void)
{
    if (s_sopen) { (void)f_close(&s_f); s_sopen = 0u; }
    s_slen = 0u; s_spos = 0u; s_sleft = CV_INF;
}

/* 打开封面源文件；成功 1，失败 0 */
static int src_open(const char *path)
{
    src_close();
    if (f_open(&s_f, path, FA_READ) != FR_OK) return 0;
    s_sopen = 1u; s_slen = 0u; s_spos = 0u; s_sleft = CV_INF;
    return 1;
}

/* 源文件定位到 off，并丢弃源缓冲里的剩余数据 */
static int src_seek(uint32_t off)
{
    if (!s_sopen) return 0;
    s_slen = 0u; s_spos = 0u;
    return (f_lseek(&s_f, (FSIZE_t)off) == FR_OK) ? 1 : 0;
}

/* 从源取 1 字节（自动续读缓冲）；源耗尽/读失败返回 -1 */
static int src_byte(void)
{
    UINT     br;
    uint32_t want;

    if (s_spos >= s_slen)
    {
        if (s_sleft == 0u) return -1;
        want = CV_SRC_BUF;
        if (s_sleft != CV_INF && s_sleft < want) want = s_sleft;
        br = 0u;
        if (f_read(&s_f, s_sbuf, want, &br) != FR_OK || br == 0u) return -1;
        if (s_sleft != CV_INF) s_sleft -= (uint32_t)br;
        s_slen = (uint16_t)br; s_spos = 0u;
    }
    return (int)s_sbuf[s_spos++];
}

/* 从源取 n 字节到 out；中途出错返回 0 */
static int src_bytes(uint8_t *out, uint32_t n)
{
    while (n--)
    {
        int b = src_byte();
        if (b < 0) return 0;
        *out++ = (uint8_t)b;
    }
    return 1;
}

/* 从源跳过 n 字节；中途出错返回 0 */
static int src_skip(uint32_t n)
{
    while (n--) { if (src_byte() < 0) return 0; }
    return 1;
}

/* ==================================================================
 * 位读取器（0xFF00 去填充、标记检测、RSTn 重启）
 * ================================================================== */
static uint32_t s_bitb;
static int      s_bitn;
static int      s_hashit;      /* 1 = 撞上真标记，位流结束 */
static int      s_rstpend;     /* 1 = 撞上的标记就是 RSTn */

static void bits_reset(void) { s_bitb = 0u; s_bitn = 0; s_hashit = 0; s_rstpend = 0; }

/* 位缓冲补 1 字节：处理 0xFF00 去填充，遇真标记则位流结束 */
static int bits_fill(void)
{
    int b, c;

    if (s_hashit) return 0;
    b = src_byte();
    if (b < 0) { s_hashit = 1; return 0; }
    if (b == 0xFF)
    {
        c = src_byte();
        if (c < 0) { s_hashit = 1; return 0; }
        if (c != 0x00)
        {
            s_hashit = 1;
            if (c >= 0xD0 && c <= 0xD7) s_rstpend = 1;
            return 0;
        }
    }
    s_bitb = (s_bitb << 8) | (uint32_t)b;
    s_bitn += 8;
    return 1;
}

/* 取 n 位（高位在前）；位流结束返回 0 */
static int bits_get(int n, uint32_t *out)
{
    while (s_bitn < n) { if (!bits_fill()) return 0; }
    *out = (s_bitb >> (s_bitn - n)) & ((1u << n) - 1u);
    s_bitn -= n;
    return 1;
}

/* 重启间隔后字节对齐并吃掉 RSTn */
static int bits_restart(void)
{
    int b, c;

    s_bitn = 0; s_hashit = 0;
    if (s_rstpend) { s_rstpend = 0; return 1; }
    for (;;)
    {
        b = src_byte();
        if (b < 0) return 0;
        if (b != 0xFF) continue;
        c = src_byte();
        if (c < 0) return 0;
        if (c == 0x00 || c == 0xFF) continue;
        if (c >= 0xD0 && c <= 0xD7) return 1;
        return 0;
    }
}

/* ==================================================================
 * Huffman 表
 * ================================================================== */
typedef struct
{
    uint16_t mincode[17];
    int32_t  maxcode[17];       /* -1 = 该长度没有码字 */
    uint16_t valptr[17];
    uint16_t nsym;
    uint8_t  sym[CV_HUFF_SYM];
} huff_t;

static huff_t s_huff[4];        /* 0/1 = DC0/DC1，2/3 = AC0/AC1 */

static int huff_build(huff_t *h, const uint8_t *cnt, const uint8_t *sym)
{
    uint32_t code = 0u, k = 0u;
    int l;

    h->mincode[0] = 0u;
    h->maxcode[0] = -1;
    h->valptr[0]  = 0u;
    for (l = 1; l <= 16; l++)
    {
        if (cnt[l - 1] != 0u)
        {
            h->valptr[l]  = (uint16_t)k;
            h->mincode[l] = (uint16_t)code;
            code += cnt[l - 1];
            h->maxcode[l] = (int32_t)(code - 1u);
            k += cnt[l - 1];
        }
        else
        {
            h->valptr[l]  = 0u;
            h->mincode[l] = 0u;
            h->maxcode[l] = -1;
        }
        /* 关键：每一级长度都要左移一位（即使这一级没有码字）。
           把 code <<= 1 放进上面的 if 里是经典错误：一旦某级长度为 0，
           之后所有长度的 mincode/maxcode 都会错位，Huffman 解出的
           符号和耗位数一起错 -> 位流失步。见 Annex C / libjpeg
           jpeg_make_d_derived_tbl()。 */
        if (code > (((uint32_t)1u << l) - 1u))
        {
            /* 出现全 1 码字 => 非法码表（libjpeg 同样报 JERR_BAD_HUFF_TABLE） */
            h->nsym = 0u;
            return 0;
        }
        code <<= 1;
    }
    if (k > CV_HUFF_SYM) k = CV_HUFF_SYM;
    if (k != 0u) memcpy(h->sym, sym, (size_t)k);
    h->nsym = (uint16_t)k;
    return 1;
}

/* 按标准 Huffman 表逐位解出一个符号 */
static int huff_decode(const huff_t *h, uint32_t *sym)
{
    uint32_t code = 0u, bit;
    int l;

    for (l = 1; l <= 16; l++)
    {
        if (!bits_get(1, &bit)) return 0;
        code = (code << 1) | bit;
        if (h->maxcode[l] >= 0 && (int32_t)code <= h->maxcode[l])
        {
            uint32_t idx = (uint32_t)h->valptr[l] + code - (uint32_t)h->mincode[l];
            if (idx >= (uint32_t)h->nsym) return 0;
            *sym = (uint32_t)h->sym[idx];
            return 1;
        }
    }
    return 0;
}

/* 接收 s 位并做符号扩展 */
static int32_t ext_val(int32_t v, int s)
{
    if (s == 0) return 0;
    if (v < (1 << (s - 1))) return v - (1 << s) + 1;
    return v;
}

/* ==================================================================
 * 解码状态
 * ================================================================== */
typedef struct
{
    uint8_t id;
    uint8_t h;
    uint8_t v;
    uint8_t tq;
    uint8_t td;
    uint8_t ta;
    uint8_t plane;          /* 累加平面：灰度=0；彩色 0=Y 1=Cb 2=Cr */
    int32_t pred;           /* DC 预测器 */
} cv_comp_t;

static cv_comp_t s_comp[3];
static uint8_t   s_ncomp;
static uint16_t  s_img_w;
static uint16_t  s_img_h;
static uint8_t   s_hmax;
static uint8_t   s_vmax;
static uint16_t  s_mcu_cols;
static uint16_t  s_mcu_rows;
static uint16_t  s_dri;
static uint16_t  s_mcu_seen;
static uint16_t  s_qt[4][64];       /* zigzag 序 */
static uint8_t   s_qt_ok[4];

/* ---------------- 输出 ---------------- */
/* 三大缓冲（64800 B）+ 上一行兜底（432 B）放在 CCM RAM（0x10000000，CPU 专线）：
   本工程没有任何外设 DMA（LCD/SDIO/UART 全是轮询），CCM 只被 CPU 访问，所以安全；
   搬走后主板 SRAM 只剩 ≈57 KB 占用（详见 project\MDK(V5)\Project_ccm.sct）。 */
#define CV_CCM __attribute__((section(".ccmram"), zero_init))
static uint16_t s_px[COVER_PX * COVER_PX]           CV_CCM;
static uint16_t s_acc[CV_ACC_ROWS][3][COVER_PX]     CV_CCM;  /* 求和 */
static uint8_t  s_accc[CV_ACC_ROWS][3][COVER_PX]    CV_CCM;  /* 计数 */
static uint8_t  s_prev[3][COVER_PX]                 CV_CCM;  /* 上一行（整行无样本时兜底） */
static uint8_t  s_prev_ok[3];

/* 编译期容量自检（防止 RAM 悄悄溢出）：上面四块必须塞得进 CCM 的 64 KB。
   s_px 2 B/格 + (s_acc 2 B + s_accc 1 B) x 3 分量 x CV_ACC_ROWS 行 + s_prev 1 B/格。
   超出时这里直接 #error，而不是等链接器报 RW_CCM region overflow。 */
#if ((COVER_PX * COVER_PX * 2u) + (CV_ACC_ROWS * 3u * COVER_PX * 3u) + (3u * COVER_PX)) > 0x10000u
#error "cover CCM buffers exceed the 64 KB RW_CCM (0x10000000) region"
#endif
static int16_t  s_win_g0;                             /* 窗口第 0 行对应的输出行 */
static uint8_t  s_win_n;                              /* 窗口内有效行数 */
static uint16_t s_map_m;                              /* min(W,H) */
static int16_t  s_map_x0;
static int16_t  s_map_y0;
static uint32_t s_stride = 1u;                        /* 大图抽样步长（防空格和 uint16/uint8 饱和） */

static uint8_t  s_ready;
static uint32_t s_drop;                               /* 窗口外被丢弃的样本数（应为 0） */

/* 分片（断点续跑）状态：cv_head() 成功后进入扫描，cv_scan_step() 从
   (s_step_my, s_step_mx) 所指的 MCU 继续，由 cover_poll() 驱动。 */
static uint16_t s_step_my;
static uint16_t s_step_mx;
static uint8_t  s_step_act;                            /* 1 = 有解码正在进行 */

/* 当前 MCU 的所有分量块暂存：3 分量 × (h*v <= 8) 块 × 64 px = 1536 B。
   为什么必须暂存：JPEG 的扫描顺序是「一个 MCU 内先 Y 块、再 Cb、再 Cr」，
   而累加窗口只能按输出行的顺序 flush。把块先解出来、再按全分辨率行序放置，
   窗口里就只会有 1~3 行待累计（否则一个 MCU 行带的 8*Vmax 个源行会让
   窗口同时挂住 ceil(8*Vmax*COVER_PX/m)+1 行，m 小于 144 时直接装不下）。 */
static uint8_t s_mcub[3][8][64];

/* cover-fit 映射：半单位（2*坐标+覆盖像素数）中心 -> 输出格 */
static int32_t cv_cellx(uint32_t half)
{
    return (int32_t)((half * COVER_PX) / (2u * (uint32_t)s_map_m));
}

/* 算 cover-fit 映射：源图坐标 -> 144x144 格子、居中裁剪偏移、采样步长（步长保证格子计数不溢出） */
static int cv_map_setup(void)
{
    uint32_t ncx = (2u * (uint32_t)(s_img_w - 1u) + 1u) * COVER_PX / (2u * (uint32_t)s_map_m) + 1u;
    uint32_t ncy = (2u * (uint32_t)(s_img_h - 1u) + 1u) * COVER_PX / (2u * (uint32_t)s_map_m) + 1u;

    s_map_x0 = (ncx > COVER_PX) ? (int16_t)((ncx - COVER_PX) / 2u) : 0;
    s_map_y0 = (ncy > COVER_PX) ? (int16_t)((ncy - COVER_PX) / 2u) : 0;

    /* 一格最多累计多少样本：沿一轴约为 m/(72*stride) + 1，取最小的 stride 让它的平方 <= 250。
       不然 s_acc（uint16 求和，上限 65535）与 s_accc（uint8 计数，上限 255）在大图上会饱和，
       平均值就不对了（m 大到 1100 以上时单格样本数超过 255）。 */
    for (s_stride = 1u; s_stride < 16u; s_stride++)
    {
        uint32_t q = (uint32_t)s_map_m / (72u * s_stride) + 1u;
        if (q * q <= 250u) break;
    }

    /* 窗口容量：整个 MCU 行带（8*Vmax 个源行）压住的输出行数必须 <= CV_ACC_ROWS，
       否则 acc_put() 会把同一带后面列的样本当"旧行"丢掉（画面缺块 + 糊色）。
       保守上界 ceil((17*Vmax-2)*COVER_PX/(2*m)) + 2；装不下就整张拒绝。 */
    if (((uint32_t)(17u * (uint32_t)s_vmax - 2u) * COVER_PX) / (2u * (uint32_t)s_map_m) + 2u
            > CV_ACC_ROWS)
    {
        return 0;
    }
    return 1;
}

/* ---------------- 累加窗口 ---------------- */
static void win_clear(void)
{
    memset(s_acc, 0, sizeof(s_acc));
    memset(s_accc, 0, sizeof(s_accc));
    memset(s_prev, 0, sizeof(s_prev));
    s_prev_ok[0] = s_prev_ok[1] = s_prev_ok[2] = 0u;
    s_win_g0 = 0;
    s_win_n = 0u;
}

/* 把一个分量样本累加进对应输出格子；被裁剪或被滑动窗口挤掉的样本直接丢弃 */
static void acc_put(uint8_t p, uint32_t halfx, uint32_t halfy, uint8_t val)
{
    int32_t ox = cv_cellx(halfx) - (int32_t)s_map_x0;
    int32_t oy = cv_cellx(halfy) - (int32_t)s_map_y0;
    int32_t idx;

    if (ox < 0 || ox >= (int32_t)COVER_PX) return;   /* 裁剪掉的列：正常 */
    if (oy < 0 || oy >= (int32_t)COVER_PX) return;   /* 裁剪掉的行：正常 */
    idx = oy - (int32_t)s_win_g0;
    if (idx < 0 || idx >= (int32_t)CV_ACC_ROWS)
    {
        s_drop++;                                    /* 窗口算错才会走到这里，供上板自查 */
        return;
    }
    if (idx >= (int32_t)s_win_n) s_win_n = (uint8_t)(idx + 1);

    s_acc[(uint16_t)idx][p][(uint16_t)ox] += val;
    if (s_accc[(uint16_t)idx][p][(uint16_t)ox] != 255u) s_accc[(uint16_t)idx][p][(uint16_t)ox]++;
}

/* 某平面一行的 144 个格子：有样本取平均，空洞用最近的左值补，整行空则沿用上一行 */
static void fill_plane(uint8_t p, uint8_t idx, uint8_t *dst)
{
    uint16_t c;
    int16_t  last = -1;
    uint8_t  any = 0u;

    for (c = 0u; c < COVER_PX; c++)
    {
        if (s_accc[idx][p][c] != 0u)
        {
            dst[c] = (uint8_t)(s_acc[idx][p][c] / s_accc[idx][p][c]);
            any = 1u;
            last = (int16_t)c;
        }
        else
        {
            dst[c] = (last >= 0) ? dst[last] : 0u;
        }
    }
    if (any == 0u)
    {
        if (s_prev_ok[p]) memcpy(dst, s_prev[p], COVER_PX);
        else              memset(dst, 0, COVER_PX);
    }
    else if (s_accc[idx][p][0] == 0u)
    {
        uint16_t f = 0u;
        while (f < COVER_PX && s_accc[idx][p][f] == 0u) f++;
        for (c = 0u; c < f; c++) dst[c] = dst[f];
    }
    memcpy(s_prev[p], dst, COVER_PX);
    s_prev_ok[p] = 1u;
}

/* 输出第 idx 行格子：分量取平均 -> 补洞 -> YCbCr 转 RGB565 写进 s_px */
static void emit_row(uint8_t idx, int16_t oy)
{
    uint8_t  yv[COVER_PX], cb[COVER_PX], cr[COVER_PX];
    uint16_t c;

    if (oy < 0 || oy >= (int16_t)COVER_PX) return;

    if (s_ncomp == 1u)
    {
        fill_plane(0u, idx, yv);
        for (c = 0u; c < COVER_PX; c++)
        {
            uint8_t v = yv[c];
            s_px[(uint32_t)oy * COVER_PX + c] =
                (uint16_t)(((uint16_t)(v & 0xF8u) << 8) | ((uint16_t)(v & 0xFCu) << 3) | (uint16_t)(v >> 3));
        }
    }
    else
    {
        fill_plane(0u, idx, yv);
        fill_plane(1u, idx, cb);
        fill_plane(2u, idx, cr);
        for (c = 0u; c < COVER_PX; c++)
        {
            int32_t y = (int32_t)yv[c];
            int32_t b = (int32_t)cb[c] - 128;
            int32_t r = (int32_t)cr[c] - 128;
            int32_t rr = y + ((91881 * r) >> 16);
            int32_t gg = y - ((22554 * b + 46802 * r) >> 16);
            int32_t bb = y + ((116130 * b) >> 16);
            if (rr < 0) rr = 0; else if (rr > 255) rr = 255;
            if (gg < 0) gg = 0; else if (gg > 255) gg = 255;
            if (bb < 0) bb = 0; else if (bb > 255) bb = 255;
            s_px[(uint32_t)oy * COVER_PX + c] =
                (uint16_t)(((uint16_t)rr & 0xF8u) << 8) |
                (uint16_t)(((uint16_t)gg & 0xFCu) << 3) |
                (uint16_t)((uint16_t)bb >> 3);
        }
    }
}

/* 输出所有 g < 目标行 的窗口行，并把它腾出来。
   没有样本的行也照样 emit：fill_plane() 的计数全 0 时会自动沿用上一行，
   这样小图放大（映射只用到前面 ncy 行）、以及整行被 stride 抽样跳过的
   情况都不会在右下角留黑边；同时 s_win_g0 永远跟得上 g，
   下一行样本的 idx = oy - s_win_g0 就一直很小，窗口不会被算爆。 */
static void win_flush_before(int32_t g)
{
    uint8_t last;

    while ((int32_t)s_win_g0 < g)
    {
        emit_row(0u, s_win_g0);
        if (s_win_n > 0u)
        {
            if (s_win_n > 1u)
            {
                memmove(&s_acc[0][0][0], &s_acc[1][0][0], (size_t)(s_win_n - 1u) * sizeof(s_acc[0]));
                memmove(&s_accc[0][0],   &s_accc[1][0],    (size_t)(s_win_n - 1u) * sizeof(s_accc[0]));
            }
            last = (uint8_t)(s_win_n - 1u);
            memset(&s_acc[last][0][0], 0, sizeof(s_acc[last]));
            memset(&s_accc[last][0], 0, sizeof(s_accc[last]));
            s_win_n--;
        }
        s_win_g0++;
    }
}

/* ==================================================================
 * 块解码
 * ================================================================== */

/* 可分离整数 IDCT：行 -> 列，每级 (sum+2048)>>12，最后 +128 电平偏移 */
static void idct_8x8(const int32_t *coef, uint8_t *out)
{
    int32_t tmp[64];
    int     u, v, x, y;

    for (y = 0; y < 8; y++)
    {
        const int32_t *row = &coef[y * 8];
        for (x = 0; x < 8; x++)
        {
            int64_t sum = 0;
            for (u = 0; u < 8; u++) sum += (int64_t)row[u] * (int64_t)s_idct[u][x];
            tmp[y * 8 + x] = (int32_t)((sum + 2048) >> 12);
        }
    }
    for (x = 0; x < 8; x++)
    {
        for (y = 0; y < 8; y++)
        {
            int64_t sum = 0;
            for (v = 0; v < 8; v++) sum += (int64_t)tmp[v * 8 + x] * (int64_t)s_idct[v][y];
            sum = ((sum + 2048) >> 12) + 128;
            if (sum < 0) sum = 0;
            if (sum > 255) sum = 255;
            out[y * 8 + x] = (uint8_t)sum;
        }
    }
}

/* 解一个 8x8 块：DC 差分 + AC 游程/Huffman -> 反量化 -> 整数 IDCT */
static int cv_decode_block(cv_comp_t *cp, uint8_t *out)
{
    int32_t  coef[64];
    uint32_t sym, bits;
    int      k, s, r;

    if (!huff_decode(&s_huff[cp->td], &sym)) return 0;
    s = (int)sym;
    if (s > 11) return 0;
    if (s > 0)
    {
        int32_t diff;
        if (!bits_get(s, &bits)) return 0;
        diff = ext_val((int32_t)bits, s);
        cp->pred += diff;
    }
    for (k = 0; k < 64; k++) coef[k] = 0;
    coef[0] = cp->pred * (int32_t)s_qt[cp->tq][0];

    k = 1;
    while (k < 64)
    {
        if (!huff_decode(&s_huff[2u + cp->ta], &sym)) return 0;
        r = (int)(sym >> 4);
        s = (int)(sym & 15u);
        if (s == 0)
        {
            if (r != 15) break;                 /* EOB */
            k += 16;
        }
        else
        {
            k += r;
            if (k > 63) return 0;
            if (!bits_get(s, &bits)) return 0;
            coef[s_zigzag[k]] = ext_val((int32_t)bits, s) * (int32_t)s_qt[cp->tq][k];
            k++;
        }
    }
    idct_8x8(coef, out);
    return 1;
}

/* 把「全分辨率行 fy」上属于某分量的样本放进累加窗口。
   块已经解好暂存在 s_mcub 里（见 s_mcub 的注释），这里只做映射。 */
static void cv_place_row(const cv_comp_t *cp, uint32_t fy, uint16_t mx)
{
    uint8_t  sx = (uint8_t)(s_hmax / cp->h);   /* 该分量像素覆盖的全分辨率像素数 */
    uint8_t  sy = (uint8_t)(s_vmax / cp->v);
    uint32_t cu, bi, r;
    uint8_t  hx, c;

    if ((fy % (uint32_t)s_stride) != 0u) return;   /* 大图抽样：整行跳过 */
    if ((fy % (uint32_t)sy) != 0u) return;         /* 该分量在这一行没有样本起点 */
    cu = fy / (uint32_t)sy;                        /* 分量自己的行号 */
    bi = (cu / 8u) % (uint32_t)cp->v;              /* 本 MCU 内的块行 */
    r  = cu % 8u;                                  /* 块内第几行 */

    for (hx = 0u; hx < cp->h; hx++)
    {
        uint32_t       bx = (uint32_t)(mx * cp->h + hx) * 8u * (uint32_t)sx;
        const uint8_t *blk = s_mcub[cp->plane][(uint8_t)(bi * (uint32_t)cp->h + hx)];

        for (c = 0u; c < 8u; c++)
        {
            uint32_t fx = bx + (uint32_t)c;
            if (fx >= (uint32_t)s_img_w) break;
            if ((fx % (uint32_t)s_stride) != 0u) continue;
            acc_put(cp->plane, 2u * fx + (uint32_t)sx, 2u * fy + (uint32_t)sy,
                    blk[(uint16_t)r * 8u + c]);
        }
    }
}

/* ==================================================================
 * 段解析
 * ================================================================== */
static int cv_read_len(uint32_t *len)
{
    uint8_t b[2];
    if (!src_bytes(b, 2u)) return 0;
    *len = ((uint32_t)b[0] << 8) | (uint32_t)b[1];
    return (*len >= 2u) ? 1 : 0;
}

/* 跳过熵数据（含 0xFF00）找下一个真正的标记，标记号写入 *m */
static int cv_next_marker(uint8_t *m)
{
    int b, c;

    for (;;)
    {
        b = src_byte();
        if (b < 0) return 0;
        if (b != 0xFF) continue;
        do { c = src_byte(); if (c < 0) return 0; } while (c == 0xFF);
        if (c == 0x00) continue;                 /* 数据里的 0xFF00 */
        *m = (uint8_t)c;
        return 1;
    }
}

/* 解析 SOF0/SOF1 帧头：图像尺寸与各分量采样系数 */
static int cv_sof(void)
{
    uint32_t len;
    uint8_t  b[6];
    uint16_t i;

    if (!cv_read_len(&len)) return 0;
    if (len < 8u || len > 8u + 3u * 3u) return 0;
    if (!src_bytes(b, 6u)) return 0;
    if (b[0] != 8u) return 0;                                  /* 只支持 8bit */
    s_img_h = (uint16_t)(((uint16_t)b[1] << 8) | (uint16_t)b[2]);
    s_img_w = (uint16_t)(((uint16_t)b[3] << 8) | (uint16_t)b[4]);
    s_ncomp = b[5];
    if (s_img_w == 0u || s_img_h == 0u) return 0;
    if (s_ncomp != 1u && s_ncomp != 3u) return 0;
    if (len != (uint32_t)(8u + 3u * s_ncomp)) return 0;
    if (s_img_w > (uint16_t)COVER_MAX_DIM || s_img_h > (uint16_t)COVER_MAX_DIM) return 0;

    for (i = 0u; i < s_ncomp; i++)
    {
        uint8_t c[3];
        if (!src_bytes(c, 3u)) return 0;
        s_comp[i].id    = c[0];
        s_comp[i].h     = (uint8_t)(c[1] >> 4);
        s_comp[i].v     = (uint8_t)(c[1] & 15u);
        s_comp[i].tq    = (uint8_t)(c[2] & 3u);
        s_comp[i].pred  = 0;
        s_comp[i].plane = (uint8_t)i;
        if (s_comp[i].h < 1u || s_comp[i].h > 4u) return 0;
        if (s_comp[i].v < 1u || s_comp[i].v > 2u) return 0;     /* Vmax <= 2（内存） */
        if (s_comp[i].tq > 3u) return 0;
        if (s_hmax < s_comp[i].h) s_hmax = s_comp[i].h;
        if (s_vmax < s_comp[i].v) s_vmax = s_comp[i].v;
    }
    for (i = 0u; i < s_ncomp; i++)
    {
        if ((s_hmax % s_comp[i].h) != 0u) return 0;
        if ((s_vmax % s_comp[i].v) != 0u) return 0;
    }
    if (s_ncomp == 3u)
    {
        /* JFIF 假定：第 0 个分量是 Y 且按 Hmax/Vmax 全采样 */
        if (s_comp[0].h != s_hmax || s_comp[0].v != s_vmax) return 0;
    }
    s_map_m    = (s_img_w < s_img_h) ? s_img_w : s_img_h;
    s_mcu_cols = (uint16_t)((s_img_w + 8u * s_hmax - 1u) / (8u * s_hmax));
    s_mcu_rows = (uint16_t)((s_img_h + 8u * s_vmax - 1u) / (8u * s_vmax));
    if (!cv_map_setup()) return 0;
    return 1;
}

/* 解析 DQT 量化表 */
static int cv_dqt(void)
{
    uint32_t len, used = 2u;
    uint8_t  pq_tq, v;
    uint16_t i;

    if (!cv_read_len(&len)) return 0;
    while (used < len)
    {
        uint8_t pq, tq;
        if (used + 65u > len) return 0;
        if (!src_bytes(&pq_tq, 1u)) return 0;
        pq = (uint8_t)(pq_tq >> 4);
        tq = (uint8_t)(pq_tq & 15u);
        if (pq != 0u || tq > 3u) return 0;                     /* 只支持 8bit 量化 */
        for (i = 0u; i < 64u; i++)
        {
            if (!src_bytes(&v, 1u)) return 0;
            s_qt[tq][i] = (uint16_t)v;
        }
        s_qt_ok[tq] = 1u;
        used += 65u;
    }
    return 1;
}

/* 解析 DHT Huffman 表（DC/AC 各最多 4 张） */
static int cv_dht(void)
{
    uint32_t len, used = 2u;

    if (!cv_read_len(&len)) return 0;
    while (used < len)
    {
        uint8_t  tt, cnt[16], sym[CV_HUFF_SYM];
        uint16_t nsym = 0u, i;
        uint8_t  tc, th;

        if (!src_bytes(&tt, 1u)) return 0;
        if (!src_bytes(cnt, 16u)) return 0;
        for (i = 0u; i < 16u; i++) nsym = (uint16_t)(nsym + cnt[i]);
        if (nsym == 0u || nsym > CV_HUFF_SYM) return 0;
        if (!src_bytes(sym, nsym)) return 0;
        tc = (uint8_t)(tt >> 4);
        th = (uint8_t)(tt & 15u);
        if (tc > 1u || th > 1u) return 0;
        if (!huff_build(&s_huff[(tc == 0u) ? th : (uint8_t)(2u + th)], cnt, sym)) return 0;
        used += (uint32_t)(1u + 16u + nsym);
    }
    return 1;
}

/* 解析 DRI 重启间隔 */
static int cv_dri(void)
{
    uint32_t len;
    uint8_t  b[2];

    if (!cv_read_len(&len)) return 0;
    if (len != 4u) return 0;
    if (!src_bytes(b, 2u)) return 0;
    s_dri = (uint16_t)(((uint16_t)b[0] << 8) | (uint16_t)b[1]);
    return 1;
}

/* 解析 SOS 扫描头：分量与 DC/AC 表选择 */
static int cv_sos(void)
{
    uint32_t len;
    uint8_t  nb, c[2];
    uint16_t ns, i, k;

    if (!cv_read_len(&len)) return 0;
    if (!src_bytes(&nb, 1u)) return 0;
    ns = nb;
    if (ns != (uint16_t)s_ncomp) return 0;
    if (len != (uint32_t)(6u + 2u * ns)) return 0;
    for (i = 0u; i < ns; i++)
    {
        if (!src_bytes(c, 2u)) return 0;
        for (k = 0u; k < s_ncomp; k++) { if (s_comp[k].id == c[0]) break; }
        if (k >= s_ncomp) return 0;
        s_comp[k].td = (uint8_t)(c[1] >> 4);
        s_comp[k].ta = (uint8_t)(c[1] & 15u);
        if (s_comp[k].td > 1u || s_comp[k].ta > 1u) return 0;
        if (s_qt_ok[s_comp[k].tq] == 0u) return 0;              /* 量化表必须先出现 */
    }
    if (!src_skip(3u)) return 0;                                /* Ss, Se, Ah/Al */
    for (k = 0u; k < s_ncomp; k++) s_comp[k].pred = 0;
    s_mcu_seen = 0u;
    bits_reset();
    return 1;
}

/* 按 MCU 逐个推进扫描解码，可以停在任意 MCU 边界（切歌不被封面解码挡住）。
 * budget_ms = 0 表示不限时间（一次解完）；否则最多跑 budget_ms
 * 毫秒，但至少解完一个 MCU 才可能返回 2，保证每次调用都有进度。
 * 返回 2 = 还没解完（断点保存在 s_step_my/s_step_mx，下次接着来）；1 = 全图完成；0 = 数据错误。 */
static int cv_scan_step(uint32_t budget_ms)
{
    uint32_t t0 = tick_ms();

    while (s_step_my < s_mcu_rows)
    {
        uint32_t band_y0 = (uint32_t)s_step_my * 8u * (uint32_t)s_vmax;
        uint16_t mx = s_step_mx;
        uint16_t my = s_step_my;
        uint16_t ci;
        uint8_t  hx, vy, u;

        /* 0) 整带开头的 flush：本带所有样本的 oy 都 >= cellx(2*band_y0+1)-y0，
              低于它的输出行已经不会再收到样本了。注意要减 s_map_y0（裁剪偏移），
              acc_put() 里的 oy 是减过偏移的输出行号。 */
        if (mx == 0u)
            win_flush_before(cv_cellx(2u * band_y0 + 1u) - (int32_t)s_map_y0);

        /* 1) 先把本 MCU 的所有分量块解出来暂存（位流顺序就是 Y -> Cb -> Cr） */
        for (ci = 0u; ci < s_ncomp; ci++)
        {
            cv_comp_t *cp = &s_comp[ci];
            for (vy = 0u; vy < cp->v; vy++)
            {
                for (hx = 0u; hx < cp->h; hx++)
                {
                    if (!cv_decode_block(cp, s_mcub[cp->plane][(uint8_t)(vy * cp->h + hx)])) return 0;
                }
            }
        }
        /* 2) 再按全分辨率行序放置本带的样本（不能在这里 flush：同一带的
              其它列 MCU 还没解出来，行还没收全）。 */
        for (u = 0u; u < (uint8_t)(8u * s_vmax); u++)
        {
            uint32_t fy = band_y0 + (uint32_t)u;
            if (fy >= (uint32_t)s_img_h) break;
            for (ci = 0u; ci < s_ncomp; ci++) cv_place_row(&s_comp[ci], fy, mx);
        }
        if (s_dri != 0u)
        {
            s_mcu_seen++;
            if (s_mcu_seen >= s_dri)
            {
                s_mcu_seen = 0u;
                /* 整张图的最后一格之后不一定有 RSTn（Pillow 之类只在行间写标记，末尾直接跟 EOI），
                   所以最后一格不强求重启标记。 */
                if (!((my == (uint16_t)(s_mcu_rows - 1u)) && (mx == (uint16_t)(s_mcu_cols - 1u))))
                {
                    if (!bits_restart()) return 0;
                    s_comp[0].pred = 0; s_comp[1].pred = 0; s_comp[2].pred = 0;
                }
            }
        }

        /* 3) 断点推进：本格已完成，记到下一格（跨行时行号前进、列号归零） */
        if (++s_step_mx >= s_mcu_cols)
        {
            s_step_mx = 0u;
            s_step_my++;
        }

        /* 4) 时间片用完就带着断点退出（上面至少解完了一格，不会空转） */
        if (budget_ms != 0u && s_step_my < s_mcu_rows &&
            (uint32_t)(tick_ms() - t0) >= budget_ms)
            return 2;
    }

    win_flush_before((int32_t)COVER_PX);
    return 1;
}

/* 解析到 SOS：清状态 -> 逐段解析（cv_sof 里会调 cv_map_setup）-> 复位累加窗口与断点。
   返回 1 = 头部长成、可以开始 cv_scan_step()；0 = 不是能解的 baseline JPEG。 */
static int cv_head(void)
{
    uint8_t m;

    s_ncomp = 0u; s_hmax = 0u; s_vmax = 0u; s_dri = 0u; s_mcu_seen = 0u;
    s_qt_ok[0] = s_qt_ok[1] = s_qt_ok[2] = s_qt_ok[3] = 0u;
    memset(s_huff, 0, sizeof(s_huff));
    memset(s_px, 0, sizeof(s_px));
    bits_reset();

    for (;;)
    {
        uint32_t len;
        if (!cv_next_marker(&m)) return 0;

        if (m == 0xD8u) continue;                               /* SOI */
        if (m == 0xD9u) return 0;                               /* EOI 来得太早 */
        if (m == 0xC0u || m == 0xC1u) { if (!cv_sof()) return 0; continue; }
        if (m == 0xC4u) { if (!cv_dht()) return 0; continue; }
        if (m == 0xDBu) { if (!cv_dqt()) return 0; continue; }
        if (m == 0xDDu) { if (!cv_dri()) return 0; continue; }
        if (m == 0xDAu)
        {
            if (!cv_sos()) return 0;
            win_clear();
            s_step_my = 0u;
            s_step_mx = 0u;
            return 1;
        }
        if ((m >= 0xD0u && m <= 0xD7u) || m == 0x01u) continue;  /* 独立标记 */
        if (m == 0xC2u || m == 0xC3u || m == 0xC9u) return 0;    /* 渐进 / 无损 / 算术 */
        if (m >= 0xC5u && m <= 0xCFu) return 0;
        if (!cv_read_len(&len)) return 0;                        /* 其余带长度的段跳过 */
        if (!src_skip(len - 2u)) return 0;
    }
}


/* ==================================================================
 * 来源 1：MP3 内嵌 ID3v2 APIC
 * ================================================================== */
static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* 读 ID3v2 的同步安全整数（4 字节，各取低 7 位） */
static uint32_t ss32(const uint8_t *p)
{
    return ((uint32_t)(p[0] & 0x7Fu) << 21) | ((uint32_t)(p[1] & 0x7Fu) << 14) |
           ((uint32_t)(p[2] & 0x7Fu) << 7)  | (uint32_t)(p[3] & 0x7Fu);
}

/* 在 ID3v2 标签里找 APIC 封面帧，命中则回填数据偏移与长度 */
static int find_apic(const char *path, uint32_t *off, uint32_t *len)
{
    uint8_t  hdr[10], id[10], hb[4];
    uint32_t tag_size, tag_end, cur, frame_size, n;
    uint8_t  ver;
    int      found = 0;

    if (!src_open(path)) return 0;
    if (!src_bytes(hdr, 10u)) { src_close(); return 0; }
    if (hdr[0] != 'I' || hdr[1] != 'D' || hdr[2] != '3') { src_close(); return 0; }
    ver = hdr[3];
    if (ver < 2u || ver > 4u) { src_close(); return 0; }
    if ((hdr[5] & 0x80u) != 0u) { src_close(); return 0; }       /* unsynchronisation 放弃 */

    tag_size = ((uint32_t)(hdr[6] & 0x7Fu) << 21) | ((uint32_t)(hdr[7] & 0x7Fu) << 14) |
               ((uint32_t)(hdr[8] & 0x7Fu) << 7)  | (uint32_t)(hdr[9] & 0x7Fu);
    tag_end = 10u + tag_size;
    if ((hdr[5] & 0x10u) != 0u) tag_end += 10u;                  /* footer */
    cur = 10u;

    if ((hdr[5] & 0x40u) != 0u)                                  /* 扩展头 */
    {
        int32_t ext;
        if (!src_bytes(hb, 4u)) { src_close(); return 0; }
        ext = (ver == 4u) ? (int32_t)ss32(hb) : (int32_t)be32(hb);
        if (ext < 0) { src_close(); return 0; }
        cur += (ver == 4u) ? (uint32_t)ext : (uint32_t)(4 + ext);
        if (cur >= tag_end) { src_close(); return 0; }
        if (!src_skip((ver == 4u) ? (uint32_t)(ext - 4) : (uint32_t)ext)) { src_close(); return 0; }
    }

    while (cur + (ver == 2u ? 6u : 10u) <= tag_end)
    {
        uint32_t need = (ver == 2u) ? 6u : 10u;
        if (!src_bytes(id, need)) break;
        cur += need;

        if (ver == 2u)
        {
            frame_size = ((uint32_t)id[3] << 16) | ((uint32_t)id[4] << 8) | (uint32_t)id[5];
        }
        else if (ver == 3u)
        {
            frame_size = be32(id + 4);
        }
        else
        {
            frame_size = ss32(id + 4);
        }
        if (frame_size == 0u || frame_size > tag_end - cur) break;

        if (!((id[0] == 'A' && id[1] == 'P' && id[2] == 'I' && id[3] == 'C' && ver >= 3u) ||
              (ver == 2u && id[0] == 'P' && id[1] == 'I' && id[2] == 'C')))
        {
            if (!src_skip(frame_size)) break;
            cur += frame_size;
            continue;
        }

        /* 帧内：encoding(1) + MIME(0 结束) + picture type(1) + description(0 结束) + 图像数据 */
        {
            uint8_t  head[256];
            uint32_t m = (frame_size < (uint32_t)sizeof(head)) ? frame_size : (uint32_t)sizeof(head);
            uint32_t i;
            uint8_t  enc;

            if (!src_bytes(head, m)) break;
            enc = head[0];
            if (ver == 2u)
            {
                i = 4u;                                           /* PIC: 3 字节图像格式(JPG/PNG) */
            }
            else
            {
                i = 1u;
                while (i < m && head[i] != 0u) i++;               /* MIME, Latin-1 */
                if (i >= m) break;
                i++;                                              /* 跳过 MIME 的 0 */
            }
            if (i >= m) break;
            i++;                                                  /* picture type */
            if (enc == 1u || enc == 2u)                           /* UTF-16 描述: 双字节 0 */
            {
                while (i + 1u < m && !(head[i] == 0u && head[i + 1u] == 0u)) i += 2u;
                i += 2u;
            }
            else
            {
                while (i < m && head[i] != 0u) i++;
                i++;
            }
            if (i + 1u >= frame_size) break;
            n = frame_size - i;
            if (i + 1u < m && head[i] == 0xFFu && head[i + 1u] == 0xD8u)
            {
                *off = cur + i;
                *len = n;
                found = 1;
            }
        }
        break;                                                    /* 只认第一个 APIC */
    }

    src_close();
    return found;
}

/* ==================================================================
 * 来源 2：同目录 sidecar 图片
 * ================================================================== */
static int sidecar_try(const char *mp3)
{
    static const char *suf[8] = { "cover.jpg", "cover.JPG", "cover.jpeg", "cover.JPEG",
                                  "folder.jpg", "folder.JPG", "folder.jpeg", "folder.JPEG" };
    static const char *ext[4] = { ".jpg", ".JPG", ".jpeg", ".JPEG" };
    char        dir[128];
    char        base[80];
    char        path[224];
    const char *sl;
    const char *dot;
    uint32_t    dl, bl;
    uint8_t     k;

    sl = strrchr(mp3, '/');
    if (sl != 0)
    {
        dl = (uint32_t)(sl - mp3) + 1u;
        if (dl >= sizeof(dir)) return 0;
        memcpy(dir, mp3, dl);
        dir[dl] = 0;
    }
    else
    {
        dir[0] = 0;
        dl = 0u;
    }
    sl  = (sl != 0) ? (sl + 1) : mp3;
    dot = strrchr(sl, '.');
    bl  = (dot != 0) ? (uint32_t)(dot - sl) : (uint32_t)strlen(sl);
    if (bl >= sizeof(base)) bl = sizeof(base) - 1u;
    memcpy(base, sl, bl);
    base[bl] = 0;

    /* cover.* / folder.* 优先 */
    for (k = 0u; k < 8u; k++)
    {
        if (dl + (uint32_t)strlen(suf[k]) + 1u > sizeof(path)) continue;
        strcpy(path, dir);
        strcat(path, suf[k]);
        if (src_open(path)) return 1;
    }
    /* <同名>.jpg / .jpeg */
    for (k = 0u; k < 4u; k++)
    {
        if (dl + bl + (uint32_t)strlen(ext[k]) + 1u > sizeof(path)) continue;
        strcpy(path, dir);
        strcat(path, base);
        strcat(path, ext[k]);
        if (src_open(path)) return 1;
    }
    return 0;
}

/* ==================================================================
 * 对外接口
 * ================================================================== */
void cover_init(void)
{
    s_ready    = 0u;
    s_img_w    = 0u;
    s_img_h    = 0u;
    s_ncomp    = 0u;
    s_hmax     = 0u;
    s_vmax     = 0u;
    s_win_g0   = 0;
    s_win_n    = 0u;
    s_prev_ok[0] = s_prev_ok[1] = s_prev_ok[2] = 0u;
    memset(s_px, 0, sizeof(s_px));
    src_close();
}

/* 异步第一步：定位源 + 解析到 SOS（不做 MCU 扫描）。
   返回 1 = 有封面正在解，之后靠 cover_poll() 推进；0 = 没有可用封面（界面留占位图）。 */
uint8_t cover_begin(const char *mp3_path)
{
    uint32_t off = 0u, len = 0u;

    src_close();                 /* 上一次没跑完的解码（或已用完的源）先关掉 */
    s_step_act = 0u;
    s_step_my  = 0u;
    s_step_mx  = 0u;
    s_ready    = 0u;
    s_img_w    = 0u;
    s_img_h    = 0u;

    if (mp3_path == 0) return 0u;

    /* 1) MP3 内嵌 APIC */
    if (find_apic(mp3_path, &off, &len))
    {
        if (src_open(mp3_path) && src_seek(off))
        {
            s_sleft = len;                    /* 只吃 APIC 里那段 JPEG */
            if (cv_head())
            {
                s_step_act = 1u;
                return 1u;
            }
        }
        src_close();
    }

    /* 2) 同目录 sidecar */
    if (sidecar_try(mp3_path))
    {
        s_sleft = CV_INF;
        if (cv_head())
        {
            s_step_act = 1u;
            return 1u;
        }
        src_close();
    }

    return 0u;
}

/* 异步第二步：把分片解码往前推 budget_ms 毫秒（0 = 一次解完）。
   返回 0 = 还没解完；1 = 解完且成功（s_px 可用）；2 = 解完但失败 / 当前没有在解。 */
uint8_t cover_poll(uint32_t budget_ms)
{
    int r;

    if (s_step_act == 0u) return 2u;

    r = cv_scan_step(budget_ms);
    if (r == 2) return 0u;                    /* 断点已保存，下次接着来 */

    src_close();
    s_step_act = 0u;
    s_ready    = (uint8_t)((r == 1) ? 1u : 0u);
    return (uint8_t)(s_ready ? 1u : 2u);
}

uint8_t cover_busy(void) { return s_step_act; }

/* 同步包装：阻塞到解完，供不便于分片推进的调用点使用（返回 1 = 成功，0 = 无封面/失败）。 */
uint8_t cover_load(const char *mp3_path)
{
    if (cover_begin(mp3_path) == 0u) return 0u;

    for (;;)
    {
        uint8_t r = cover_poll(0u);
        if (r == 1u) return 1u;
        if (r == 2u) return 0u;
    }
}

uint8_t         cover_ready(void)  { return s_ready; }
const uint16_t *cover_pixels(void) { return s_px; }
