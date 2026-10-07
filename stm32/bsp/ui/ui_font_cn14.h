/* Auto-generated 14 px 1 bpp bitmap font -- DO NOT EDIT.
 * Source : simsun.ttc @ 14 px (new glyphs) + P1 ui_font_cn14 glyphs (unchanged).
 * Format : 1bpp, row-major, stride=(w+7)/8, MSB = leftmost pixel.
 * Glyphs : 3973 (ASCII 0x20-0x7E + 3878 CJK, including GB2312 level-1).
 * off is unsigned int: bitmap exceeds 64 KB.
 */
#ifndef UI_FONT_CN14_H
#define UI_FONT_CN14_H

#define UI_F14_LINE_H  15
#define UI_F14_BASE_Y  13

typedef struct {
    unsigned short uni;   /* unicode codepoint      */
    unsigned char  adv;   /* advance width, pixels  */
    unsigned char  w;     /* bitmap box width       */
    unsigned char  h;     /* bitmap box height      */
    signed char    ox;    /* left edge from pen     */
    signed char    oy;    /* top row in the line box*/
    unsigned int   off;   /* byte offset into bits  */
} ui_f14_glyph_t;

extern const unsigned char  ui_f14_bits[];
extern const ui_f14_glyph_t ui_f14_glyphs[];
extern const unsigned int   ui_f14_n;

#endif /* UI_FONT_CN14_H */
