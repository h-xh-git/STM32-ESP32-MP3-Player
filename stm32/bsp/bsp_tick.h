#ifndef __BSP_TICK_H
#define __BSP_TICK_H

/*
 * bsp_tick.h - 1 kHz (1 ms) 系统节拍
 *
 * TIM7 更新中断累加, 不占用任何引脚, 与 board.c 里基于 SysTick 轮询的
 * delay_us()/delay_ms() 互不干扰。主循环、播放进度、超时判断都用它,
 * 避免用"主循环迭代次数"当时钟(那会随 SPI 刷新/阻塞而变慢)。
 */
#include <stdint.h>

void     tick_init(void);

/* 自 tick_init() 起的累计毫秒数, 约 49.7 天回绕 */
uint32_t tick_ms(void);

/* now - t0 的无符号差值, 正确处理回绕 */
uint32_t tick_elapsed(uint32_t t0);

#endif /* __BSP_TICK_H */
