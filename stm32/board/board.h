/*
 * 立创开发板软硬件资料与相关扩展板软硬件资料官网全部开源
 * 开发板官网：www.lckfb.com
 * 技术支持常驻论坛，任何技术问题欢迎随时交流学习
 * 立创论坛：https://oshwhub.com/forum
 * 关注bilibili账号：【立创开发板】，掌握我们的最新动态！
 * 不靠卖板赚钱，以培养中国工程师为己任
 * 

 Change Logs:
 * Date           Author       Notes
 * 2024-03-07     LCKFB-LP    first version
 */

/*
 * board.h - 板级初始化与延时接口（立创天空星 STM32F407 开发板）
 *
 * 作用：声明上电最先调用的 board_init()，以及基于内核 SysTick 的微秒/毫秒延时。
 *       本文件与具体外设引脚无关，外设初始化都在 bsp_* 各模块里。
 * 硬件要点：中断向量表基址 VTOR = 0x08000000（Flash 启动），SysTick 时钟源 = HCLK。
 * 依赖：stm32f4xx.h（CMSIS + StdPeriph 库）。
 * 调用关系：main() 与各 bsp 外设初始化调用。
 */

#ifndef __BOARD_H__
#define __BOARD_H__

#include "stm32f4xx.h"

void board_init(void);
void delay_us(uint32_t _us);
void delay_ms(uint32_t _ms);

#endif
