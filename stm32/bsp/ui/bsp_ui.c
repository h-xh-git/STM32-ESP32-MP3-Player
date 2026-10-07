/*
 * bsp_ui.c - 2.4 寸 ST7789 面板（横版 320x240）的 UTF-8 文本渲染实现
 *
 * 一次画一个字形：先按 unicode 在已排序的字库里二分查找，再把 1 bpp 点阵
 * 展开成颜色缓冲（s_glyph），最后 lcd_blit() 整块贴到屏上。
 * 没有裁剪矩形：字形超出面板就按行列计数少画（右对齐文本正需要这个行为）。
 *
 * ui_text_scl() 是带缩放比例的通用实现，比例 num/den：1/1 = 普通 14 px 行盒，
 * 3/2 = 全屏歌词页当前行（1.5 倍最近邻），2/1 是备用整数倍放大。
 *
 * 依赖：bsp_lcd（lcd_blit）、ui_font_cn14.h（ui_f14_glyphs / ui_f14_bits）。
 * 调用者：bsp/ui/ui_player.c；对外接口只有 bsp_ui.h 里的四个函数。
 */
#include "bsp_ui.h"
#include "ui_font_cn14.h"

#define UI_GLYPH_MAX    16

/* 最大 2 倍放大时单字形 32x32 = 1024 项；1.5 倍只需 24x24 = 576 项 */
static uint16_t s_glyph[UI_GLYPH_MAX * UI_GLYPH_MAX * 4];

/* ui_f14_glyphs[] 按 unicode 升序排列，所以可以二分 */
static const ui_f14_glyph_t *ui_find(uint16_t uni)
{
    int lo = 0;
    int hi = (int)ui_f14_n - 1;

    while (lo <= hi)
    {
        int mid = (lo + hi) >> 1;
        uint16_t u = ui_f14_glyphs[mid].uni;

        if (u == uni) return &ui_f14_glyphs[mid];
        if (u < uni)  lo = mid + 1;
        else          hi = mid - 1;
    }
    return 0;
}

/* 解一个 UTF-8 码点并把 *ps 前移（4 字节序列与非法字节都当作 '?'） */
static uint16_t ui_utf8(const char **ps)
{
    const unsigned char *p = (const unsigned char *)(*ps);
    uint16_t cp;

    if (p[0] < 0x80u)
    {
        *ps = (const char *)(p + 1);
        return (uint16_t)p[0];
    }
    if (((p[0] & 0xE0u) == 0xC0u) && ((p[1] & 0xC0u) == 0x80u))
    {
        cp = (uint16_t)(((uint16_t)(p[0] & 0x1Fu) << 6) | (uint16_t)(p[1] & 0x3Fu));
        *ps = (const char *)(p + 2);
        return cp;
    }
    if (((p[0] & 0xF0u) == 0xE0u) &&
        ((p[1] & 0xC0u) == 0x80u) && ((p[2] & 0xC0u) == 0x80u))
    {
        cp = (uint16_t)(((uint16_t)(p[0] & 0x0Fu) << 12) |
                        ((uint16_t)(p[1] & 0x3Fu) << 6)  |
                         (uint16_t)(p[2] & 0x3Fu));
        *ps = (const char *)(p + 3);
        return cp;
    }
    *ps = (const char *)(p + 1);
    return (uint16_t)'?';
}

/* 通用放大渲染器：输出尺寸 = 源尺寸 * num / den（最近邻采样，整数运算）。
 * (1,1) 普通 14 px 行盒 / (3,2) 1.5 倍（全屏歌词页当前行）/ (2,1) 2 倍备用。
 * 每个输出像素都写 fg 或 bg，所以画出来的始终是不透明矩形块。 */
static void ui_text_scl(uint16_t x, uint16_t y, const char *s, uint16_t fg, uint16_t bg,
                        uint8_t num, uint8_t den)
{
    const char *p = s;

    while (*p != '\0')
    {
        const ui_f14_glyph_t *g = ui_find(ui_utf8(&p));
        uint16_t adv;

        if (g == 0)                     /* 字库里没有这个码点 */
        {
            x = (uint16_t)(x + (uint16_t)((uint32_t)7u * num / den));
            continue;
        }
        adv = (uint16_t)((uint32_t)g->adv * num / den);
        if (((uint8_t)g->w > UI_GLYPH_MAX) || ((uint8_t)g->h > UI_GLYPH_MAX))
        {
            x = (uint16_t)(x + adv);
            continue;
        }
        if ((g->w != 0u) && (g->h != 0u))
        {
            uint32_t stride = ((uint32_t)g->w + 7u) >> 3;
            int32_t  gx     = (int32_t)x + ((int32_t)g->ox * (int32_t)num) / (int32_t)den;
            int32_t  gy     = (int32_t)y + ((int32_t)g->oy * (int32_t)num) / (int32_t)den;
            uint16_t cols   = (uint16_t)((uint32_t)g->w * num / den);
            uint16_t rows   = (uint16_t)((uint32_t)g->h * num / den);
            uint16_t r0     = 0u;                     /* 屏幕顶部被截掉的首行 */
            uint16_t r, c;

            if ((cols == 0u) || (rows == 0u)) { x = (uint16_t)(x + adv); continue; }
            if (gx < 0) { x = (uint16_t)(x + adv); continue; }

            if (gy < 0)                       /* 字形顶部越出屏幕，裁掉上面几行 */
            {
                r0 = (uint16_t)(-gy);
                if (r0 >= rows) { x = (uint16_t)(x + adv); continue; }
                gy   = gy + (int32_t)r0;
                rows = (uint16_t)(rows - r0);
            }
            if ((gy + (int32_t)rows) > (int32_t)LCD_H)
                rows = (uint16_t)((int32_t)LCD_H - gy);
            if ((gx + (int32_t)cols) > (int32_t)LCD_W)
                cols = (uint16_t)((int32_t)LCD_W - gx);
            if ((rows == 0u) || (cols == 0u)) { x = (uint16_t)(x + adv); continue; }

            for (r = 0u; r < rows; r++)
            {
                uint32_t src = (uint32_t)(r0 + r) * den / num;   /* 最近邻：输出行 -> 源行 */

                for (c = 0u; c < cols; c++)
                {
                    uint32_t sx = (uint32_t)c * den / num;       /* 输出列 -> 源列 */
                    uint8_t  on = (uint8_t)(ui_f14_bits[(uint32_t)g->off + src * stride +
                                                        (sx >> 3)] &
                                            (uint8_t)(0x80u >> (sx & 7u)));
                    s_glyph[(uint16_t)(r * cols + c)] = on ? fg : bg;
                }
            }
            lcd_blit((uint16_t)gx, (uint16_t)gy, cols, rows, s_glyph);
        }
        x = (uint16_t)(x + adv);
    }
}

/* 常规 1 倍渲染 */
void ui_text(uint16_t x, uint16_t y, const char *s, uint16_t fg, uint16_t bg)
{
    ui_text_scl(x, y, s, fg, bg, 1u, 1u);
}

/* 1.5 倍放大（全屏歌词页的当前行）：y 是 22 px 放大行盒的顶部 */
void ui_text15(uint16_t x, uint16_t y, const char *s, uint16_t fg, uint16_t bg)
{
    ui_text_scl(x, y, s, fg, bg, 3u, 2u);
}

/* 单个字形的前进量也是各自取整，和 ui_text_scl 的步进严格一致 */
static uint16_t ui_text_wsc(const char *s, uint8_t num, uint8_t den)
{
    const char *p = s;
    uint16_t w = 0u;

    while (*p != '\0')
    {
        const ui_f14_glyph_t *g = ui_find(ui_utf8(&p));
        w = (uint16_t)(w + (uint16_t)((uint32_t)(g ? g->adv : 7u) * num / den));
    }
    return w;
}

/* 1 倍像素宽度 */
uint16_t ui_text_w(const char *s)
{
    return ui_text_wsc(s, 1u, 1u);
}

/* 1.5 倍像素宽度 */
uint16_t ui_text15_w(const char *s)
{
    return ui_text_wsc(s, 3u, 2u);
}
