/*
 * bsp_lcd.h - ST7789 2.4 寸 TFT 驱动（原生 240x320，RGB565，BGR 玻璃）
 *
 * 硬件 SPI1 主机、只写（只用 SCK + MOSI，不需要 MISO）。引脚全部由下面的宏给出，
 * 换板子时只改宏。
 *
 * 默认接线（嘉立创天空星 STM32F407VET6）：
 *   SCK  = PA5   (SPI1_SCK, AF5)
 *   MOSI = PA7   (SPI1_MOSI, AF5)
 *   CS   = PA4   (GPIO 输出)
 *   DC   = PB0   (GPIO 输出)
 *   RST  = PB1   (GPIO 输出)
 *   BLK  = PB10  (TIM2_CH3 AF1，10 kHz PWM 背光)
 */
#ifndef __BSP_LCD_H
#define __BSP_LCD_H

#include "stm32f4xx.h"

/* ===================== 引脚表（换板子只改这里） ===================== */
#define LCD_SPI             SPI1
#define LCD_SPI_CLK         RCC_APB2Periph_SPI1

#define LCD_GPIO_CLK        (RCC_AHB1Periph_GPIOA | RCC_AHB1Periph_GPIOB)

#define LCD_SCK_PORT         GPIOA
#define LCD_SCK_PIN          GPIO_Pin_5
#define LCD_SCK_PIN_SRC      GPIO_PinSource5

#define LCD_MOSI_PORT        GPIOA
#define LCD_MOSI_PIN         GPIO_Pin_7
#define LCD_MOSI_PIN_SRC     GPIO_PinSource7

#define LCD_CS_PORT          GPIOA
#define LCD_CS_PIN           GPIO_Pin_4

#define LCD_DC_PORT          GPIOB
#define LCD_DC_PIN           GPIO_Pin_0

#define LCD_RST_PORT         GPIOB
#define LCD_RST_PIN          GPIO_Pin_1

#define LCD_BLK_PORT         GPIOB
#define LCD_BLK_PIN          GPIO_Pin_10
#define LCD_BLK_PIN_SRC      GPIO_PinSource10
#define LCD_BLK_AF           GPIO_AF_TIM2

/* ---------------- 背光 PWM（PB10 上的 TIM2_CH3） ----------------
 * APB1 定时器时钟 84 MHz，PSC=83 -> 1 MHz，ARR=99 -> 10 kHz。
 * 亮度档位 0..LCD_BL_FULL（100 档，因为 ARR+1 == 100，所以 CCR 就等于档位）。
 * 板上 PB10 有 10 kOhm 下拉：复位期间到 lcd_backlight_init() 之前背光一直灭，不会上电闪。 */
#define LCD_BL_TIM           TIM2
#define LCD_BL_PSC           83u
#define LCD_BL_ARR           99u
#define LCD_BL_FULL          100u

/* SPI 波特率分频（APB2 = 84 MHz）
   _8  -> 10.5 MHz（长跳线/面包板上也稳，默认）
   _4  -> 21 MHz （线短且面板稳定时可用）
   _2  -> 42 MHz （只有 PCB 短线才建议） */
#ifndef LCD_SPI_PRESCALER
#define LCD_SPI_PRESCALER    SPI_BaudRatePrescaler_8
#endif

/* ---------------- 控制脚操作宏 ---------------- */
#define LCD_CS_HI()    GPIO_SetBits(LCD_CS_PORT,   LCD_CS_PIN)
#define LCD_CS_LO()    GPIO_ResetBits(LCD_CS_PORT, LCD_CS_PIN)
#define LCD_DC_HI()    GPIO_SetBits(LCD_DC_PORT,   LCD_DC_PIN)
#define LCD_DC_LO()    GPIO_ResetBits(LCD_DC_PORT, LCD_DC_PIN)
#define LCD_RST_HI()   GPIO_SetBits(LCD_RST_PORT,  LCD_RST_PIN)
#define LCD_RST_LO()   GPIO_ResetBits(LCD_RST_PORT,LCD_RST_PIN)
/* 背光由 TIM2_CH3 硬件 PWM 驱动（PB10 处于 AF 模式），下面两个宏在 PWM 模式下不生效，
   仅供非 PWM 移植参考；改亮度请用 lcd_backlight_set()/lcd_set_backlight()。 */
#define LCD_BLK_ON()   GPIO_SetBits(LCD_BLK_PORT,  LCD_BLK_PIN)
#define LCD_BLK_OFF()  GPIO_ResetBits(LCD_BLK_PORT,LCD_BLK_PIN)

/* ---------------- 面板几何（2.4 寸 ST7789） ----------------
 * 面板原生 240x320；靠 MADCTL 旋转帧存，本工程按 320x240 横屏驱动。 */
#define LCD_W   320
#define LCD_H   240

/* MADCTL(0x36) 取值：默认 0xA0 = MY|MV（横屏方向）；画面若上下颠倒就改成 0x60 = MX|MV。
 * BGR 位保持 OFF（面板是 RGB 玻璃，开 BGR 会红蓝互换）。 */
#ifndef LCD_MADCTL
#define LCD_MADCTL  0xA0
#endif

/* ---------------- RGB565 颜色宏 ---------------- */
#define RGB565(r,g,b)  ((uint16_t)((((r)&0xF8)<<8) | (((g)&0xFC)<<3) | ((b)>>3)))
#define COLOR_BLACK    0x0000
#define COLOR_WHITE    0xFFFF
#define COLOR_RED      0xF800
#define COLOR_GREEN    0x07E0
#define COLOR_BLUE     0x001F
#define COLOR_YELLOW   0xFFE0
#define COLOR_CYAN     0x07FF
#define COLOR_MAGENTA  0xF81F
#define COLOR_ORANGE   0xFD20
#define COLOR_GRAY     0x8410

/* ---------------- 对外接口 ---------------- */
void lcd_init(void);
void lcd_set_backlight(uint8_t on);          /* 兼容包装: on!=0 -> 100%, 0 -> 全灭 */
void lcd_backlight_init(void);               /* PB10 -> TIM2_CH3 PWM, 初始占空比 0 */
void lcd_backlight_set(uint8_t duty);        /* 0..LCD_BL_FULL 档 (10 kHz PWM) */
uint8_t lcd_backlight_get(void);             /* 当前占空比档位 */
void lcd_backlight_fade(uint8_t to, uint16_t ms);  /* 阻塞式淡变; 仅上电阶段用 */

void lcd_fill_screen(uint16_t color);
void lcd_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);
void lcd_draw_pixel(uint16_t x, uint16_t y, uint16_t color);

/* 行优先 RGB565 位图：px[0]=左上角，先左到右再上到下 */
void lcd_blit(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint16_t *px);
void lcd_draw_hline(uint16_t x, uint16_t y, uint16_t w, uint16_t color);
void lcd_draw_vline(uint16_t x, uint16_t y, uint16_t h, uint16_t color);
void lcd_draw_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);


#endif /* __BSP_LCD_H */
