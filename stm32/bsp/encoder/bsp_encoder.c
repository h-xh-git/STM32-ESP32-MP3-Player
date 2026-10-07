/*
 * bsp_encoder.c - EC11 类旋转编码器音量旋钮
 *
 * 硬件：PC6 = TIM8_CH1(AF3)、PC7 = TIM8_CH2(AF3) 接 A/B 两相；PD12 为旋钮按下（输入上拉，按下接地）。
 *       用 TIM8 硬件编码器模式（TIM_EncoderMode_TI12）4 倍频计数，无中断、无 GPIO 轮询。
 * 时序：一机械格 = 4 个计数；enc_step() 用「本次计数 - 上次计数」的差分累积，余数留在 s_acc 里，
 *       主循环偶发卡顿最多一次报 ±4 档。
 * 依赖：bsp_encoder.h、board.h（delay_ms、GPIO/RCC 定义）。
 * 调用：main.c 主循环调 enc_step()（音量）与 enc_sw_scan()（按下）。
 */

#include "bsp_encoder.h"
#include "board.h"

/* 1 = 把 A/B 两相调换 (屏幕上的音量方向与旋钮手感相反时改成 1) */
#define ENC_DIR_INVERT      0

/* 旋钮按下时的消抖时间, 与 bsp_key.c 保持一致 */
#define ENC_SW_DEBOUNCE_MS  15

/* 旋钮按下脚: PD12。PC8 是 SDIO_D0 不能用, PC5 不配置 */
#define ENC_SW_PORT         GPIOD
#define ENC_SW_PIN          GPIO_Pin_12

/* 尚未凑够一档的零余计数 (有符号, 正负都留着) */
static int32_t  s_acc = 0;

/* 上次读取的定时器计数 (uint16, 硬件自行回绕) */
static uint16_t s_last_cnt = 0;

/* 消抖后的旋钮按下状态 */
static uint8_t  s_sw_last = 0;

uint8_t enc_sw_raw(void)
{
    return (GPIO_ReadInputDataBit(ENC_SW_PORT, ENC_SW_PIN) == Bit_RESET) ? 1u : 0u;
}

void enc_init(void)
{
    GPIO_InitTypeDef        gi;
    TIM_TimeBaseInitTypeDef tb;
    TIM_ICInitTypeDef       ic;

    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOC | RCC_AHB1Periph_GPIOD, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_TIM8, ENABLE);

    /* PC6/PC7 -> TIM8_CH1/CH2 (AF3), 复用推挽, 内部上拉 */
    GPIO_PinAFConfig(GPIOC, GPIO_PinSource6, GPIO_AF_TIM8);
    GPIO_PinAFConfig(GPIOC, GPIO_PinSource7, GPIO_AF_TIM8);

    gi.GPIO_Pin   = GPIO_Pin_6 | GPIO_Pin_7;
    gi.GPIO_Mode  = GPIO_Mode_AF;
    gi.GPIO_OType = GPIO_OType_PP;
    gi.GPIO_Speed = GPIO_Speed_50MHz;
    gi.GPIO_PuPd  = GPIO_PuPd_UP;
    GPIO_Init(GPIOC, &gi);

    /* PD12 = 旋钮按下, 普通输入 + 内部上拉 (与 5 个按键同款接法) */
    gi.GPIO_Pin   = ENC_SW_PIN;
    gi.GPIO_Mode  = GPIO_Mode_IN;
    gi.GPIO_OType = GPIO_OType_PP;
    gi.GPIO_Speed = GPIO_Speed_2MHz;
    gi.GPIO_PuPd  = GPIO_PuPd_UP;
    GPIO_Init(ENC_SW_PORT, &gi);

    /* 16 位向上计数, 不分频, 计数范围就是整个 0..65535 */
    TIM_TimeBaseStructInit(&tb);
    tb.TIM_Prescaler         = 0;
    tb.TIM_CounterMode       = TIM_CounterMode_Up;
    tb.TIM_Period            = 0xFFFF;
    tb.TIM_ClockDivision     = TIM_CKD_DIV1;
    tb.TIM_RepetitionCounter = 0;
    TIM_TimeBaseInit(TIM8, &tb);

    /* 硬件编码器模式: TI1 和 TI2 都计数 -> 每个机械格 4 个计数 */
    TIM_EncoderInterfaceConfig(TIM8, TIM_EncoderMode_TI12,
                               TIM_ICPolarity_Rising, TIM_ICPolarity_Rising);

    /* 输入滤波 (ICFilter=10, 约 6~12 us), 吞掉触点抖动产生的假边沿 */
    TIM_ICStructInit(&ic);
    ic.TIM_Channel     = TIM_Channel_1;
    ic.TIM_ICPolarity  = TIM_ICPolarity_Rising;
    ic.TIM_ICSelection = TIM_ICSelection_DirectTI;
    ic.TIM_ICPrescaler = TIM_ICPSC_DIV1;
    ic.TIM_ICFilter    = 10;
    TIM_ICInit(TIM8, &ic);

    ic.TIM_Channel     = TIM_Channel_2;
    TIM_ICInit(TIM8, &ic);

    /* 编码器模式下必须打开两个通道的输入捕获使能, 计数器才会动 */
    TIM_CCxCmd(TIM8, TIM_Channel_1, TIM_CCx_Enable);
    TIM_CCxCmd(TIM8, TIM_Channel_2, TIM_CCx_Enable);

    TIM_SetCounter(TIM8, 0);
    TIM_Cmd(TIM8, ENABLE);

    delay_ms(2);
    s_last_cnt = (uint16_t)TIM_GetCounter(TIM8);
    s_sw_last  = enc_sw_raw();      /* 上电就按住不算一次按键事件 */
}

int8_t enc_step(void)
{
    uint16_t now = (uint16_t)TIM_GetCounter(TIM8);
    int16_t  d   = (int16_t)(now - s_last_cnt);
    int32_t  steps;

    s_last_cnt = now;
    if (d == 0)
        return 0;

#if ENC_DIR_INVERT
    d = (int16_t)(-d);
#endif

    s_acc += (int32_t)d;

    steps = s_acc / ENC_COUNTS_PER_DETENT;      /* C99: 向零取整, 正负对称 */
    if (steps > 4)
        steps = 4;
    else if (steps < -4)
        steps = -4;

    s_acc -= steps * ENC_COUNTS_PER_DETENT;     /* 余数留下, 不丢半格 */

    return (int8_t)steps;
}

uint8_t enc_sw_scan(void)
{
    uint8_t raw = enc_sw_raw();

    if (raw == s_sw_last)
        return 0;

    delay_ms(ENC_SW_DEBOUNCE_MS);
    raw = enc_sw_raw();
    if (raw == s_sw_last)
        return 0;

    s_sw_last = raw;
    return raw;                 /* 只报"刚按下", 松开事件不上报 */
}
