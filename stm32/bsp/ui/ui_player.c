/*
 * ui_player.c - 主界面 / 歌单浮层 / 全屏歌词页的绘制与增量刷新
 *
 * 三块画面互斥占用整屏：
 *   主界面  状态栏(0..25: 播放/暂停圆点 + 音量 + SD + 模式) + 封面 144x144
 *           + 信息区(歌名/歌手/序号/歌词 4 行/进度条/时间) + 控制栏(192..239)
 *   歌单浮层 y 26..239，6 行条目 + 头(曲目数) + 脚(按键提示)
 *   歌词页  整屏，5 行视窗（当前行 1.5x 放大居中高亮）+ 顶部歌名 + 底部进度/时间/音量
 * 页间切换用「背光淡出 -> 全黑那一帧换画面 -> 淡入」过渡（page_fade_poll()），
 * 页内刷新则尽量只重画变化的小矩形（脏缓存 + s_blk 有效位），不整屏重画。
 *
 * 依赖：bsp_lcd（矩形/像素绘制、lcd_blit、背光）、bsp_ui（ui_text/ui_text15）、
 *       lyrics.c（lrc_*）、cover.c（cover_ready/cover_pixels）、playlist.h、ui_player.h 的状态结构。
 * 调用者：app/main.c —— ui_player_update() 在界面帧循环里调用，按键分发调用
 *         ui_player_list_* / ui_player_lyrics_* / ui_player_vol_flash() / ui_player_cover_refresh()。
 */
#include "ui_player.h"
#include "bsp_ui.h"
#include "bsp_lcd.h"
#include "lyrics.h"
#include "cover.h"
#include <string.h>
#include <stdio.h>

/* ==================================================================
 * 配色（取自设计稿 CSS 变量）
 * ================================================================== */
#define CLR_BG      RGB565(0x0B,0x0E,0x14)
#define CLR_SF      RGB565(0x15,0x1A,0x23)
#define CLR_CARD    RGB565(0x1C,0x23,0x33)
#define CLR_BD      RGB565(0x26,0x30,0x45)
#define CLR_T1      RGB565(0xE8,0xED,0xF4)
#define CLR_T2      RGB565(0x7A,0x8B,0xA6)
#define CLR_PRI     RGB565(0x4F,0xC3,0xF7)
#define CLR_ACC     RGB565(0xFF,0xD5,0x4F)
#define CLR_OK      RGB565(0x66,0xBB,0x6A)

/* ==================================================================
 * 版面（顶部状态栏 + 封面 + 信息区 + 控制栏）
 * ================================================================== */
#define TITLE_H     26                      /* 0..25  状态栏 */
#define CB_H        48
#define CB_Y        (LCD_H - CB_H)          /* 192 */
#define MAIN_Y      TITLE_H                 /* 26  */
#define MAIN_H      (CB_Y - MAIN_Y)         /* 166 */

#define COVER_S     144
#define COVER_X     8
#define COVER_Y     (uint16_t)(MAIN_Y + (MAIN_H - COVER_S) / 2)   /* 37 */
#define INFO_X      (uint16_t)(COVER_X + COVER_S + 8)             /* 160 */
#define INFO_W      (uint16_t)(LCD_W - INFO_X - 8)                /* 152 */

/* 信息区内部（相对 COVER_Y） */
#define IN_TITLE    0     /* 歌名（标题行） */
#define IN_ARTIST   21    /* 歌手 / 大小 + 序号 */
#define IN_LYRIC    42    /* 歌词首行 */
#define LYRIC_H     17
#define LYRIC_PAD   4     /* 歌词底板内缩 */
#define LYRIC_BG_Y  (IN_LYRIC - LYRIC_PAD)      /* 38 -> y75  */
#define LYRIC_BG_H  (LYRIC_H * 4u + LYRIC_PAD)  /* 72 -> y75..146 */
#define IN_PROG     115   /* 进度条 */
#define PROG_H      6
#define IN_TIME     127   /* 时间 */

/* 控制栏按钮 */
#define BTN_Y       (uint16_t)(CB_Y + 8)    /* 200 */
#define SM_W        30
#define MD_W        38
#define LG_W        52
#define BTN_H2      32
#define GAP         3

/* 控制栏里的音量数字左边界（与 draw_controls 的排版一致） */
#define CB_VOL_X    (uint16_t)(8u + (MD_W + GAP) + (LG_W + GAP) + (MD_W + GAP) + 30u)   /* = 175 */

/* 侧边音量条 */
#define SV_W        24

/* ==================================================================
 * 状态缓存
 * ================================================================== */
static uint8_t  s_playing = 0xFFu;
static uint8_t  s_mode    = 0xFFu;
static uint8_t  s_volume  = 0xFFu;
static uint8_t  s_sd_ok   = 0xFFu;
static uint16_t s_index   = 0xFFFFu;
static uint16_t s_total   = 0xFFFFu;
static uint32_t s_total_ms = 0xFFFFFFFFu;   /* 曲目总时长变化 -> 信息区(总时长/进度刻度)整块重画 */
static uint16_t s_list_n  = 0xFFFFu;
static uint32_t s_pos_s   = 0xFFFFFFFFu;
static char     s_title[80];
static uint8_t  s_have_title;
static uint8_t  s_vol_show;
static uint32_t s_vol_t0;
static char     s_st_vol[8]   = "";      /* 已画出的状态栏音量数字（变化时只擦那一小块） */
static char     s_cb_vol[8]   = "";      /* 已画出的控制栏音量数字 */
static char     s_sv_txt[8]   = "";      /* 已画出的侧边音量条数字 */
static uint16_t s_sv_fh       = 0u;      /* 已画出的侧边音量条填充高 */
static uint16_t s_pr_fw       = 0u;      /* 已画出的主界面进度条填充宽（增量刷新用） */
static char     s_pr_tstr[12] = "";      /* 已画出的主界面当前时间文本 */
static int16_t  s_lyric_idx = -1;   /* 上次绘制的歌词行号（-1 = 尚未确定） */

/* 主界面四块的"屏幕上已画好"有效位（valid）: 1 = 该块内容与增量缓存一致, 可以局部增量刷新。
   被歌单浮层/全屏歌词页盖住时清零, 退出时按覆盖范围局部恢复并重新置位。
   任一块无效时, 对应的增量函数直接放弃, 等整块重画（ui_player_full() 仍是整屏兜底）。 */
#define BLK_STATUS  0x01u
#define BLK_COVER   0x02u
#define BLK_INFO    0x04u
#define BLK_CTRL    0x08u
#define BLK_ALL     0x0Fu
static uint8_t  s_blk = 0u;

/* ---------------- 切页背光淡变 ----------------
   切页时先把背光淡下去, 在最暗的那一帧才做整块重画, 再淡回来。
   单程 60 ms => 一次切页约 120 ms, 短到不拖沓、又足够盖住画面突变。 */
#define PG_FADE_MS       60u
#define PG_ACT_NONE      0u
#define PG_ACT_LIST_ON   1u     /* 打开歌单浮层 */
#define PG_ACT_LIST_OFF  2u     /* 关闭歌单浮层 */
#define PG_ACT_LY_ON     3u     /* 进入全屏歌词页 */
#define PG_ACT_LY_OFF    4u     /* 退出全屏歌词页 */
static uint8_t  s_pg_act     = PG_ACT_NONE;   /* 正在进行的切页动作 */
static uint8_t  s_pg_in      = 0u;            /* 0 = 正在淡出, 1 = 正在淡入 */
static uint8_t  s_pg_started = 0u;            /* 首帧取时间基准用 */
static uint32_t s_pg_t0      = 0u;

/* 歌词 4 行脏刷新缓存: 文本或有无变化时才擦+重画该行 */
static char    s_lyr[4][LRC_LINE_LEN];
static uint8_t s_lyr_on[4];

/* 歌单浮层 */
static uint8_t  s_pl_open;
static uint16_t s_pl_sel;
static uint16_t s_pl_first;

/* ---------------- 全屏歌词页 ----------------
   视窗固定 5 行：上 2 + 当前行 + 下 2，当前行在 LY_CUR_ROW 槽位 = 屏幕中间。
   当前行用 ui_text15() 放大 1.5 倍（22 px 行盒）并高亮，其余行 1x。 */
#define LY_HEAD     30u                                        /* 顶部歌名条 */
#define LY_FOOT     32u                                        /* 底部进度/时间/返回提示 */
#define LY_SLOT     30u                                        /* 每行高度：1.5x 字（22 px）刚好放得下 */
#define LY_NROW     5u
#define LY_CUR_ROW  2u
#define LY_BIG      22u                                        /* 当前行放大行盒高：15 * 3/2 */
#define LY_VOL_X    52u                                        /* 页脚音量读数左边界（左时间之后、中间提示之前） */
#define LY_Y0       (uint16_t)(LY_HEAD + (((LCD_H - LY_HEAD - LY_FOOT) - (LY_NROW * LY_SLOT)) / 2u))

static uint8_t  s_lyr_open;                     /* 1 = 正在显示全屏歌词 */
static int16_t  s_ly2_cur = -2;                 /* 当前行缓存（-2 = 尚未画过） */
static char     s_ly2[LY_NROW][LRC_LINE_LEN];   /* 已画出的 5 行文本 */
static uint8_t  s_ly2_on[LY_NROW];
static char     s_ly2_title[80];
static uint8_t  s_ly2_have_title;
static uint16_t s_ly2_index = 0xFFFFu;
static uint16_t s_ly2_total = 0xFFFFu;
static uint16_t s_ly2_lrcn  = 0xFFFFu;
static uint32_t s_ly2_s     = 0xFFFFFFFFu;
static uint16_t s_ly2_fw    = 0u;        /* 已画出的进度条内宽（只补新增像素，不整块重画） */
static char     s_ly2_tstr[12] = "";     /* 已画出的左侧时间文本 */
static uint8_t  s_ly2_vol    = 0xFFu;    /* 已画出的页脚音量值 */
static uint16_t s_ly2_vol_w  = 0u;       /* 已画出的音量读数宽（数字位数变化时把旧宽度也擦掉） */
static uint8_t  s_ly2_vol_hl = 0u;       /* 1 = 刚调过音量，读数用高亮底（1.5 s 后恢复常规色） */
static uint32_t s_ly2_vol_t  = 0u;       /* 音量变化时刻 */

/* 前向声明：全屏歌词页的 ly_refresh() 在 cache_title() 定义之前就要用它 */
static void cache_title(const ui_player_state_t *st);

/* ==================================================================
 * 绘图小工具
 * ================================================================== */
/* 画实心矩形（宽或高为 0 时什么都不画） */
static void bar(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t c)
{
    if ((w != 0u) && (h != 0u)) lcd_fill_rect(x, y, w, h, c);
}

/* 画 1 px 矩形边框 */
static void frame(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t c)
{
    lcd_draw_rect(x, y, w, h, c);
}

/* 右向三角形（下一曲箭头）：逐列缩短的竖线拼出 */
static void tri_right(uint16_t x, uint16_t y, uint16_t h, uint16_t c)
{
    uint16_t i;
    for (i = 0u; i < (uint16_t)(h / 2u + 1u); i++)
        lcd_draw_vline((uint16_t)(x + i), (uint16_t)(y + i), (uint16_t)(h - 2u * i), c);
}

/* 左向三角形（上一曲箭头） */
static void tri_left(uint16_t x, uint16_t y, uint16_t h, uint16_t c)
{
    uint16_t i;
    for (i = 0u; i < (uint16_t)(h / 2u + 1u); i++)
        lcd_draw_vline((uint16_t)(x - i), (uint16_t)(y + i), (uint16_t)(h - 2u * i), c);
}

/* 暂停图标：两条 3 px 竖条 */
static void pause_bars(uint16_t x, uint16_t y, uint16_t h, uint16_t c)
{
    bar(x, y, 3u, h, c);
    bar((uint16_t)(x + 7u), y, 3u, h, c);
}

/* 实心圆（音乐符号的头） */
static void fill_circle(uint16_t cx, uint16_t cy, uint16_t r, uint16_t c)
{
    int16_t dy;
    for (dy = -(int16_t)r; dy <= (int16_t)r; dy++)
    {
        int32_t v  = (int32_t)r * (int32_t)r - (int32_t)dy * (int32_t)dy;
        int16_t dx = 0;
        while (((int32_t)(dx + 1) * (int32_t)(dx + 1)) <= v) dx++;
        lcd_draw_hline((uint16_t)((int16_t)cx - dx), (uint16_t)((int16_t)cy + dy),
                       (uint16_t)(2 * dx + 1), c);
    }
}

/* 圆环轮廓（封面上的旋转环） */
static void ring(uint16_t cx, uint16_t cy, uint16_t r, uint16_t c)
{
    int16_t dy;
    for (dy = -(int16_t)r; dy <= (int16_t)r; dy++)
    {
        int32_t v  = (int32_t)r * (int32_t)r - (int32_t)dy * (int32_t)dy;
        int16_t dx = 0;
        while (((int32_t)(dx + 1) * (int32_t)(dx + 1)) <= v) dx++;
        lcd_draw_pixel((uint16_t)((int16_t)cx - dx), (uint16_t)((int16_t)cy + dy), c);
        lcd_draw_pixel((uint16_t)((int16_t)cx + dx), (uint16_t)((int16_t)cy + dy), c);
    }
}

/* 按像素宽度截断 UTF-8，超宽追加 … */
static void fit_text(const char *src, char *dst, uint32_t dstsz, uint16_t maxw)
{
    uint32_t o = 0u;
    uint16_t w = 0u;
    const char *p = src;

    if ((src == 0) || (dstsz < 8u)) { if (dstsz != 0u) dst[0] = '\0'; return; }

    while (*p != '\0')
    {
        uint8_t n = 1u;
        uint16_t gw;
        char tmp[8];

        if ((((unsigned char)p[0] & 0xE0u) == 0xC0u) && p[1]) n = 2u;
        else if ((((unsigned char)p[0] & 0xF0u) == 0xE0u) && p[1] && p[2]) n = 3u;

        if ((o + n + 4u) >= dstsz) break;

        memcpy(tmp, p, n); tmp[n] = '\0';
        gw = ui_text_w(tmp);
        if ((uint16_t)(w + gw) > maxw) break;

        memcpy(&dst[o], p, n);
        o += n;
        w = (uint16_t)(w + gw);
        p += n;
    }
    dst[o] = '\0';

    if ((*p != '\0') && ((o + 3u) < dstsz))
    {
        dst[o] = (char)0xE2; dst[o + 1] = (char)0x80; dst[o + 2] = (char)0xA6;
        dst[o + 3] = '\0';
    }
}

/* 画字符串（s 为 NULL 时什么都不画） */
static void text_at(uint16_t x, uint16_t y, const char *s, uint16_t fg, uint16_t bg)
{
    if (s != 0) ui_text(x, y, s, fg, bg);
}

/* 在 [x, x+w) 内水平居中画字符串；串比区域还宽时退化为左对齐 */
static void text_center(uint16_t x, uint16_t w, uint16_t y, const char *s,
                        uint16_t fg, uint16_t bg)
{
    uint16_t tw;
    if (s == 0) return;
    tw = ui_text_w(s);
    if (tw >= w) text_at(x, y, s, fg, bg);
    else         text_at((uint16_t)(x + (w - tw) / 2u), y, s, fg, bg);
}

/* 播放模式的中文名：0 顺序(列表循环) / 1 单曲 / 2 随机 */
static const char *mode_str(uint8_t m)
{
    if (m == 1u) return "\345\215\225\346\233\262";
    if (m == 2u) return "\351\232\217\346\234\272";
    return "\351\241\272\345\272\217";   /* 顺序（列表循环）：避免与旁边 [列表] 键重名 */
}

/* 音量数字固定字段宽（"100" 的像素宽）：
   draw_status() 里 SD/模式的位置从它算起 => 不随音量位数跳动；
   draw_status_vol() 也只需要擦这一个固定方块（否则数字变宽时会盖掉 "SD" 的尾巴）。 */
static uint16_t vol_field_w(void) { return ui_text_w("100"); }

/* ==================================================================
 * 歌名（顶部居中）
 * ================================================================== */
static void draw_status(const ui_player_state_t *st)
{
    char buf[16];
    uint16_t y = 5u;

    bar(0u, 0u, LCD_W, TITLE_H, CLR_BG);

    /* 左：脉冲圆点 + 播放/暂停 */
    {
        uint16_t c = (st->playing != 0u) ? CLR_PRI : CLR_T2;
        fill_circle(11u, (uint16_t)(TITLE_H / 2u), 4u, c);
        text_at(21u, y, (st->playing != 0u) ? "\346\222\255\346\224\276" : "\346\232\202\345\201\234", c, CLR_BG);
    }

    /* 右：音量 / SD / 播放模式（不含歌名） */
    {
        uint16_t x = (uint16_t)(LCD_W - 8u);
        uint16_t tw;
        const char *ms = mode_str(st->mode);

        sprintf(buf, "%u", (unsigned)st->volume);
        tw = ui_text_w(buf);
        text_at((uint16_t)(x - tw), y, buf, CLR_ACC, CLR_BG);   /* 右对齐 = LCD_W-8 */
        strncpy(s_st_vol, buf, sizeof(s_st_vol) - 1u);
        s_st_vol[sizeof(s_st_vol) - 1u] = '\0';

        x = (uint16_t)(x - vol_field_w());    /* 固定字段：SD/模式位置与位数无关 */
        x = (uint16_t)(x - 10u);
        tw = ui_text_w("SD");
        x = (uint16_t)(x - tw);
        text_at(x, y, "SD", (st->sd_ok != 0u) ? CLR_OK : CLR_T2, CLR_BG);

        x = (uint16_t)(x - 10u);
        tw = ui_text_w(ms);
        x = (uint16_t)(x - tw);
        text_at(x, y, ms, CLR_PRI, CLR_BG);
    }

    s_blk |= BLK_STATUS;      /* 状态栏整条已画好: 之后音量数字可增量 */
}

/* 状态栏音量数字：只擦"旧值/新值并集"那一小块（最多 21x15）再重画,
   不重画整条状态栏 —— 旋钮调音量时顶部不再整条闪 */
static void draw_status_vol(const ui_player_state_t *st)
{
    char     buf[8];
    uint16_t y  = 5u;
    uint16_t wn;
    uint16_t fw = vol_field_w();       /* 固定方块宽（与 draw_status 的排版一致） */

    if ((s_blk & BLK_STATUS) == 0u) return;   /* 状态栏无效：等整块重画，别在浮层/歌词页上乱画 */

    sprintf(buf, "%u", (unsigned)st->volume);
    if (strcmp(buf, s_st_vol) == 0) return;

    wn = ui_text_w(buf);
    bar((uint16_t)(LCD_W - 8u - fw), y, fw, 15u, CLR_BG);   /* 只擦固定方块，不碰 SD/模式 */
    text_at((uint16_t)(LCD_W - 8u - wn), y, buf, CLR_ACC, CLR_BG);
    strncpy(s_st_vol, buf, sizeof(s_st_vol) - 1u);
    s_st_vol[sizeof(s_st_vol) - 1u] = '\0';
}

/* ==================================================================
 * 封面
 * ================================================================== */
static void draw_cover(void)
{
    uint16_t cx = (uint16_t)(COVER_X + COVER_S / 2u);
    uint16_t cy = (uint16_t)(COVER_Y + COVER_S / 2u);
    uint16_t card = CLR_CARD;

    bar(COVER_X, COVER_Y, COVER_S, COVER_S, card);
    frame(COVER_X, COVER_Y, COVER_S, COVER_S, CLR_BD);

    if (cover_ready())
    {
        /* 真实封面：144x144 RGB565 一次 lcd_blit 写完（会盖掉描边），之后补回一圈边框 */
        lcd_blit(COVER_X, COVER_Y, COVER_S, COVER_S, cover_pixels());
        frame(COVER_X, COVER_Y, COVER_S, COVER_S, CLR_BD);
    }
    else
    {
        /* 占位图：唱片盘面 + 两道沟槽环 */
        fill_circle(cx, cy, 46u, RGB565(0x12,0x18,0x22));
        ring(cx, cy, 46u, RGB565(0x1E,0x3A,0x4A));
        ring(cx, cy, 36u, RGB565(0x18,0x2A,0x38));

        /* 音符：头 + 杆 + 旗 */
        fill_circle((uint16_t)(cx - 6u), (uint16_t)(cy + 14u), 9u, CLR_PRI);
        bar((uint16_t)(cx + 3u), (uint16_t)(cy - 18u), 4u, 32u, CLR_PRI);
        bar((uint16_t)(cx + 3u), (uint16_t)(cy - 18u), 14u, 4u, CLR_PRI);
    }

    s_blk |= BLK_COVER;       /* 封面整块已画好: 侧边音量条可增量 */
}

/* ==================================================================
 * 歌词滚动
 * ================================================================== */

/* 当前行号：前奏(无匹配)时预先停在第 1 行，与设计稿一致 */
static int16_t lyric_cur(uint32_t pos_ms)
{
    int16_t i = lrc_index(pos_ms);
    if ((i < 0) && (lrc_count() != 0u)) i = 0;
    return i;
}

/* 生成 4 行歌词文本(上一行/当前行/下一行/再下一行), on[r]=1 表示该行有字 */
static void lyric_rows(int16_t cur, char out[4][LRC_LINE_LEN], uint8_t on[4])
{
    uint16_t n = lrc_count();
    uint8_t  r;

    for (r = 0u; r < 4u; r++)
    {
        const char *src = 0;

        out[r][0] = '\0';
        on[r]     = 0u;

        if (n == 0u)
        {
            if (r == 1u)      src = "\346\232\202\346\227\240\346\255\214\350\257\215";                   /* 暂无歌词 */
            else if (r == 2u) src = "\350\257\267\346\254\243\350\265\217\347\272\257\351\237\263\344\271\220"; /* 请欣赏纯音乐 */
        }
        else
        {
            int16_t li = (int16_t)(cur - 1 + (int16_t)r);
            if ((li >= 0) && (li < (int16_t)n)) src = lrc_line((uint16_t)li);
        }

        if ((src != 0) && (*src != '\0'))
        {
            strncpy(out[r], src, LRC_LINE_LEN - 1u);
            out[r][LRC_LINE_LEN - 1u] = '\0';
            on[r] = 1u;
        }
    }
}

/* 歌词 4 行脏刷新: 只擦+重画内容变化的行(full=1 时先铺整块底板) */
static void draw_lyric_rows(uint16_t y0, int16_t cur, uint8_t full)
{
    char    nt[4][LRC_LINE_LEN];
    uint8_t non[4];
    char    lb[LRC_LINE_LEN];
    uint8_t r;

    if (full != 0u)
        bar(INFO_X, (uint16_t)(y0 + LYRIC_BG_Y), INFO_W, LYRIC_BG_H, CLR_SF);

    lyric_rows(cur, nt, non);

    for (r = 0u; r < 4u; r++)
    {
        if ((full == 0u) && (non[r] == s_lyr_on[r]) && (strcmp(nt[r], s_lyr[r]) == 0u))
            continue;

        {
            uint16_t yy = (uint16_t)(y0 + IN_LYRIC + (uint16_t)r * LYRIC_H);

            bar(INFO_X, yy, INFO_W, LYRIC_H, CLR_SF);
            if (non[r] != 0u)
            {
                fit_text(nt[r], lb, sizeof(lb), INFO_W);
                text_center(INFO_X, INFO_W, yy, lb, (r == 1u) ? CLR_PRI : CLR_T2, CLR_SF);
            }
        }

        strncpy(s_lyr[r], nt[r], LRC_LINE_LEN - 1u);
        s_lyr[r][LRC_LINE_LEN - 1u] = '\0';
        s_lyr_on[r] = non[r];
    }
}

/* ==================================================================
 * 信息区
 * ================================================================== */
static void draw_info(const ui_player_state_t *st)
{
    char buf[80];
    uint16_t y0 = COVER_Y;

    bar(INFO_X, y0, INFO_W, COVER_S, CLR_BG);

    /* 歌名（设计稿：信息区第一行标题；状态栏不再显示歌名） */
    {
        char nm[96];
        fit_text(s_have_title ? s_title : "--", nm, sizeof(nm), INFO_W);
        text_at(INFO_X, (uint16_t)(y0 + IN_TITLE), nm, CLR_T1, CLR_BG);
    }

    /* 歌手 | 序号 */
    {
        char nm[96];
        fit_text((st->artist != 0) ? st->artist : "", nm, sizeof(nm), (uint16_t)(INFO_W - 50u));
        text_at(INFO_X, (uint16_t)(y0 + IN_ARTIST), nm, CLR_T2, CLR_BG);
    }
    if (st->total != 0u)
        sprintf(buf, "%u/%u", (unsigned)st->index, (unsigned)st->total);
    else
        strcpy(buf, "-/-");
    {
        uint16_t tw = ui_text_w(buf);
        text_at((uint16_t)(INFO_X + INFO_W - tw), (uint16_t)(y0 + IN_ARTIST), buf, CLR_T2, CLR_BG);
    }

    /* 歌词：底板 + 4 行滚动（.lrc；无歌词时显示占位文案） */
    draw_lyric_rows(y0, lyric_cur(st->pos_ms), 1u);

    /* 进度条 */
    bar(INFO_X, (uint16_t)(y0 + IN_PROG), INFO_W, PROG_H, CLR_SF);
    frame(INFO_X, (uint16_t)(y0 + IN_PROG), INFO_W, PROG_H, CLR_BD);
    s_pr_fw = 0u;
    if ((st->total_ms != 0u) && (st->pos_ms <= st->total_ms))
    {
        uint16_t fw = (uint16_t)((uint32_t)INFO_W * st->pos_ms / st->total_ms);
        if (fw > INFO_W) fw = INFO_W;
        s_pr_fw = fw;
        bar(INFO_X, (uint16_t)(y0 + IN_PROG + 1u), fw, (uint16_t)(PROG_H - 2u), CLR_PRI);
    }

    /* 时间：左当前 / 右总时长 */
    {
        uint32_t t = st->pos_ms / 1000u;
        sprintf(buf, "%lu:%02lu", (unsigned long)(t / 60u), (unsigned long)(t % 60u));
    }
    text_at(INFO_X, (uint16_t)(y0 + IN_TIME), buf, CLR_T2, CLR_BG);
    strncpy(s_pr_tstr, buf, sizeof(s_pr_tstr) - 1u);
    s_pr_tstr[sizeof(s_pr_tstr) - 1u] = '\0';
    if (st->total_ms != 0u)
    {
        uint32_t t = st->total_ms / 1000u;
        sprintf(buf, "%lu:%02lu", (unsigned long)(t / 60u), (unsigned long)(t % 60u));
    }
    else strcpy(buf, "--:--");
    {
        uint16_t tw = ui_text_w(buf);
        text_at((uint16_t)(INFO_X + INFO_W - tw), (uint16_t)(y0 + IN_TIME), buf, CLR_T2, CLR_BG);
    }

    s_blk |= BLK_INFO;        /* 信息区整块已画好: 进度条/当前时间可增量 */
}

/* 主界面进度条：只补"新增的那一段"像素，不铺底板、不重画轨道边框 => 不闪。
   几何与 draw_info 完全一致：轨道 INFO_X..INFO_X+INFO_W-1（6 px 高），
   填充从左边框列开始画，高 PROG_H-2 */
static void draw_progress_delta(const ui_player_state_t *st)
{
    uint16_t y  = (uint16_t)(COVER_Y + IN_PROG);
    uint16_t fw = 0u;

    if ((s_blk & BLK_INFO) == 0u) return;     /* 信息区无效：等整块重画 */

    if ((st->total_ms != 0u) && (st->pos_ms <= st->total_ms))
    {
        fw = (uint16_t)((uint32_t)INFO_W * st->pos_ms / st->total_ms);
        if (fw > INFO_W) fw = INFO_W;
    }

    if (fw > s_pr_fw)          /* 前进：只画新增的一小段 */
    {
        bar((uint16_t)(INFO_X + s_pr_fw), (uint16_t)(y + 1u),
            (uint16_t)(fw - s_pr_fw), (uint16_t)(PROG_H - 2u), CLR_PRI);
    }
    else if (fw < s_pr_fw)     /* 后退 / 换曲：只擦掉多出来的那一小段 */
    {
        bar((uint16_t)(INFO_X + fw), (uint16_t)(y + 1u),
            (uint16_t)(s_pr_fw - fw), (uint16_t)(PROG_H - 2u), CLR_SF);
        /* 填充会盖住左/右边框列, 擦完后按"是否仍被填充覆盖"复原 */
        if (fw == 0u)    bar(INFO_X, (uint16_t)(y + 1u), 1u, (uint16_t)(PROG_H - 2u), CLR_BD);
        if (fw < INFO_W) bar((uint16_t)(INFO_X + INFO_W - 1u), (uint16_t)(y + 1u),
                             1u, (uint16_t)(PROG_H - 2u), CLR_BD);
    }
    s_pr_fw = fw;
}

/* 当前时间：只有文本真的变了才擦那一小块（max(旧,新) 宽 x 15）重画 */
static void draw_progress_time(const ui_player_state_t *st)
{
    char     buf[12];
    uint16_t wo, wn, w;
    uint32_t t = st->pos_ms / 1000u;

    if ((s_blk & BLK_INFO) == 0u) return;     /* 信息区无效：等整块重画 */

    sprintf(buf, "%lu:%02lu", (unsigned long)(t / 60u), (unsigned long)(t % 60u));
    if (strcmp(buf, s_pr_tstr) == 0) return;

    wo = ui_text_w(s_pr_tstr);
    wn = ui_text_w(buf);
    w  = (wo > wn) ? wo : wn;
    bar(INFO_X, (uint16_t)(COVER_Y + IN_TIME), w, 15u, CLR_BG);
    text_at(INFO_X, (uint16_t)(COVER_Y + IN_TIME), buf, CLR_T2, CLR_BG);
    strncpy(s_pr_tstr, buf, sizeof(s_pr_tstr) - 1u);
    s_pr_tstr[sizeof(s_pr_tstr) - 1u] = '\0';
}

/* ==================================================================
 * 控制栏：[⏮][▶][⏭] 音量 | [歌词][模式][歌单]
 * ================================================================== */
static void draw_controls(const ui_player_state_t *st)
{
    uint16_t x;
    uint16_t h = 18u;
    uint16_t cy = (uint16_t)(BTN_Y + (BTN_H2 - h) / 2u);

    bar(0u, CB_Y, LCD_W, CB_H, CLR_SF);
    bar(0u, CB_Y, LCD_W, 1u, CLR_BD);

    x = 8u;

    /* 上一曲 (K2) */
    frame(x, BTN_Y, MD_W, BTN_H2, CLR_BD);
    tri_left((uint16_t)(x + 24u), cy, h, CLR_T1);
    tri_left((uint16_t)(x + 15u), cy, h, CLR_T1);
    x = (uint16_t)(x + MD_W + GAP);

    /* 播放 / 暂停（K1 / 旋钮按下） */
    bar(x, BTN_Y, LG_W, BTN_H2, CLR_PRI);
    if (st->playing != 0u)
    {
        tri_right((uint16_t)(x + 22u), cy, h, CLR_BG);
    }
    else
    {
        pause_bars((uint16_t)(x + 21u), cy, h, CLR_BG);
    }
    x = (uint16_t)(x + LG_W + GAP);

    /* 下一曲 (K3) */
    frame(x, BTN_Y, MD_W, BTN_H2, CLR_BD);
    tri_right((uint16_t)(x + 14u), cy, h, CLR_T1);
    tri_right((uint16_t)(x + 23u), cy, h, CLR_T1);
    x = (uint16_t)(x + MD_W + GAP);

    /* 音量：旋钮旋转 */
    {
        char vb[8];
        sprintf(vb, "%u", (unsigned)st->volume);
        text_at(x, (uint16_t)(BTN_Y + 8u), "\351\237\263\351\207\217", CLR_T2, CLR_SF);
        text_at((uint16_t)(x + 30u), (uint16_t)(BTN_Y + 8u), vb, CLR_ACC, CLR_SF);
        strncpy(s_cb_vol, vb, sizeof(s_cb_vol) - 1u);
        s_cb_vol[sizeof(s_cb_vol) - 1u] = '\0';
    }

    /* 右侧：[歌词](旋钮按下 PD12) + 模式(K5) + 歌单(K4)
       42 px 三个按钮放不下(音量数字会挤到)，所以三个按钮都收窄到 36 px、间距 2 px */
    {
        uint16_t rw = 36u;
        uint16_t rg = 2u;
        uint16_t rx = (uint16_t)(LCD_W - 8u - (rw * 3u + rg * 2u));

        frame(rx, BTN_Y, rw, BTN_H2, CLR_ACC);
        text_center(rx, rw, (uint16_t)(BTN_Y + 8u), "\346\255\214\350\257\215", CLR_ACC, CLR_SF);
        rx = (uint16_t)(rx + rw + rg);

        frame(rx, BTN_Y, rw, BTN_H2, CLR_PRI);
        text_center(rx, rw, (uint16_t)(BTN_Y + 8u), mode_str(st->mode), CLR_PRI, CLR_SF);
        rx = (uint16_t)(rx + rw + rg);

        frame(rx, BTN_Y, rw, BTN_H2, CLR_BD);
        text_center(rx, rw, (uint16_t)(BTN_Y + 8u), "\346\255\214\345\215\225", CLR_T2, CLR_SF);
    }

    s_blk |= BLK_CTRL;        /* 控制栏整条已画好: 音量数字可增量 */
}

/* 控制栏音量数字：只擦旧宽/新宽并集那一小块再重画；控制栏有效位被清时直接跳过 */
static void draw_ctrl_vol(const ui_player_state_t *st)
{
    char     vb[8];
    uint16_t wo, wn, w;

    if ((s_blk & BLK_CTRL) == 0u) return;     /* 控制栏无效：等整块重画 */

    sprintf(vb, "%u", (unsigned)st->volume);
    if (strcmp(vb, s_cb_vol) == 0) return;

    wo = ui_text_w(s_cb_vol);
    wn = ui_text_w(vb);
    w  = (wo > wn) ? wo : wn;
    bar(CB_VOL_X, (uint16_t)(BTN_Y + 8u), w, 15u, CLR_SF);
    text_at(CB_VOL_X, (uint16_t)(BTN_Y + 8u), vb, CLR_ACC, CLR_SF);
    strncpy(s_cb_vol, vb, sizeof(s_cb_vol) - 1u);
    s_cb_vol[sizeof(s_cb_vol) - 1u] = '\0';
}

/* ==================================================================
 * 侧边音量条（按需显示）
 * ================================================================== */
static void draw_side_vol(const ui_player_state_t *st)
{
    uint16_t top = TITLE_H;
    uint16_t bot = CB_Y;
    uint16_t track_y = (uint16_t)(top + 34u);
    uint16_t track_h = (uint16_t)(bot - top - 58u);
    uint16_t fh = (uint16_t)((uint32_t)track_h * st->volume / 100u);
    char buf[8];

    bar(0u, top, SV_W, (uint16_t)(bot - top), CLR_SF);
    bar((uint16_t)(SV_W - 1u), top, 1u, (uint16_t)(bot - top), CLR_BD);

    sprintf(buf, "%u", (unsigned)st->volume);
    text_center(0u, SV_W, (uint16_t)(top + 6u), buf, CLR_ACC, CLR_SF);
    strncpy(s_sv_txt, buf, sizeof(s_sv_txt) - 1u);
    s_sv_txt[sizeof(s_sv_txt) - 1u] = '\0';

    bar((uint16_t)(SV_W - 6u), track_y, 4u, track_h, CLR_CARD);
    if (fh != 0u)
        bar((uint16_t)(SV_W - 6u), (uint16_t)(track_y + track_h - fh), 4u, fh, CLR_ACC);
    s_sv_fh = fh;

    text_center(0u, SV_W, (uint16_t)(bot - 20u), "\351\237\263", CLR_T2, CLR_SF);
}

/* 侧边音量条增量刷新：数字与填充只补差额（条体/边框/轨道/「音」只在弹出时画一次） */
static void draw_side_vol_delta(const ui_player_state_t *st)
{
    uint16_t top     = TITLE_H;
    if ((s_blk & BLK_COVER) == 0u) return;    /* 侧条基线(封面列)无效：等整块重画 */
    uint16_t bot     = CB_Y;
    uint16_t track_y = (uint16_t)(top + 34u);
    uint16_t track_h = (uint16_t)(bot - top - 58u);
    uint16_t fh      = (uint16_t)((uint32_t)track_h * st->volume / 100u);
    char     buf[8];

    sprintf(buf, "%u", (unsigned)st->volume);
    if (strcmp(buf, s_sv_txt) != 0)
    {
        bar(0u, (uint16_t)(top + 6u), (uint16_t)(SV_W - 1u), 15u, CLR_SF);   /* 不碰右边框列 */
        text_center(0u, SV_W, (uint16_t)(top + 6u), buf, CLR_ACC, CLR_SF);
        strncpy(s_sv_txt, buf, sizeof(s_sv_txt) - 1u);
        s_sv_txt[sizeof(s_sv_txt) - 1u] = '\0';
    }

    if (fh > s_sv_fh)          /* 调大：只补新长出来的那一段 */
        bar((uint16_t)(SV_W - 6u), (uint16_t)(track_y + track_h - fh),
            4u, (uint16_t)(fh - s_sv_fh), CLR_ACC);
    else if (fh < s_sv_fh)     /* 调小：只把缩短的那一段擦回轨道色 */
        bar((uint16_t)(SV_W - 6u), (uint16_t)(track_y + track_h - s_sv_fh),
            4u, (uint16_t)(s_sv_fh - fh), CLR_CARD);
    s_sv_fh = fh;
}

/* 隐藏左侧音量条：擦掉侧条那一条，并把被它盖住的封面补回来 */
static void hide_side_vol(const ui_player_state_t *st)
{
    bar(0u, TITLE_H, SV_W, (uint16_t)(CB_Y - TITLE_H), CLR_BG);
    draw_cover();
}

/* ==================================================================
 * 歌单浮层
 * ================================================================== */
#define PL_TOP     TITLE_H
#define PL_HEAD   26u
#define PL_FOOT   26u
#define PL_ROW    27u
#define PL_ROWS   ((LCD_H - PL_TOP - PL_HEAD - PL_FOOT) / PL_ROW)   /* 6，6x27=162 正好填满 */

/* 画歌单一行：左侧序号 + 文件名，选中行换底色并按当前曲目加左侧竖条 */
static void draw_pl_item(uint16_t slot, uint16_t idx0, const ui_player_state_t *st)
{
    uint16_t y = (uint16_t)(PL_TOP + PL_HEAD + slot * PL_ROW);
    uint8_t  sel = (uint8_t)(idx0 == s_pl_sel);
    uint16_t bg = sel ? RGB565(0x12,0x22,0x2C) : ((slot & 1u) ? CLR_BG : CLR_SF);
    const pl_entry_t *e = (st->list != 0 && idx0 < st->list_n) ? &st->list[idx0] : 0;
    char buf[12];

    bar(0u, y, LCD_W, PL_ROW, bg);
    if (idx0 == (uint16_t)(s_index - 1u)) bar(0u, y, 2u, PL_ROW, CLR_PRI);

    sprintf(buf, "%u", (unsigned)(idx0 + 1u));
    text_at(6u, (uint16_t)(y + 6u), buf, CLR_T2, bg);
    if (e != 0)
    {
        char nm[96];
        fit_text(e->name, nm, sizeof(nm), (uint16_t)(LCD_W - 56u));
        text_at(30u, (uint16_t)(y + 6u), nm, sel ? CLR_PRI : CLR_T1, bg);
    }
}

/* 只重画 6 行条目（不动背景/头/脚），并让视窗跟随选中行 */
static void draw_pl_rows(const ui_player_state_t *st)
{
    uint16_t i;

    if (s_pl_sel < s_pl_first)                        s_pl_first = s_pl_sel;
    else if (s_pl_sel >= (uint16_t)(s_pl_first + PL_ROWS))
        s_pl_first = (uint16_t)(s_pl_sel - PL_ROWS + 1u);

    for (i = 0u; i < PL_ROWS; i++)
        draw_pl_item(i, (uint16_t)(s_pl_first + i), st);
}

/* 整层重画歌单浮层：背景 + 头(目录/曲目数) + 6 行条目 + 脚(按键提示) */
static void draw_pl_full(const ui_player_state_t *st)
{
    char buf[40];

    bar(0u, PL_TOP, LCD_W, (uint16_t)(LCD_H - PL_TOP), CLR_BG);

    /* 头 */
    bar(0u, PL_TOP, LCD_W, PL_HEAD, CLR_SF);
    bar(0u, (uint16_t)(PL_TOP + PL_HEAD - 1u), LCD_W, 1u, CLR_BD);
    sprintf(buf, "SD:/MUSIC  %u \351\246\226", (unsigned)st->list_n);
    text_at(8u, (uint16_t)(PL_TOP + 6u), buf, CLR_T1, CLR_SF);
    text_at((uint16_t)(LCD_W - 52u), (uint16_t)(PL_TOP + 6u), "\350\277\224\345\233\236", CLR_T2, CLR_SF);

    /* 歌单条目 */
    draw_pl_rows(st);

    /* 脚 */
    bar(0u, (uint16_t)(LCD_H - PL_FOOT), LCD_W, PL_FOOT, CLR_SF);
    bar(0u, (uint16_t)(LCD_H - PL_FOOT), LCD_W, 1u, CLR_BD);
    text_center(0u, LCD_W, (uint16_t)(LCD_H - PL_FOOT + 6u),
                "K2\344\270\212 K3\344\270\213 K1\351\200\211 K4/K5\350\277\224\345\233\236", CLR_T2, CLR_SF);
}

/* ==================================================================
 * 全屏歌词页（控制栏「歌词」按钮 = 旋钮按下 PD12 进出）
 * ================================================================== */

/* 生成视窗 5 行文本，当前行在 LY_CUR_ROW 槽位；无歌词时给占位文案 */
static void ly_rows(int16_t cur, char out[LY_NROW][LRC_LINE_LEN], uint8_t on[LY_NROW])
{
    uint16_t n = lrc_count();
    uint8_t  r;

    for (r = 0u; r < LY_NROW; r++)
    {
        const char *src = 0;

        out[r][0] = '\0';
        on[r]     = 0u;

        if (n == 0u)
        {
            if (r == (uint8_t)LY_CUR_ROW)      src = "\346\232\202\346\227\240\346\255\214\350\257\215";
            else if (r == (uint8_t)(LY_CUR_ROW + 1u)) src = "\350\257\267\346\254\243\350\265\217\347\272\257\351\237\263\344\271\220";
        }
        else
        {
            int16_t li = (int16_t)(cur - (int16_t)LY_CUR_ROW + (int16_t)r);
            if ((li >= 0) && (li < (int16_t)n)) src = lrc_line((uint16_t)li);
        }

        if ((src != 0) && (*src != '\0'))
        {
            strncpy(out[r], src, LRC_LINE_LEN - 1u);
            out[r][LRC_LINE_LEN - 1u] = '\0';
            on[r] = 1u;
        }
    }
}

/* 画一行：当前行 1.5x 放大 + 高亮色，其余 1x 居中 */
static void ly_draw_slot(uint8_t r, const char *txt, uint16_t y)
{
    char lb[LRC_LINE_LEN];

    bar(0u, y, LCD_W, LY_SLOT, CLR_BG);
    if ((txt == 0) || (*txt == '\0')) return;

    if (r == (uint8_t)LY_CUR_ROW)
    {
        uint16_t w2;

        /* 1.5x 下最多这么宽（留出 8 px 边距，fit_text 追加的 … 不会被挤出屏幕）：
           fit_text 是拿 1x 宽度比的，所以 1x 上限 = 可用宽 * 2 / 3 ≈ 197 px */
        fit_text(txt, lb, sizeof(lb), (uint16_t)(((LCD_W - 24u) * 2u) / 3u));
        w2 = ui_text15_w(lb);
        if (w2 > (uint16_t)(LCD_W - 8u)) w2 = (uint16_t)(LCD_W - 8u);
        ui_text15((uint16_t)((LCD_W - w2) / 2u), (uint16_t)(y + ((LY_SLOT - LY_BIG) / 2u)),
                  lb, CLR_PRI, CLR_BG);
    }
    else
    {
        fit_text(txt, lb, sizeof(lb), (uint16_t)(LCD_W - 8u));
        text_center(0u, LCD_W, (uint16_t)(y + ((LY_SLOT - 15u) / 2u)), lb, CLR_T2, CLR_BG);
    }
}

/* 顶部：歌名（左，截断）+ 序号/总数（右） */
static void ly_draw_header(const ui_player_state_t *st)
{
    char     nm[96];
    char     buf[24];
    uint16_t tw;

    bar(0u, 0u, LCD_W, LY_HEAD, CLR_BG);

    fit_text(s_have_title ? s_title : "--", nm, sizeof(nm), 210u);
    text_at(8u, 7u, nm, CLR_T1, CLR_BG);

    if (st->total != 0u) sprintf(buf, "%u/%u", (unsigned)st->index, (unsigned)st->total);
    else                 strcpy(buf, "-/-");
    tw = ui_text_w(buf);
    text_at((uint16_t)(LCD_W - 8u - tw), 7u, buf, CLR_T2, CLR_BG);

    bar(0u, (uint16_t)(LY_HEAD - 1u), LCD_W, 1u, CLR_BD);
}

/* 页脚音量读数：只有值变了（或高亮态切换）才擦那一小块重画，不做整条底板重铺。
   hl = 1 表示"刚调过音量"，用高亮底 + 亮色标签强调 1.5 s */
static void ly_draw_vol(const ui_player_state_t *st, uint8_t hl)
{
    char     vb[8];
    uint16_t y  = (uint16_t)(LCD_H - LY_FOOT);
    uint16_t w, we;
    uint16_t bg = (hl != 0u) ? CLR_SF : CLR_BG;
    uint16_t fg = (hl != 0u) ? CLR_T1 : CLR_T2;

    sprintf(vb, "%u", (unsigned)st->volume);
    w  = (uint16_t)(30u + ui_text_w(vb));        /* "音量" 28 px + 2 px 间隙 + 数字 */
    we = (w > s_ly2_vol_w) ? w : s_ly2_vol_w;    /* 擦旧宽/新宽里的较大者，位数变小时也能整块还原 */

    bar((uint16_t)(LY_VOL_X - 4u), (uint16_t)(y + 14u), (uint16_t)(we + 8u), 18u, bg);
    text_at(LY_VOL_X, (uint16_t)(y + 17u), "\351\237\263\351\207\217", fg, bg);
    text_at((uint16_t)(LY_VOL_X + 30u), (uint16_t)(y + 17u), vb, CLR_ACC, bg);

    s_ly2_vol    = st->volume;
    s_ly2_vol_w  = we;     /* 存"已画出的块宽"（不是读数宽）：位数变小时下次才能整块还原 */
    s_ly2_vol_hl = hl;
}

/* 底部：进度条 + 左右时间 + 音量读数 + 中间"旋钮按下 返回" */
static void ly_draw_footer(const ui_player_state_t *st)
{
    char     buf[24];
    uint16_t y  = (uint16_t)(LCD_H - LY_FOOT);
    uint16_t tw;
    uint16_t fw = 0u;

    bar(0u, y, LCD_W, LY_FOOT, CLR_BG);
    bar(0u, y, LCD_W, 1u, CLR_BD);

    bar(8u, (uint16_t)(y + 4u), (uint16_t)(LCD_W - 16u), 5u, CLR_SF);
    frame(8u, (uint16_t)(y + 4u), (uint16_t)(LCD_W - 16u), 5u, CLR_BD);
    s_ly2_fw = 0u;
    if ((st->total_ms != 0u) && (st->pos_ms <= st->total_ms))
    {
        fw = (uint16_t)((uint32_t)(LCD_W - 16u) * st->pos_ms / st->total_ms);
        if (fw > (uint16_t)(LCD_W - 16u)) fw = (uint16_t)(LCD_W - 16u);
        if (fw > 2u)
        {
            s_ly2_fw = (uint16_t)(fw - 2u);     /* 记住已画出的内宽，供增量刷新用 */
            bar(9u, (uint16_t)(y + 5u), s_ly2_fw, 3u, CLR_PRI);
        }
    }

    {
        uint32_t t = st->pos_ms / 1000u;
        sprintf(buf, "%lu:%02lu", (unsigned long)(t / 60u), (unsigned long)(t % 60u));
    }
    text_at(8u, (uint16_t)(y + 17u), buf, CLR_T2, CLR_BG);
    strncpy(s_ly2_tstr, buf, sizeof(s_ly2_tstr) - 1u);
    s_ly2_tstr[sizeof(s_ly2_tstr) - 1u] = '\0';

    ly_draw_vol(st, 0u);       /* 页脚音量读数（增量刷新） */

    text_center(0u, LCD_W, (uint16_t)(y + 17u), "\346\227\213\351\222\256\346\214\211\344\270\213 \350\277\224\345\233\236", CLR_ACC, CLR_BG);

    if (st->total_ms != 0u)
    {
        uint32_t t = st->total_ms / 1000u;
        sprintf(buf, "%lu:%02lu", (unsigned long)(t / 60u), (unsigned long)(t % 60u));
    }
    else strcpy(buf, "--:--");
    tw = ui_text_w(buf);
    text_at((uint16_t)(LCD_W - 8u - tw), (uint16_t)(y + 17u), buf, CLR_T2, CLR_BG);
}

/* 底部进度条：只补"新增的那一段"像素，不铺底板、不重画轨道/边框 ⇒ 不闪频 */
static void ly_draw_progress(const ui_player_state_t *st)
{
    uint16_t y  = (uint16_t)(LCD_H - LY_FOOT);
    uint16_t iw = 0u;

    if ((st->total_ms != 0u) && (st->pos_ms <= st->total_ms))
    {
        uint16_t fw = (uint16_t)((uint32_t)(LCD_W - 16u) * st->pos_ms / st->total_ms);
        if (fw > (uint16_t)(LCD_W - 16u)) fw = (uint16_t)(LCD_W - 16u);
        if (fw > 2u) iw = (uint16_t)(fw - 2u);
    }

    if (iw > s_ly2_fw)          /* 前进：只画新增的一小段 */
        bar((uint16_t)(9u + s_ly2_fw), (uint16_t)(y + 5u), (uint16_t)(iw - s_ly2_fw), 3u, CLR_PRI);
    else if (iw < s_ly2_fw)     /* 后退 / 换曲：只擦掉多出来的那一小段 */
        bar((uint16_t)(9u + iw), (uint16_t)(y + 5u), (uint16_t)(s_ly2_fw - iw), 3u, CLR_SF);
    s_ly2_fw = iw;
}

/* 左时间：只有文本真的变了才擦那一小块(40x15)重画，不整条重铺 */
static void ly_draw_time(const ui_player_state_t *st)
{
    char     buf[12];
    uint16_t y = (uint16_t)(LCD_H - LY_FOOT);
    uint32_t t = st->pos_ms / 1000u;

    sprintf(buf, "%lu:%02lu", (unsigned long)(t / 60u), (unsigned long)(t % 60u));
    if (strcmp(buf, s_ly2_tstr) == 0) return;

    bar(8u, (uint16_t)(y + 17u), 40u, 15u, CLR_BG);
    text_at(8u, (uint16_t)(y + 17u), buf, CLR_T2, CLR_BG);
    strncpy(s_ly2_tstr, buf, sizeof(s_ly2_tstr) - 1u);
    s_ly2_tstr[sizeof(s_ly2_tstr) - 1u] = '\0';
}

/* 整页重画（进页 / 换曲 / 歌词条数变化） */
static void ly_draw_full(const ui_player_state_t *st)
{
    char    nt[LY_NROW][LRC_LINE_LEN];
    uint8_t non[LY_NROW];
    uint8_t r;

    s_ly2_cur = lyric_cur(st->pos_ms);
    ly_rows(s_ly2_cur, nt, non);

    bar(0u, 0u, LCD_W, LCD_H, CLR_BG);
    ly_draw_header(st);
    for (r = 0u; r < LY_NROW; r++)
        ly_draw_slot(r, nt[r], (uint16_t)(LY_Y0 + (uint16_t)r * LY_SLOT));
    ly_draw_footer(st);

    for (r = 0u; r < LY_NROW; r++)
    {
        strncpy(s_ly2[r], nt[r], LRC_LINE_LEN - 1u);
        s_ly2[r][LRC_LINE_LEN - 1u] = '\0';
        s_ly2_on[r] = non[r];
    }
    s_ly2_have_title = s_have_title;
    strncpy(s_ly2_title, s_title, sizeof(s_ly2_title) - 1u);
    s_ly2_title[sizeof(s_ly2_title) - 1u] = '\0';
    s_ly2_index = st->index;
    s_ly2_total = st->total;
    s_ly2_lrcn  = lrc_count();
    s_ly2_s     = st->pos_ms / 1000u;
}

/* 脏刷新：行变了只重画变化的行，秒变了只重画底部，换曲才整页重画 */
static void ly_refresh(const ui_player_state_t *st, uint32_t now_ms)
{
    uint8_t need_full = 0u;

    if (st->title == 0)
    {
        if (s_ly2_have_title != 0u) { s_ly2_have_title = 0u; s_ly2_title[0] = '\0'; need_full = 1u; }
    }
    else if ((s_ly2_have_title == 0u) || (strcmp(s_ly2_title, st->title) != 0))
    {
        need_full = 1u;
    }
    if ((st->index != s_ly2_index) || (st->total != s_ly2_total)) need_full = 1u;
    if (lrc_count() != s_ly2_lrcn) need_full = 1u;

    if (need_full != 0u)
    {
        cache_title(st);              /* 供 ly_draw_header 取歌名 */
        ly_draw_full(st);
        return;
    }

    {
        int16_t cur = lyric_cur(st->pos_ms);

        if (cur != s_ly2_cur)
        {
            char    nt[LY_NROW][LRC_LINE_LEN];
            uint8_t non[LY_NROW];
            uint8_t r;

            s_ly2_cur = cur;
            ly_rows(cur, nt, non);
            for (r = 0u; r < LY_NROW; r++)
            {
                if ((non[r] == s_ly2_on[r]) && (strcmp(nt[r], s_ly2[r]) == 0u)) continue;
                ly_draw_slot(r, nt[r], (uint16_t)(LY_Y0 + (uint16_t)r * LY_SLOT));
                strncpy(s_ly2[r], nt[r], LRC_LINE_LEN - 1u);
                s_ly2[r][LRC_LINE_LEN - 1u] = '\0';
                s_ly2_on[r] = non[r];
            }
        }
    }

    {
        uint32_t ps = st->pos_ms / 1000u;
        if (ps != s_ly2_s) { s_ly2_s = ps; ly_draw_time(st); }
    }
    ly_draw_progress(st);      /* 每帧只补新增像素（通常 0~几个像素），不整块重画底部 */

    /* 音量：值变了只重画读数那一小块并高亮 1.5 s，之后恢复常规色 */
    if (st->volume != s_ly2_vol)
    {
        s_ly2_vol_t = now_ms;
        ly_draw_vol(st, 1u);
    }
    else if ((s_ly2_vol_hl != 0u) && ((uint32_t)(now_ms - s_ly2_vol_t) >= 1500u))
    {
        ly_draw_vol(st, 0u);
    }
}

/* ==================================================================
 * 对外接口
 * ================================================================== */
/* 复位全部增量刷新缓存（上电调用一次；整屏铺底由 ui_player_full() 负责） */
void ui_player_init(void)
{
    s_lyr_open = 0u; s_ly2_cur = -2;
    s_ly2_have_title = 0u; s_ly2_title[0] = '\0';
    s_ly2_index = 0xFFFFu; s_ly2_total = 0xFFFFu; s_ly2_lrcn = 0xFFFFu;
    s_ly2_s = 0xFFFFFFFFu;
    s_ly2_fw = 0u; s_ly2_tstr[0] = '\0';
    s_ly2_vol = 0xFFu; s_ly2_vol_w = 0u; s_ly2_vol_hl = 0u; s_ly2_vol_t = 0u;
    s_playing = 0xFFu; s_mode = 0xFFu; s_volume = 0xFFu; s_sd_ok = 0xFFu;
    s_index = 0xFFFFu; s_total = 0xFFFFu; s_list_n = 0xFFFFu;
    s_pos_s = 0xFFFFFFFFu;
    s_have_title = 0u; s_title[0] = '\0';
    s_vol_show = 0u; s_vol_t0 = 0u;
    s_st_vol[0] = '\0'; s_cb_vol[0] = '\0'; s_sv_txt[0] = '\0'; s_sv_fh = 0u;
    s_pr_fw = 0u; s_pr_tstr[0] = '\0';
    s_lyric_idx = -1;
    s_pl_open = 0u; s_pl_sel = 0u; s_pl_first = 0u;
    s_blk = 0u;                                   /* 主界面四块都还没画 */
    s_pg_act = PG_ACT_NONE; s_pg_in = 0u; s_pg_started = 0u; s_pg_t0 = 0u;
}

/* 缓存歌名；歌名变化时一并使信息区失效，多个绘制路径都靠它取当前歌名 */
static void cache_title(const ui_player_state_t *st)
{
    if (st->title == 0) { s_have_title = 0u; s_title[0] = '\0'; return; }
    if (s_have_title && (strcmp(s_title, st->title) == 0)) return;
    strncpy(s_title, st->title, sizeof(s_title) - 1u);
    s_title[sizeof(s_title) - 1u] = '\0';
    s_have_title = 1u;
}

/* 整屏重画主界面（状态栏/封面/信息区/控制栏依次铺底）并同步状态缓存 */
void ui_player_full(const ui_player_state_t *st)
{
    s_lyr_open = 0u;               /* 整屏主界面 = 离开全屏歌词页 */
    cache_title(st);
    s_playing = st->playing; s_mode = st->mode; s_volume = st->volume;
    s_sd_ok = st->sd_ok;
    s_index = st->index; s_total = st->total; s_list_n = st->list_n;
    s_pos_s = st->pos_ms / 1000u;
    s_vol_show = 0u;

    bar(0u, 0u, LCD_W, LCD_H, CLR_BG);
    draw_status(st);
    draw_cover();
    draw_info(st);
    draw_controls(st);
}

/* 调音量时弹出/刷新左侧音量条（1.5 s 后由 ui_player_update() 自动收起） */
void ui_player_vol_flash(const ui_player_state_t *st)
{
    if (s_pg_act   != PG_ACT_NONE) return;   /* 切页淡变中：别把侧条画到正在被换掉的画面上 */
    if (s_lyr_open != 0u) return;   /* 歌词页：页脚音量读数由 ly_refresh() 增量刷新 */
    if (s_pl_open  != 0u) return;   /* 歌单浮层：不要往浮层上画侧条 */
    s_vol_show = 1u;
    draw_side_vol(st);
}

/* 换曲后重画封面块。
   - 切页淡变中：page_commit()/restore_main() 会顺带画上新封面 -> 跳过
   - 歌单浮层/全屏歌词页中：退出时的 restore_main() 会重画封面 -> 跳过
   其余情况只重画 144x144 那块，不动状态栏/信息区/控制栏。 */
void ui_player_cover_refresh(void)
{
    if (s_pg_act   != PG_ACT_NONE) return;
    if (s_lyr_open != 0u) return;
    if (s_pl_open  != 0u) return;
    draw_cover();
}

/* 切页背光淡变状态机（定义在本文件"主界面局部恢复"一节末尾） */
static void page_fade_poll(const ui_player_state_t *st, uint32_t now_ms);

/* 界面帧循环：比较状态缓存决定哪几块要重画，其余走局部增量刷新 */
void ui_player_update(const ui_player_state_t *st, uint32_t now_ms)
{
    uint8_t need_status = 0u;
    uint8_t need_info   = 0u;
    uint8_t need_ctrl   = 0u;
    uint8_t need_lyric  = 0u;

    /* 切页淡变进行中：只走过渡状态机, 整帧刷新先停一拍。
       （否则淡出/淡入的中间帧会拿"新老混合"的状态去画, 反而更乱） */
    if (s_pg_act != PG_ACT_NONE)
    {
        page_fade_poll(st, now_ms);
        return;
    }


    if (s_lyr_open != 0u)          /* 全屏歌词页：主界面各块都不画，只做歌词页脏刷新 */
    {
        ly_refresh(st, now_ms);
        return;
    }

    if (s_pl_open) return;

    if (st->title == 0)
    {
        if (s_have_title) { s_have_title = 0u; need_info = 1u; }
    }
    else if ((s_have_title == 0u) || (strcmp(s_title, st->title) != 0))
    {
        cache_title(st);
        need_info = 1u;
    }

    if (st->index != s_index) { s_index = st->index; need_info = 1u; }
    if (st->total != s_total) { s_total = st->total; need_info = 1u; }
    if (st->total_ms != s_total_ms) { s_total_ms = st->total_ms; need_info = 1u; }
    if (st->list_n != s_list_n) s_list_n = st->list_n;
    if (st->playing != s_playing) { s_playing = st->playing; need_status = 1u; need_ctrl = 1u; }
    if (st->mode != s_mode)       { s_mode = st->mode; need_status = 1u; need_ctrl = 1u; }
    if (st->sd_ok != s_sd_ok)     { s_sd_ok = st->sd_ok; need_status = 1u; }

    {   /* 歌词当前行变了 -> 信息区要重画 */
        int16_t li = lyric_cur(st->pos_ms);
        if (li != s_lyric_idx) { s_lyric_idx = li; need_lyric = 1u; }
    }

    if (need_status) draw_status(st);
    if (need_info)   draw_info(st);
    if (need_ctrl)   draw_controls(st);

    /* 歌词滚动: 只重画变化的行, 不再整块重绘信息区 */
    if ((need_lyric != 0u) && (need_info == 0u) && ((s_blk & BLK_INFO) != 0u))
        draw_lyric_rows(COVER_Y, s_lyric_idx, 0u);

    {
        uint32_t ps = st->pos_ms / 1000u;
        if (ps != s_pos_s) { s_pos_s = ps; draw_progress_time(st); }
    }
    draw_progress_delta(st);   /* 进度条每帧只补差额像素（与歌词页同款增量刷新） */

    if ((s_vol_show != 0u) && ((uint32_t)(now_ms - s_vol_t0) >= 1500u))
    {
        s_vol_show = 0u;
        hide_side_vol(st);
    }

    /* 音量变化：侧条(弹一次后只补差额) + 状态栏数字 + 控制栏数字，都是小块增量刷新 */
    if (st->volume != s_volume)
    {
        s_volume = st->volume;
        if (s_vol_show != 0u) draw_side_vol_delta(st);   /* 已弹出：只补差额 */
        else                  draw_side_vol(st);         /* 首次弹出：整条画一次 */
        s_vol_show = 1u;
        s_vol_t0 = now_ms;
        draw_status_vol(st);   /* 只擦数字那一小块，不重画整条状态栏 */
        draw_ctrl_vol(st);     /* 控制栏音量数字增量补齐 */
    }
}

/* ==================================================================
 * 主界面局部恢复（歌单浮层 / 全屏歌词页退出）
 * ui_player_full() 仍是整屏兜底；这里是"只重画被覆盖区域"的常规路径。
 * ================================================================== */

/* 把"上一帧状态"缓存同步到当前 st：整块重画或局部恢复之后调用,
   这样下一帧不会重复重画, 增量刷新也能接着用 */
static void sync_state_cache(const ui_player_state_t *st)
{
    cache_title(st);
    s_playing   = st->playing;
    s_mode      = st->mode;
    s_volume    = st->volume;
    s_sd_ok     = st->sd_ok;
    s_index     = st->index;
    s_total     = st->total;
    s_total_ms  = st->total_ms;
    s_list_n    = st->list_n;
    s_pos_s     = st->pos_ms / 1000u;
    s_lyric_idx = lyric_cur(st->pos_ms);
    s_vol_show  = 0u;
}

/* 只擦主区里"四块底板铺不到"的背景缝隙, 不整屏清屏:
   上 11 px 横带(26..36) + 下 11 px 横带(181..191)
   + 封面左侧 8 px / 封面与信息区之间 8 px / 信息区右侧 8 px（各 144 行） */
static void clear_main_bg(void)
{
    bar(0u, MAIN_Y, LCD_W, (uint16_t)(COVER_Y - MAIN_Y), CLR_BG);
    bar(0u, (uint16_t)(COVER_Y + COVER_S), LCD_W,
        (uint16_t)(CB_Y - COVER_Y - COVER_S), CLR_BG);
    bar(0u, COVER_Y, COVER_X, COVER_S, CLR_BG);
    bar((uint16_t)(COVER_X + COVER_S), COVER_Y,
        (uint16_t)(INFO_X - COVER_X - COVER_S), COVER_S, CLR_BG);
    bar((uint16_t)(INFO_X + INFO_W), COVER_Y,
        (uint16_t)(LCD_W - INFO_X - INFO_W), COVER_S, CLR_BG);
}

/* 局部恢复被浮层/歌词页盖住的主界面:
   ① 浮层/歌词页期间 st 可能已经变了(旋钮调音量/曲目结束/换模式…) => 状态栏不在恢复范围内时先看它是否过期;
   ② 只擦缝隙, 不清整屏;
   ③ 逐块重画被覆盖的块（各块自带底板, 画完自己置有效位）, 最后同步"上一帧状态"缓存。 */
static void restore_main(const ui_player_state_t *st, uint8_t mask)
{
    if ((mask & BLK_STATUS) == 0u)
    {
        if ((s_blk & BLK_STATUS) == 0u)          mask |= BLK_STATUS;   /* 本来就无效: 必须重画 */
        else if ((st->playing != s_playing) ||
                 (st->mode    != s_mode)    ||
                 (st->sd_ok   != s_sd_ok))       mask |= BLK_STATUS;   /* 圆点/模式/SD 变了: 整条补 */
        else if (st->volume != s_volume)         draw_status_vol(st);  /* 只有音量数字变了: 只擦那一小块 */
    }

    if ((mask & (BLK_COVER | BLK_INFO)) != 0u) clear_main_bg();

    sync_state_cache(st);

    if ((mask & BLK_STATUS) != 0u) draw_status(st);
    if ((mask & BLK_COVER)  != 0u) draw_cover();
    if ((mask & BLK_INFO)   != 0u) draw_info(st);
    if ((mask & BLK_CTRL)   != 0u) draw_controls(st);
}

/* ==================================================================
 * 切页背光淡变状态机（非阻塞）
 *
 * 为什么加: 切页时画面整块变化, 即使已经做到"局部恢复", 人眼还是觉得
 * 跳一下。把整块重画放到背光最暗的那帧, 再淡回来, 割裂感就没了。
 * 为什么非阻塞: 主循环里还要跑 audio_srv_poll() 往 USART2 发送环补 SD
 * 数据, 阻塞 120 ms 可能让 ESP32 端 I2S 断流; 而 ui_player_update() 每帧
 * 约 1 ms 被调用一次, 每帧走一档已经足够顺。
 * 时序: 淡出 60 ms -> 全黑那一帧执行 page_commit() -> 淡入 60 ms -> 全亮。
 * ================================================================== */
static void pg_begin(uint8_t act)
{
    /* 过渡中再次请求切页: 直接换成新动作, 从当前亮度重新淡出,
       最终状态由新的 commit 决定 (开/关语义相反, 不会停在错页面上) */
    s_pg_act     = act;
    s_pg_in      = 0u;
    s_pg_started = 0u;
    s_pg_t0      = 0u;
}

/* 最暗的那一帧才真正换画面 */
static void page_commit(const ui_player_state_t *st)
{
    switch (s_pg_act)
    {
    case PG_ACT_LIST_ON:
        draw_pl_full(st);
        break;
    case PG_ACT_LIST_OFF:
        restore_main(st, (uint8_t)(BLK_COVER | BLK_INFO | BLK_CTRL));
        break;
    case PG_ACT_LY_ON:
        ly_draw_full(st);
        break;
    default:   /* PG_ACT_LY_OFF */
        restore_main(st, BLK_ALL);
        break;
    }
}

static void page_fade_poll(const ui_player_state_t *st, uint32_t now_ms)
{
    uint32_t e;
    uint8_t  lv;

    if (s_pg_act == PG_ACT_NONE) return;
    if (s_pg_started == 0u) { s_pg_started = 1u; s_pg_t0 = now_ms; }

    e = now_ms - s_pg_t0;                        /* 无符号差, 自然处理回绕 */
    if (e > PG_FADE_MS) e = PG_FADE_MS;

    if (s_pg_in == 0u)                           /* 淡出: FULL -> 0 */
    {
        lv = (uint8_t)(LCD_BL_FULL - (uint8_t)((LCD_BL_FULL * e) / PG_FADE_MS));
        lcd_backlight_set(lv);
        if (lv == 0u)
        {
            page_commit(st);                     /* 全黑时才换画面 */
            s_pg_in = 1u;
            s_pg_t0 = now_ms;                    /* 淡入重新计时 */
        }
    }
    else                                         /* 淡入: 0 -> FULL */
    {
        lv = (uint8_t)((LCD_BL_FULL * e) / PG_FADE_MS);
        lcd_backlight_set(lv);
        if (lv >= LCD_BL_FULL)
        {
            lcd_backlight_set(LCD_BL_FULL);      /* 兜底: 无论如何都回到全亮 */
            s_pg_act = PG_ACT_NONE;              /* 过渡结束, 恢复正常刷新 */
        }
    }
}

/* ---------------- 歌单 ---------------- */
void ui_player_list_open(const ui_player_state_t *st)
{
    if (s_pl_open == 0u)          /* 首次打开：选中行定位到当前曲目 */
    {
        s_pl_sel   = (st->index != 0u) ? (uint16_t)(st->index - 1u) : 0u;
        s_pl_first = 0u;
    }
    s_pl_open = 1u;
    /* 浮层从 y=26(PL_TOP=TITLE_H) 起盖住整个主区: 封面/信息区/控制栏的增量基线失效;
       状态栏(0..25)没被覆盖, 保持有效 => 关浮层时不必重画它 */
    s_blk = (uint8_t)(s_blk & (uint8_t)~(BLK_COVER | BLK_INFO | BLK_CTRL));
    pg_begin(PG_ACT_LIST_ON);     /* 先淡出, 到最暗点再 draw_pl_full() */
}

/* 移动选中行：脏刷新 —— 不重画背景/头/脚，只重画受影响的行 */
void ui_player_list_move(int8_t dir, const ui_player_state_t *st)
{
    uint16_t old_sel;
    uint16_t old_first;

    if (s_pl_open == 0u) return;
    if (s_pg_act != PG_ACT_NONE) return;   /* 淡变中: 浮层还没铺好, 先不接受行移动 */

    old_sel   = s_pl_sel;
    old_first = s_pl_first;

    if (dir < 0)
    {
        if (s_pl_sel > 0u) s_pl_sel--;
    }
    else
    {
        if ((s_list_n != 0u) && (s_pl_sel < (uint16_t)(s_list_n - 1u))) s_pl_sel++;
    }

    if (s_pl_sel == old_sel) return;      /* 到顶/到底：无变化，一帧都不重画 */

    if (s_pl_sel < s_pl_first) s_pl_first = s_pl_sel;
    else if (s_pl_sel >= (uint16_t)(s_pl_first + PL_ROWS))
        s_pl_first = (uint16_t)(s_pl_sel - PL_ROWS + 1u);

    if (s_pl_first != old_first)
    {
        draw_pl_rows(st);                                    /* 翻页：整窗内容位移，逐行重画 */
    }
    else
    {
        draw_pl_item((uint16_t)(old_sel - old_first), old_sel, st);      /* 旧选中行还原 */
        draw_pl_item((uint16_t)(s_pl_sel - s_pl_first), s_pl_sel, st);   /* 新选中行高亮 */
    }
}

/* 歌单浮层当前选中下标（0 起） */
uint16_t ui_player_list_sel(void) { return s_pl_sel; }

/* 关歌单浮层：清打开标志，实际恢复动作放到淡变最暗点执行 */
void ui_player_list_close(const ui_player_state_t *st)
{
    s_pl_open = 0u;
    /* 只恢复浮层盖住的三块（状态栏没被盖住 ⇒ 不重画、不闪）;
       实际恢复动作放到淡变最暗点执行 */
    pg_begin(PG_ACT_LIST_OFF);
}

/* 歌单浮层是否打开 */
uint8_t ui_player_list_is_open(void) { return s_pl_open; }

/* ---------------- 全屏歌词模式 ---------------- */
/* 进全屏歌词页：清主界面有效位与歌词页缓存，淡出到最暗点再整页绘制 */
void ui_player_lyrics_open(const ui_player_state_t *st)
{
    s_lyr_open = 1u;
    s_blk = 0u;                    /* 歌词页整屏覆盖 => 主界面四块全部失效 */
    s_ly2_have_title = 0u; s_ly2_title[0] = '\0';
    s_ly2_index = 0xFFFFu; s_ly2_total = 0xFFFFu; s_ly2_lrcn = 0xFFFFu;
    s_ly2_s = 0xFFFFFFFFu;
    s_ly2_fw = 0u; s_ly2_tstr[0] = '\0';
    s_ly2_vol = 0xFFu; s_ly2_vol_w = 0u; s_ly2_vol_hl = 0u; s_ly2_vol_t = 0u;
    cache_title(st);               /* 进页前同步歌名缓存 */
    pg_begin(PG_ACT_LY_ON);        /* 先淡出, 到最暗点再 ly_draw_full() */
}

/* 退全屏歌词页：清标志，实际恢复动作放到淡变最暗点执行 */
void ui_player_lyrics_close(const ui_player_state_t *st)
{
    s_lyr_open = 0u;
    /* 歌词页盖住整屏 => 四块都恢复, 但走"局部重画被覆盖区域"而不是整屏清屏;
       实际恢复动作放到淡变最暗点执行 */
    pg_begin(PG_ACT_LY_OFF);
}

/* 全屏歌词页是否打开 */
uint8_t ui_player_lyrics_is_open(void) { return s_lyr_open; }
