/*
 * bsp_lcd.c - ST7789 2.4 寸 TFT 驱动（硬件 SPI1，只写）
 *
 * 硬件：SCK=PA5(SPI1_SCK,AF5)、MOSI=PA7(SPI1_MOSI,AF5)、CS=PA4、DC=PB0、RST=PB1、
 *       背光 BLK=PB10(TIM2_CH3,AF1，10 kHz 硬件 PWM)。引脚宏都在 bsp_lcd.h 里。
 * 时序/总线：SPI1 全双工主机、8 位、Mode 0(CPOL_Low/CPHA_1Edge)，默认 10.5 MHz
 *       (APB2 84 MHz / 8)；每个字节都读走 RXNE，移位寄存器不会卡住；MISO(PA6) 不配成
 *       AF，保持空闲。无 DMA：逐窗口连续突发写 GRAM，部分刷新已经够快。
 * 依赖：bsp_lcd.h（引脚/几何/颜色宏）、board.h（delay_ms）、stm32f4xx.h。
 * 调用：board.c 初始化 lcd_init()/lcd_backlight_init()；UI 层 bsp_ui.c、ui_player.c 调
 *       lcd_blit()/lcd_fill_rect() 等绘图接口。
 */
#include "bsp_lcd.h"
#include "board.h"   /* delay_ms */

/* ===================================================================
 * 底层 SPI 字节收发（阻塞，读走 RX 防止移位寄存器卡住）
 * =================================================================== */
static void lcd_spi_xfer(uint8_t d)
{
    /* 等 TXE：发送缓冲空 */
    while (SPI_I2S_GetFlagStatus(LCD_SPI, SPI_I2S_FLAG_TXE) == RESET) { }
    SPI_I2S_SendData(LCD_SPI, d);
    /* 等 RXNE：字节收发完成 */
    while (SPI_I2S_GetFlagStatus(LCD_SPI, SPI_I2S_FLAG_RXNE) == RESET) { }
    (void)SPI_I2S_ReceiveData(LCD_SPI);
}

/* 发一个命令字节（DC=0） */
static void lcd_cmd(uint8_t c)
{
    LCD_CS_LO();
    LCD_DC_LO();
    lcd_spi_xfer(c);
    LCD_CS_HI();
}

/* 连发 N 个数据字节（DC=1），CS 全程保持低 */
static void lcd_data_buf(const uint8_t *p, uint32_t n)
{
    LCD_CS_LO();
    LCD_DC_HI();
    while (n--)
        lcd_spi_xfer(*p++);
    LCD_CS_HI();
}

/* 发一个数据字节（DC=1） */
static void lcd_data8(uint8_t d)
{
    lcd_data_buf(&d, 1);
}

/* ===================================================================
 * GPIO 与 SPI1 初始化
 * =================================================================== */
static void lcd_gpio_init(void)
{
    GPIO_InitTypeDef gi;

    RCC_AHB1PeriphClockCmd(LCD_GPIO_CLK, ENABLE);

    /* 控制脚：推挽输出 50 MHz */
    GPIO_StructInit(&gi);
    gi.GPIO_Mode  = GPIO_Mode_OUT;
    gi.GPIO_OType = GPIO_OType_PP;
    gi.GPIO_PuPd  = GPIO_PuPd_NOPULL;
    gi.GPIO_Speed = GPIO_Speed_50MHz;

    gi.GPIO_Pin = LCD_CS_PIN;   GPIO_Init(LCD_CS_PORT,  &gi);
    gi.GPIO_Pin = LCD_DC_PIN;   GPIO_Init(LCD_DC_PORT,  &gi);
    gi.GPIO_Pin = LCD_RST_PIN;  GPIO_Init(LCD_RST_PORT, &gi);
    gi.GPIO_Pin = LCD_BLK_PIN;  GPIO_Init(LCD_BLK_PORT, &gi);

    /* SPI 脚：AF5（PA5=SPI1_SCK、PA7=SPI1_MOSI），50 MHz */
    GPIO_PinAFConfig(LCD_SCK_PORT,  LCD_SCK_PIN_SRC,  GPIO_AF_SPI1);
    GPIO_PinAFConfig(LCD_MOSI_PORT, LCD_MOSI_PIN_SRC, GPIO_AF_SPI1);
    gi.GPIO_Mode  = GPIO_Mode_AF;
    gi.GPIO_OType = GPIO_OType_PP;
    gi.GPIO_PuPd  = GPIO_PuPd_UP;
    gi.GPIO_Speed = GPIO_Speed_50MHz;
    gi.GPIO_Pin   = LCD_SCK_PIN;   GPIO_Init(LCD_SCK_PORT,  &gi);
    gi.GPIO_Pin   = LCD_MOSI_PIN;  GPIO_Init(LCD_MOSI_PORT, &gi);

    /* 空闲电平：CS/DC 拉高、RST 先拉低、背光先关 */
    LCD_CS_HI();
    LCD_DC_HI();
    LCD_RST_HI();

    /* 背光: 先把 PB10 拉低(灭)保证上电瞬间不亮, 再切成 TIM2_CH3 硬件 PWM
       (占空比初值 0 -> 仍然不亮)。真正点亮由 main 在首帧 UI 画完后触发。 */
    LCD_BLK_OFF();
    lcd_backlight_init();
}

/* 配置 SPI1：主机、8 位、Mode 0、软件 NSS、波特率取 LCD_SPI_PRESCALER */
static void lcd_spi_init(void)
{
    SPI_InitTypeDef si;

    RCC_APB2PeriphClockCmd(LCD_SPI_CLK, ENABLE);

    SPI_StructInit(&si);
    si.SPI_Direction           = SPI_Direction_2Lines_FullDuplex;
    si.SPI_Mode                = SPI_Mode_Master;
    si.SPI_DataSize            = SPI_DataSize_8b;
    si.SPI_CPOL                = SPI_CPOL_Low;    /* mode 0 */
    si.SPI_CPHA                = SPI_CPHA_1Edge;  /* mode 0 */
    si.SPI_NSS                 = SPI_NSS_Soft;
    si.SPI_BaudRatePrescaler   = LCD_SPI_PRESCALER;
    si.SPI_FirstBit            = SPI_FirstBit_MSB;
    si.SPI_CRCPolynomial       = 7;
    SPI_Init(LCD_SPI, &si);
    SPI_CalculateCRC(LCD_SPI, DISABLE);
    SPI_Cmd(LCD_SPI, ENABLE);
}

/* ===================================================================
 * ST7789 初始化序列（标准 2.4 寸模块，16 bpp，BGR 玻璃）
 * =================================================================== */
/* 硬件复位 + 厂商初始化序列 + 整屏填黑（刻意不开背光，等首帧画完再开） */
void lcd_init(void)
{
    lcd_gpio_init();
    lcd_spi_init();

    /* 硬件复位 */
    LCD_RST_LO();
    delay_ms(20);
    LCD_RST_HI();
    delay_ms(120);

    lcd_cmd(0x01);   /* SWRESET */
    delay_ms(120);

    lcd_cmd(0x11);   /* SLPOUT */
    delay_ms(120);

    lcd_cmd(0x13);   /* NORON (normal display mode) */

    lcd_cmd(0x36);   /* MADCTL: 横屏方向由 LCD_MADCTL 给出(默认 0xA0=MY|MV)，BGR 位 OFF */
    lcd_data8(LCD_MADCTL);

    lcd_cmd(0x3A);   /* COLMOD: 16 bpp RGB565 */
    lcd_data8(0x55);

    { uint8_t d[5] = {0x0C, 0x0C, 0x00, 0x33, 0x33}; lcd_cmd(0xB2); lcd_data_buf(d, 5); } /* PORCTRL */
    { lcd_cmd(0xB7); lcd_data8(0x35); }                          /* GCTRL   VGH/VGL */
    { lcd_cmd(0xBB); lcd_data8(0x19); }                          /* VCOMS   0.7V   */
    { lcd_cmd(0xC0); lcd_data8(0x2C); }                          /* LCMCTRL */
    { uint8_t d[2] = {0x01, 0xFF}; lcd_cmd(0xC2); lcd_data_buf(d, 2); } /* VDVVRHEN */
    { lcd_cmd(0xC3); lcd_data8(0x27); }                          /* VRHS    */
    { lcd_cmd(0xC4); lcd_data8(0x1F); }                          /* VDVSET  */
    { lcd_cmd(0xC6); lcd_data8(0x0A); }                          /* FRCTRL2 ~60Hz */
    { uint8_t d[2] = {0xA4, 0xA1}; lcd_cmd(0xD0); lcd_data_buf(d, 2); } /* PWCTRL1 */

    /* 刻意不发 INVON(0x21)：这块 RGB 玻璃在反显关掉时才颜色正确。若同时开 0x21 和
       BGR 位，会出现黑变白、红变黄（反显与 R/B 交换叠加）。 */

    lcd_cmd(0x37);   /* VSCRSADD: vertical scroll start = 0 (kill leftover scroll) */
    { uint8_t d[2] = {0x00, 0x00}; lcd_data_buf(d, 2); }

    lcd_cmd(0x29);   /* DISPON */
    delay_ms(50);

    /* 这里刻意【不】开背光，并先把整屏填黑。
       上电顺序：面板初始化 -> 整屏填黑 -> 其它外设初始化 -> 画好首帧 UI -> main 再开背光；
       背光早开时 GRAM 还是随机内容，会看到上电闪一下/花屏。 */
    lcd_fill_screen(COLOR_BLACK);
    lcd_set_backlight(0);
}

/* 兼容包装：on!=0 视为 100% 亮度，0 视为全灭 */
void lcd_set_backlight(uint8_t on)
{
    lcd_backlight_set((on != 0u) ? LCD_BL_FULL : 0u);
}

/* ===================================================================
 * 背光 PWM：TIM2_CH3 / PB10，10 kHz，占空比 0..LCD_BL_FULL 档
 *
 * 84 MHz / (PSC+1) / (ARR+1) = 84M / 84 / 100 = 10 kHz
 * （PWM1 极性高：CCR=0 -> 全灭，CCR >= ARR+1 -> 全亮）
 * 硬件 PWM 既支持上电/切页的短淡变，也提供多档亮度接口。
 * =================================================================== */
static uint8_t s_bl_duty = 0u;      /* 当前占空比档位(0..LCD_BL_FULL) */

/* 把 PB10 配成 TIM2_CH3 PWM（10 kHz），初始占空比 0（灭） */
void lcd_backlight_init(void)
{
    GPIO_InitTypeDef        gi;
    TIM_TimeBaseInitTypeDef tb;
    TIM_OCInitTypeDef       oc;

    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM2, ENABLE);

    /* PB10 -> AF1(TIM2_CH3)，推挽，50 MHz */
    GPIO_PinAFConfig(LCD_BLK_PORT, LCD_BLK_PIN_SRC, LCD_BLK_AF);
    GPIO_StructInit(&gi);
    gi.GPIO_Pin   = LCD_BLK_PIN;
    gi.GPIO_Mode  = GPIO_Mode_AF;
    gi.GPIO_OType = GPIO_OType_PP;
    gi.GPIO_PuPd  = GPIO_PuPd_NOPULL;    /* 板上 PB10 已有 10 k 下拉 */
    gi.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(LCD_BLK_PORT, &gi);

    TIM_TimeBaseStructInit(&tb);
    tb.TIM_Prescaler     = LCD_BL_PSC;
    tb.TIM_Period        = LCD_BL_ARR;
    tb.TIM_CounterMode   = TIM_CounterMode_Up;
    tb.TIM_ClockDivision = TIM_CKD_DIV1;
    TIM_TimeBaseInit(LCD_BL_TIM, &tb);

    TIM_OCStructInit(&oc);
    oc.TIM_OCMode      = TIM_OCMode_PWM1;
    oc.TIM_OutputState = TIM_OutputState_Enable;
    oc.TIM_OCPolarity  = TIM_OCPolarity_High;
    oc.TIM_Pulse       = 0u;             /* 上电先全灭 */
    TIM_OC3Init(LCD_BL_TIM, &oc);
    TIM_OC3PreloadConfig(LCD_BL_TIM, TIM_OCPreload_Enable);
    TIM_ARRPreloadConfig(LCD_BL_TIM, ENABLE);

    s_bl_duty = 0u;
    TIM_SetCompare3(LCD_BL_TIM, 0u);
    TIM_Cmd(LCD_BL_TIM, ENABLE);
}

/* 设置背光亮度档位（0..LCD_BL_FULL，超范围会被钳位） */
void lcd_backlight_set(uint8_t duty)
{
    if (duty > LCD_BL_FULL) duty = LCD_BL_FULL;
    s_bl_duty = duty;
    TIM_SetCompare3(LCD_BL_TIM, (uint16_t)duty);
}

/* 当前背光亮度档位 */
uint8_t lcd_backlight_get(void)
{
    return s_bl_duty;
}

/* 阻塞式淡变: 只在上电"全部初始化 + 首帧画完"之后调用一次
   (那时主循环还没开始, 阻塞几十毫秒没有副作用)。
   运行期的切页淡变走 ui_player.c 里的非阻塞状态机, 不用这个函数。 */
void lcd_backlight_fade(uint8_t to, uint16_t ms)
{
    uint8_t  from = s_bl_duty;
    uint16_t i;

    if (to > LCD_BL_FULL) to = LCD_BL_FULL;
    if (ms == 0u) { lcd_backlight_set(to); return; }

    for (i = 1u; i <= ms; i++)
    {
        int32_t v = (int32_t)from +
                    (((int32_t)to - (int32_t)from) * (int32_t)i) / (int32_t)ms;
        lcd_backlight_set((uint8_t)v);
        delay_ms(1u);
    }
    lcd_backlight_set(to);
}

/* ===================================================================
 * 绘图原语
 * =================================================================== */

/* 设置写窗口（CASET/RASET）并发 RAMWR 准备写入 */
/* 设置活动写窗口并发 RAMWR，之后的像素数据都落在这个矩形里 */
static void lcd_set_window(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    uint16_t x2 = x + w - 1;
    uint16_t y2 = y + h - 1;
    uint8_t  b[4];

    if (x2 >= LCD_W) x2 = LCD_W - 1;
    if (y2 >= LCD_H) y2 = LCD_H - 1;

    lcd_cmd(0x2A);                       /* CASET */
    b[0] = (uint8_t)(x  >> 8); b[1] = (uint8_t)(x  & 0xFF);
    b[2] = (uint8_t)(x2 >> 8); b[3] = (uint8_t)(x2 & 0xFF);
    lcd_data_buf(b, 4);

    lcd_cmd(0x2B);                       /* RASET */
    b[0] = (uint8_t)(y  >> 8); b[1] = (uint8_t)(y  & 0xFF);
    b[2] = (uint8_t)(y2 >> 8); b[3] = (uint8_t)(y2 & 0xFF);
    lcd_data_buf(b, 4);

    lcd_cmd(0x2C);                       /* RAMWR */
}

/* 填充矩形（超出屏幕的部分由面板裁掉） */
void lcd_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    if (w == 0 || h == 0) return;
    lcd_set_window(x, y, w, h);

    uint32_t  n  = (uint32_t)w * (uint32_t)h;
    uint8_t   hi = (uint8_t)(color >> 8);
    uint8_t   lo = (uint8_t)(color & 0xFF);

    LCD_CS_LO();
    LCD_DC_HI();
    while (n--)
    {
        lcd_spi_xfer(hi);
        lcd_spi_xfer(lo);
    }
    LCD_CS_HI();
}

/* 整屏填色（320x240） */
void lcd_fill_screen(uint16_t color)
{
    lcd_fill_rect(0, 0, LCD_W, LCD_H, color);
}

/* 画一个像素 */
void lcd_draw_pixel(uint16_t x, uint16_t y, uint16_t color)
{
    lcd_set_window(x, y, 1, 1);
    uint8_t b[2] = { (uint8_t)(color >> 8), (uint8_t)(color & 0xFF) };
    lcd_data_buf(b, 2);
}

/* 行优先 RGB565 位图 -> 一个窗口、一次连续 SPI 突发；比逐像素快得多（TFT 文字用它） */
void lcd_blit(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint16_t *px)
{
    uint32_t n;
    if (w == 0 || h == 0) return;
    lcd_set_window(x, y, w, h);

    n = (uint32_t)w * (uint32_t)h;
    LCD_CS_LO();
    LCD_DC_HI();
    while (n--)
    {
        lcd_spi_xfer((uint8_t)(*px >> 8));
        lcd_spi_xfer((uint8_t)(*px & 0xFF));
        px++;
    }
    LCD_CS_HI();
}

/* 画水平线 */
void lcd_draw_hline(uint16_t x, uint16_t y, uint16_t w, uint16_t color)
{
    lcd_fill_rect(x, y, w, 1, color);
}

/* 画垂直线 */
void lcd_draw_vline(uint16_t x, uint16_t y, uint16_t h, uint16_t color)
{
    lcd_fill_rect(x, y, 1, h, color);
}

/* 画矩形边框（不填充） */
void lcd_draw_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    lcd_draw_hline(x,         y,         w, color);
    lcd_draw_hline(x,         y + h - 1, w, color);
    lcd_draw_vline(x,         y,         h, color);
    lcd_draw_vline(x + w - 1, y,         h, color);
}
