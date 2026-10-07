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
 * board.c - 板级初始化与 SysTick 延时实现（立创天空星 STM32F407 开发板）
 *
 * 作用：设置中断向量表基址与内核 SysTick，实现全工程共用的 delay_us()/delay_ms()。
 * 硬件要点：SCB->VTOR = 0x08000000 & 0x3FFFFF80（Flash 启动，向量表 512 B 对齐；
 *           若用 VECT_TAB_RAM 则为 0x10000000）；SysTick 时钟源 = HCLK，
 *           LOAD = 0xFFFF 且不开中断，delay_us() 靠轮询 SysTick->VAL 的差值累加
 *           时钟数计时（ticks = us * SystemCoreClock / 1000000），
 *           delay_ms() 就是 delay_us(ms * 1000)。
 * 依赖：board.h -> stm32f4xx.h（CMSIS + StdPeriph 库），不依赖任何 bsp。
 * 调用关系：main() 最先调用 board_init()；各 bsp 外设初始化用 delay_us/delay_ms
 *           做上电时序。
 */

#include <board.h>

/**
 * This function will initial stm32 board.
 */
void board_init(void)
{
    /* NVIC Configuration */
#define NVIC_VTOR_MASK              0x3FFFFF80
#ifdef  VECT_TAB_RAM
    /* Set the Vector Table base location at 0x10000000 */
    SCB->VTOR  = (0x10000000 & NVIC_VTOR_MASK);
#else  /* VECT_TAB_FLASH  */
    /* Set the Vector Table base location at 0x08000000 */
    SCB->VTOR  = (0x08000000 & NVIC_VTOR_MASK);
#endif

    SysTick_CLKSourceConfig(SysTick_CLKSource_HCLK);  /* 时钟源 = HCLK，与 SystemCoreClock 一致 */
    SysTick->LOAD = 0xFFFFu;                          /* 重装载值：不开中断，只当自由减计数器 */
    SysTick->CTRL |= SysTick_CTRL_ENABLE_Msk;         /* 使能计数 */

}

/**
 -  @brief  用内核的 systick 实现的微妙延时
 -  @note   None
 -  @param  _us:要延时的us数
 -  @retval None
*/
void delay_us(uint32_t _us)
{
    uint32_t ticks;
    uint32_t told, tnow, tcnt = 0;

    // 计算需要的时钟数 = 延迟微秒数 * 每微秒的时钟数
    ticks = _us * (SystemCoreClock / 1000000);

    // 获取当前的SysTick值
    told = SysTick->VAL;

    while (1)
    {
        // 重复刷新获取当前的SysTick值
        tnow = SysTick->VAL;

        if (tnow != told)
        {
            if (tnow < told)
                tcnt += told - tnow;
            else
                tcnt += SysTick->LOAD - tnow + told;

            told = tnow;

            // 如果达到了需要的时钟数，就退出循环
            if (tcnt >= ticks)
                break;
        }
    }
}

/**
 -  @brief  调用用内核的 systick 实现的毫秒延时
 -  @note   None
 -  @param  _ms:要延时的ms数
 -  @retval None
*/
void delay_ms(uint32_t _ms) { delay_us(_ms * 1000); }
