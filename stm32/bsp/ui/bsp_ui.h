/*
 * bsp_ui.h - 2.4 寸 ST7789 面板（横版 320x240）的 UTF-8 文本渲染接口
 *
 * 面板由 bsp_lcd 的阻塞 SPI 驱动，板上没有离屏帧缓冲，所以字符串是逐字形
 * 直接画在调用方指定的坐标上，不做整屏合成。
 *
 * 字库来自 ui_font_cn14.h：14 px、1 bpp，由 LVGL v8 格式的
 * font_chinese_14.h 精简生成，含可打印 ASCII 加播放器界面用到的汉字。
 * 因此传入的字符串必须是 UTF-8；界面文案宏 S_* 在 bsp/ui/ui_text.h。
 *
 * 依赖：bsp_lcd（lcd_blit）、ui_font_cn14.h 的字库数组。
 * 调用者：bsp/ui/ui_player.c（主界面 / 歌单浮层 / 歌词页的文本绘制）。
 */
#ifndef __BSP_UI_H
#define __BSP_UI_H

#include "bsp_lcd.h"

/* 字形裁剪边界是按 320x240 横版面板写死的，尺寸不符直接编译报错 */
#if (LCD_W != 320) || (LCD_H != 240)
#error "bsp_ui expects a 320x240 landscape panel"
#endif

/* ===================== text ===================== */
/* 画一个 UTF-8 字符串。y 是 14 px 行盒的顶部。
   字形盒内每个像素都会写 fg 或 bg，所以文本是不透明的矩形块。 */
void     ui_text(uint16_t x, uint16_t y, const char *s, uint16_t fg, uint16_t bg);

/* UTF-8 串的像素宽度（与绘制步进严格一致，供居中/右对齐用） */
uint16_t ui_text_w(const char *s);

/* 1.5 倍放大版（全屏歌词页当前行）：按 num/den = 3/2 最近邻采样，
   宽高都是 1x 的 1.5 倍（14 px 行盒 -> 22 px）。y 是 22 px 放大行盒的顶部；
   ui_text15_w 返回放大后的像素宽（逐字形取整，与绘制步进严格一致）。 */
void     ui_text15(uint16_t x, uint16_t y, const char *s, uint16_t fg, uint16_t bg);
uint16_t ui_text15_w(const char *s);

#endif /* __BSP_UI_H */
