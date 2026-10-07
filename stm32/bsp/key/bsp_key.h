#ifndef __BSP_KEY_H
#define __BSP_KEY_H
#include <stdint.h>

/*
 * bsp_key.h - 5 个用户按键（PC0..PC4，低电平有效）
 *
 *   PC0 = KEY_1（播放/暂停）
 *   PC1 = KEY_2（上一曲）
 *   PC2 = KEY_3（下一曲）
 *   PC3 = KEY_4（音量+）
 *   PC4 = KEY_5（音量-）
 *
 * 引脚配置为输入 + 内部上拉：按下时把引脚短到 GND（低电平有效，无需外部电阻）。
 *
 * key_scan() 是非阻塞边沿检测：一次按下恰好返回一次 KEY_1..KEY_5，其余返回 KEY_NONE；
 * 只有原始电平真的变化时才花 15 ms 消抖，空闲时主循环零开销。
 */

#define KEY_NONE   0
#define KEY_1      1
#define KEY_2      2
#define KEY_3      3
#define KEY_4      4
#define KEY_5      5
#define KEY_N      5

void    key_init(void);
uint8_t key_scan(void);              /* KEY_1..KEY_5 on a fresh press, else KEY_NONE */
uint8_t key_raw(void);               /* bit i set = key (i+1) is down (no debounce) */

#endif /* __BSP_KEY_H */
