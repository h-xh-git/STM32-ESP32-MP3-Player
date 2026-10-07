/*
 * bsp_key.c - 5 个用户按键（PC0..PC4，低电平有效）
 *
 * 硬件：PC0 = KEY_1(播放/暂停)、PC1 = KEY_2(上一曲)、PC2 = KEY_3(下一曲)、
 *       PC3 = KEY_4(音量+)、PC4 = KEY_5(音量-)；输入 + 内部上拉，按下把引脚短到 GND。
 * 时序：key_scan() 是非阻塞边沿检测，只在原始电平变化时才花 15 ms 消抖；一次按下只报一次。
 * 依赖：bsp_key.h、board.h（delay_ms 与 GPIO 定义）。
 * 调用：主循环每轮调用 key_scan()，由 main.c 分发。
 */

#include "bsp_key.h"
#include "board.h"

typedef struct {
    GPIO_TypeDef *port;
    uint16_t      pin;
} key_pin_t;

static const key_pin_t key_pins[KEY_N] =
{
    { GPIOC, GPIO_Pin_0 },      /* KEY_1 PLAY   */
    { GPIOC, GPIO_Pin_1 },      /* KEY_2 PREV   */
    { GPIOC, GPIO_Pin_2 },      /* KEY_3 NEXT   */
    { GPIOC, GPIO_Pin_3 },      /* KEY_4 VOL+   */
    { GPIOC, GPIO_Pin_4 },      /* KEY_5 VOL-   */
};

/* 消抖后的按键状态：bit i = 1 表示第 (i+1) 个键当前处于按下状态 */
static uint8_t key_last = 0;

uint8_t key_raw(void)
{
    uint8_t v = 0;
    uint8_t i;

    for (i = 0; i < KEY_N; i++)
        if (GPIO_ReadInputDataBit(key_pins[i].port, key_pins[i].pin) == Bit_RESET)
            v |= (uint8_t)(1u << i);

    return v;
}

void key_init(void)
{
    GPIO_InitTypeDef gi;

    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOC, ENABLE);

    gi.GPIO_Pin   = GPIO_Pin_0 | GPIO_Pin_1 | GPIO_Pin_2 | GPIO_Pin_3 | GPIO_Pin_4;
    gi.GPIO_Mode  = GPIO_Mode_IN;
    gi.GPIO_OType = GPIO_OType_PP;
    gi.GPIO_Speed = GPIO_Speed_2MHz;
    gi.GPIO_PuPd  = GPIO_PuPd_UP;
    GPIO_Init(GPIOC, &gi);

    delay_ms(2);
    key_last = key_raw();       /* 上电时就按住的按键不算一次按键事件 */
}

uint8_t key_scan(void)
{
    uint8_t raw = key_raw();
    uint8_t pressed;
    uint8_t i;

    if (raw == key_last)
        return KEY_NONE;

    delay_ms(15);               /* 只有电平真的变了才消抖 */
    raw = key_raw();
    if (raw == key_last)
        return KEY_NONE;

    pressed  = (uint8_t)(raw & (uint8_t)(~key_last));   /* 刚按下的位 */
    key_last = raw;

    for (i = 0; i < KEY_N; i++)
        if (pressed & (uint8_t)(1u << i))
            return (uint8_t)(i + 1);

    return KEY_NONE;
}
