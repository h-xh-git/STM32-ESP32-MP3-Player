/*
 * bsp_tick.c - 1 ms 系统节拍源
 *
 * TIM7 更新中断累加毫秒计数，不占用任何引脚。主循环、播放进度、超时判断、
 * 按键提示音定时都用它，避免用「主循环迭代次数」当时钟（那会随 SPI 刷新/阻塞变慢）。
 *
 * 时钟：TIM7 挂 APB1（PCLK1 = 42 MHz），定时器时钟 = 2 x PCLK1 = 84 MHz；
 *       84 MHz / 8400 = 10 kHz，再 / 10 = 1 kHz，即每 1 ms 一次更新中断。
 * 依赖：bsp_tick.h、stm32f4xx.h（StdPeriph 库）。
 * 调用：board.c 初始化时 tick_init()；其它模块用 tick_ms()/tick_elapsed() 取时间。
 */

#include "bsp_tick.h"
#include "stm32f4xx.h"

static volatile uint32_t s_ms = 0u;   /* 启动以来的毫秒数，只在 TIM7 中断里累加 */

/*
 * TIM7 挂在 APB1 (PCLK1 = 42 MHz), 定时器时钟 = 2 x PCLK1 = 84 MHz。
 * 84 MHz / 8400 = 10 kHz;  10 kHz / 10 = 1 kHz  ->  1 ms 一次更新中断。
 */
/* 初始化 TIM7 为 1 ms 更新中断并打开 NVIC（优先级低于 USART2，不打断串口收发） */
void tick_init(void)
{
    TIM_TimeBaseInitTypeDef tb;
    NVIC_InitTypeDef        nv;

    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM7, ENABLE);

    tb.TIM_Prescaler         = 8400u - 1u;
    tb.TIM_CounterMode       = TIM_CounterMode_Up;
    tb.TIM_Period            = 10u - 1u;
    tb.TIM_ClockDivision     = TIM_CKD_DIV1;
    tb.TIM_RepetitionCounter = 0u;
    TIM_TimeBaseInit(TIM7, &tb);

    TIM_ClearFlag(TIM7, TIM_FLAG_Update);
    TIM_ITConfig(TIM7, TIM_IT_Update, ENABLE);

    /* 优先级低于 USART2(1/0), 节拍中断不会打断串口收发 */
    nv.NVIC_IRQChannel                   = TIM7_IRQn;
    nv.NVIC_IRQChannelPreemptionPriority = 3u;
    nv.NVIC_IRQChannelSubPriority        = 0u;
    nv.NVIC_IRQChannelCmd                = ENABLE;
    NVIC_Init(&nv);

    TIM_Cmd(TIM7, ENABLE);
}

/* 当前毫秒计数（32 位，约 49.7 天回绕，跨回绕比较请用 tick_elapsed） */
uint32_t tick_ms(void) { return s_ms; }

/* 自 t0 起经过的毫秒数（无符号回绕安全） */
uint32_t tick_elapsed(uint32_t t0) { return (uint32_t)(s_ms - t0); }

/* 1 ms 更新中断：清标志后把毫秒计数加一 */
void TIM7_IRQHandler(void)
{
    if (TIM_GetITStatus(TIM7, TIM_IT_Update) != RESET)
    {
        TIM_ClearITPendingBit(TIM7, TIM_IT_Update);
        s_ms++;
    }
}
