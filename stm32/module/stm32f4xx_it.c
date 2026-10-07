/**
  ******************************************************************************
  * @file    Project/STM32F4xx_StdPeriph_Templates/stm32f4xx_it.c 
  * @author  MCD Application Team
  * @version V1.8.1
  * @date    27-January-2022
  * @brief   Main Interrupt Service Routines.
  *          This file provides template for all exceptions handler and 
  *          peripherals interrupt service routine.
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
 * stm32f4xx_it.c - Cortex-M4 内核异常处理（除 HardFault 外都是空壳）
 *
 * 作用：NMI/HardFault/MemManage/BusFault/UsageFault/SVC/DebugMon/PendSV/SysTick 的
 *       中断处理函数。除 HardFault（以及 MemManage/BusFault/UsageFault）停在死循环
 *       便于调试外，其余都是空实现。
 * 硬件要点：SysTick 不产生中断（没有使能 SysTick 中断），只被 board.c 当自由减
 *           计数器用于 delay_us()；本工程的 1 ms 节拍来自 TIM7 中断（在
 *           bsp\bsp_tick.c 的 TIM7_IRQHandler 里累加），不在这里。
 * 依赖：stm32f4xx_it.h -> stm32f4xx.h（CMSIS + StdPeriph）。
 * 调用关系：只由 startup_stm32f40xx.s 的向量表调用，不对外提供接口。
 */

/* Includes ------------------------------------------------------------------*/
#include "stm32f4xx_it.h"

/** @addtogroup Template_Project
  * @{
  */

/* Private typedef -----------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/
/* Private macro -------------------------------------------------------------*/
/* Private variables ---------------------------------------------------------*/
/* Private function prototypes -----------------------------------------------*/
/* Private functions ---------------------------------------------------------*/

/******************************************************************************/
/*            Cortex-M4 Processor Exceptions Handlers                         */
/******************************************************************************/

/**
  * @brief  This function handles NMI exception.
  * @param  None
  * @retval None
  */
void NMI_Handler(void)
{
}

/**
  * @brief  This function handles Hard Fault exception.
  * @param  None
  * @retval None
  */
void HardFault_Handler(void)
{
  /* Go to infinite loop when Hard Fault exception occurs */
  while (1)
  {
  }
}

/**
  * @brief  This function handles Memory Manage exception.
  * @param  None
  * @retval None
  */
void MemManage_Handler(void)
{
  /* Go to infinite loop when Memory Manage exception occurs */
  while (1)
  {
  }
}

/**
  * @brief  This function handles Bus Fault exception.
  * @param  None
  * @retval None
  */
void BusFault_Handler(void)
{
  /* Go to infinite loop when Bus Fault exception occurs */
  while (1)
  {
  }
}

/**
  * @brief  This function handles Usage Fault exception.
  * @param  None
  * @retval None
  */
void UsageFault_Handler(void)
{
  /* Go to infinite loop when Usage Fault exception occurs */
  while (1)
  {
  }
}

/**
  * @brief  This function handles SVCall exception.
  * @param  None
  * @retval None
  */
void SVC_Handler(void)
{
}

/**
  * @brief  This function handles Debug Monitor exception.
  * @param  None
  * @retval None
  */
void DebugMon_Handler(void)
{
}

/**
  * @brief  This function handles PendSVC exception.
  * @param  None
  * @retval None
  */
void PendSV_Handler(void)
{
}

/**
  * @brief  This function handles SysTick Handler.
  * @param  None
  * @retval None
  */
void SysTick_Handler(void)
{

}

/******************************************************************************/
/*                 STM32F4xx 外设中断处理函数                                  */
/*  本工程不把外设中断写在这里，而是放在各自的 bsp 文件中：USART1 在            */
/*  bsp/uart/bsp_uart.c，USART2 在 bsp/uart2/bsp_uart2.c，SPI2 的 DMA 与片选   */
/*  EXTI 在 bsp/spid/bsp_spid.c，1 ms 节拍的 TIM7 在 bsp/bsp_tick.c。           */
/******************************************************************************/

/**
  * @}
  */ 


