#include "bsp_beep.h"
#include "bsp_tick.h"
#include "board.h"

/*
 * bsp_beep.c - PB8 按键提示音（有源蜂鸣器 / 低电平触发）
 *
 * 硬件：3 脚有源蜂鸣器模块（VCC/GND/IO），IO 接 PB8；PB8 拉低 = 响，拉高 = 静音。
 *       模块自带振荡电路，不需要 MCU 输出载波，只占一个 GPIO，不占定时器/通道。
 * 时序：beep_on() 只记下到期时刻 tick_ms() + ms，beep_poll() 到点后把引脚拉高，
 *       全程不 delay、不阻塞主循环。
 * 依赖：bsp_beep.h、bsp_tick.h（tick_ms）、board.h（GPIO/RCC 定义）。
 * 调用：key_scan() 检测到按键后由 main.c 调 beep_key()/beep_ms()；主循环每轮调 beep_poll()。
 */

#define BEEP_PORT   GPIOB
#define BEEP_PIN    GPIO_Pin_8

static uint8_t  s_en;        /* 1 = 允许提示音（开机默认 BEEP_DEFAULT_EN = 0，即静音）*/
static uint8_t  s_on;        /* 1 = 蜂鸣器正在响                  */
static uint32_t s_until;     /* 到期时刻 tick_ms()（仅 s_on 有效）*/

static void beep_hw(uint8_t on)
{
    if (on)
        GPIO_ResetBits(BEEP_PORT, BEEP_PIN);   /* 低电平 = 响 */
    else
        GPIO_SetBits(BEEP_PORT, BEEP_PIN);     /* 高电平 = 静音 */
}

void beep_init(void)
{
    GPIO_InitTypeDef gi;

    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOB, ENABLE);

    /* 先关时钟再配置引脚，保证复位后的第一段输出就是"静音"电平 */
    gi.GPIO_Pin   = BEEP_PIN;
    gi.GPIO_Mode  = GPIO_Mode_OUT;
    gi.GPIO_OType = GPIO_OType_PP;
    gi.GPIO_Speed = GPIO_Speed_2MHz;
    gi.GPIO_PuPd  = GPIO_PuPd_UP;      /* 复位期/浮空期被上拉，不会误响 */
    GPIO_Init(BEEP_PORT, &gi);

    s_en    = BEEP_DEFAULT_EN;   /* = 0：开机即静音（要提示音改成 1u）*/
    s_on    = 0u;
    s_until = 0u;
    beep_hw(0u);
}

void beep_off(void)
{
    s_on = 0u;
    beep_hw(0u);
}

void beep_on(uint16_t ms)
{
    if (ms == 0u)
        ms = 1u;

    beep_hw(1u);
    s_on    = 1u;
    s_until = tick_ms() + (uint32_t)ms;
}

void beep_poll(void)
{
    if (s_on == 0u)
        return;

    /* tick_ms() 是 32 位毫秒计数，用有符号差比较可安全处理回绕 */
    if ((int32_t)(tick_ms() - s_until) >= 0)
        beep_off();
}

uint8_t beep_busy(void)
{
    return s_on;
}

void beep_ms(uint16_t ms)
{
    if (s_en == 0u)
        return;

    beep_on(ms);
}

void beep_key(void)
{
    beep_ms(BEEP_MS_KEY);
}

void beep_set_enable(uint8_t en)
{
    s_en = (en != 0u) ? 1u : 0u;

    if (s_en == 0u)
        beep_off();
}

uint8_t beep_enabled(void)
{
    return s_en;
}
