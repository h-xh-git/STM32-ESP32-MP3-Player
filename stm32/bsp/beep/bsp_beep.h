#ifndef __BSP_BEEP_H
#define __BSP_BEEP_H
#include <stdint.h>

/*
 * bsp_beep.h - PB8 按键提示音（有源蜂鸣器 / 低电平触发）
 *
 * 硬件：3 脚有源蜂鸣器模块(VCC/GND/IO)，IO 接 PB8。
 *   低电平触发：PB8 拉低 = 响，拉高 = 静音。
 *   模块内部自带振荡电路，不需要 MCU 输出方波载波，所以只占一个 GPIO，
 *   不占用任何定时器/通道（对比无源蜂鸣器需要 PWM）。
 *   引脚配置成"推挽输出 + 内部上拉"：复位期间 PB8 被内部上拉钳在高电平，
 *   上电不会先"嘀"一声。
 *
 *   开机默认静音（BEEP_DEFAULT_EN = 0）：要恢复提示音，把该宏改成 1u，或在运行期调用
 *   beep_set_enable(1)；调用点（main.c 的 beep_key()/beep_ms()）不需要改动。
 *
 * 用法（主循环）：
 *     beep_init();
 *     ...
 *     if (key_scan() != KEY_NONE) beep_key();   // 触发一次提示音
 *     beep_poll();                              // 每轮调用，到点自动停
 *
 * 全部非阻塞：beep_on() 只是记下到期时刻 tick_ms() + ms，由 beep_poll()
 * 在到点后把引脚拉高，不 delay、不阻塞主循环。
 */

#define BEEP_MS_KEY   30u   /* 普通按键 / 旋钮格数提示音 */
#define BEEP_MS_ACT   60u   /* 进入/退出歌单浮层、歌词页 */

/* 开机默认使能位：0 = 上电即静音（当前交付配置，蜂鸣器不会响）、1 = 上电即有提示音。
   运行期可随时 beep_set_enable(1)/(0) 打开/关闭，不需要改调用点、不需要改接线。 */
#define BEEP_DEFAULT_EN  0u

void    beep_init(void);
void    beep_on(uint16_t ms);        /* 强制响 ms 毫秒（不受使能开关限制） */
void    beep_off(void);              /* 立即静音 */
void    beep_poll(void);             /* 主循环每轮调用：到点关蜂鸣器 */
uint8_t beep_busy(void);             /* 1 = 正在响 */

void    beep_ms(uint16_t ms);        /* 按 ms 长响一次（受使能开关限制） */
void    beep_key(void);              /* 一次按键提示音 = beep_ms(BEEP_MS_KEY) */
void    beep_set_enable(uint8_t en); /* 0 = 全局静音 */
uint8_t beep_enabled(void);

#endif /* __BSP_BEEP_H */
