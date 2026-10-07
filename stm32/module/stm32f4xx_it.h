/**
  ******************************************************************************
  * @file    Project/STM32F4xx_StdPeriph_Templates/stm32f4xx_it.h 
  * @author  MCD Application Team
  * @version V1.8.1
  * @date    27-January-2022
  * @brief   This file contains the headers of the interrupt handlers.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2016 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */

/*
 * stm32f4xx_it.h - 内核异常处理函数声明
 *
 * 作用：声明 Cortex-M4 的内核异常处理函数，供 startup_stm32f40xx.s 的向量表链接。
 * 硬件要点：这些是内核异常，不是外设中断；本工程的外设中断都在各自的 bsp 文件里
 *           （USART1 在 bsp\uart、USART2 在 bsp\uart2、SPI2/DMA/EXTI 在 bsp\spid、
 *           TIM7 节拍在 bsp\bsp_tick）。
 * 依赖：stm32f4xx.h（CMSIS + StdPeriph）。
 * 调用关系：由启动文件调用；各 bsp 不给它传参。
 */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __STM32F4xx_IT_H
#define __STM32F4xx_IT_H

#ifdef __cplusplus
 extern "C" {
#endif 

/* Includes ------------------------------------------------------------------*/
#include "stm32f4xx.h"

/* 内核异常处理函数（外设中断见各自的 bsp 文件）----------------------------- */
/* Exported types ------------------------------------------------------------*/
/* Exported constants --------------------------------------------------------*/
/* Exported macro ------------------------------------------------------------*/
/* Exported functions ------------------------------------------------------- */

void NMI_Handler(void);
void HardFault_Handler(void);
void MemManage_Handler(void);
void BusFault_Handler(void);
void UsageFault_Handler(void);
void SVC_Handler(void);
void DebugMon_Handler(void);
void PendSV_Handler(void);
void SysTick_Handler(void);

#ifdef __cplusplus
}
#endif

#endif /* __STM32F4xx_IT_H */

