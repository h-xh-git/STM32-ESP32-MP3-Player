/*
 * bsp_uart2.c - 与 ESP32-S3 的板间串口链路（USART2，PA2=TX / PA3=RX，AF7）
 *
 * 作用：双向字节通道，承载 bsp_proto 的帧 [0xAA][0x55][type][len LE][payload][crc16 LE]，
 *       默认 1 Mbps；接收走 RXNE 中断 + 环形缓冲，发送走 TXE 中断 + 环形缓冲，全非阻塞。
 * 缓冲：RX 环 2048 B（uart2_getc 取走）、TX 环 4096 B（uart2_putc 投入）；
 *       环形缓冲留 1 字节区分空/满，环满时静默丢字节并累加 drops 计数（诊断用，不阻塞主循环）。
 * 依赖：bsp_uart2.h、stm32f4xx.h（StdPeriph 库，库文件不改动）。
 * 调用：main.c 初始化 uart2_init(1000000)；上层用 bsp_proto 收发，不经本文件的裸接口。
 */

#include "bsp_uart2.h"

#define UART2_RX_BUF_SIZE 2048u   /* RX 环：中断里写 head，主循环读 tail */
#define UART2_TX_BUF_SIZE 4096u   /* TX 环：主循环写 tail，中断里读 head */

static volatile uint8_t  s_rxbuf[UART2_RX_BUF_SIZE];
static volatile uint16_t s_rx_head = 0;   /* 中断写入位置 */
static volatile uint16_t s_rx_tail = 0;   /* 主循环读取位置 */

static volatile uint8_t  s_txbuf[UART2_TX_BUF_SIZE];
static volatile uint16_t s_tx_head = 0;   /* 中断读取位置 */
static volatile uint16_t s_tx_tail = 0;   /* 主循环写入位置 */
static volatile uint32_t s_tx_drops = 0;  /* TX 环满时丢掉的字节数 */
static volatile uint32_t s_rx_drops = 0;  /* RX 环满时静默丢掉的字节数 */

/* 初始化 USART2 为 8N1 + RXNE 中断（PA2/PA3 复用推挽上拉），并清空收发环形缓冲 */
void uart2_init(uint32_t baud)
{
    GPIO_InitTypeDef  GPIO_InitStructure;
    USART_InitTypeDef USART_InitStructure;
    NVIC_InitTypeDef  NVIC_InitStructure;

    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART2, ENABLE);

    GPIO_PinAFConfig(GPIOA, GPIO_PinSource2, GPIO_AF_USART2);
    GPIO_PinAFConfig(GPIOA, GPIO_PinSource3, GPIO_AF_USART2);

    GPIO_StructInit(&GPIO_InitStructure);
    GPIO_InitStructure.GPIO_Pin   = GPIO_Pin_2;
    GPIO_InitStructure.GPIO_Mode   = GPIO_Mode_AF;
    GPIO_InitStructure.GPIO_OType  = GPIO_OType_PP;
    GPIO_InitStructure.GPIO_PuPd   = GPIO_PuPd_UP;
    GPIO_InitStructure.GPIO_Speed  = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    GPIO_StructInit(&GPIO_InitStructure);
    GPIO_InitStructure.GPIO_Pin   = GPIO_Pin_3;
    GPIO_InitStructure.GPIO_Mode   = GPIO_Mode_AF;
    GPIO_InitStructure.GPIO_OType  = GPIO_OType_PP;
    GPIO_InitStructure.GPIO_PuPd   = GPIO_PuPd_UP;
    GPIO_InitStructure.GPIO_Speed  = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    USART_StructInit(&USART_InitStructure);
    USART_InitStructure.USART_BaudRate            = baud;
    USART_InitStructure.USART_WordLength           = USART_WordLength_8b;
    USART_InitStructure.USART_StopBits            = USART_StopBits_1;
    USART_InitStructure.USART_Parity               = USART_Parity_No;
    USART_InitStructure.USART_Mode                 = USART_Mode_Rx | USART_Mode_Tx;
    USART_InitStructure.USART_HardwareFlowControl  = USART_HardwareFlowControl_None;
    USART_Init(USART2, &USART_InitStructure);

    USART_ClearFlag(USART2, USART_FLAG_RXNE);
    s_rx_head = 0u;
    s_rx_tail = 0u;
    s_tx_head = 0u;
    s_tx_tail = 0u;
    s_tx_drops = 0u;
    USART_ITConfig(USART2, USART_IT_RXNE, ENABLE);
    USART_Cmd(USART2, ENABLE);

    NVIC_InitStructure.NVIC_IRQChannel                   = USART2_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority  = 1;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority         = 0;
    NVIC_InitStructure.NVIC_IRQChannelCmd                 = ENABLE;
    NVIC_Init(&NVIC_InitStructure);
}

/* 非阻塞发送：把字节投入 TX 环，由 TXE 中断送出；只有环满才丢字节并累加 s_tx_drops */
void uart2_putc(uint8_t c)
{
    uint16_t next;
    uint32_t prim;

    next = (uint16_t)((s_tx_tail + 1u) % UART2_TX_BUF_SIZE);
    if (next == s_tx_head)
    {
        s_tx_drops++;
        return;
    }

    prim = __get_PRIMASK();
    __disable_irq();
    s_txbuf[s_tx_tail] = c;
    s_tx_tail = next;
    USART_ITConfig(USART2, USART_IT_TXE, ENABLE);
    if (prim == 0u) __enable_irq();
}

/* TX 环还能再投多少字节 */
uint16_t uart2_tx_free(void)
{
    int32_t used = (int32_t)s_tx_tail - (int32_t)s_tx_head;
    if (used < 0) used += (int32_t)UART2_TX_BUF_SIZE;
    return (uint16_t)((int32_t)UART2_TX_BUF_SIZE - 1 - used);
}

/* 因 TX 环满而丢弃的累计字节数 */
uint32_t uart2_tx_drops(void)
{
    return s_tx_drops;
}

/* 因 RX 环满而丢弃的累计字节数（0 表示链路健康） */
uint32_t uart2_rx_drops(void)
{
    return s_rx_drops;
}

/* 非阻塞取字节：返回 1 = 取到，0 = RX 环空 */
int uart2_getc(uint8_t *out_c)
{
    if (s_rx_head == s_rx_tail) return 0;
    *out_c = s_rxbuf[s_rx_tail];
    s_rx_tail = (uint16_t)((s_rx_tail + 1u) % UART2_RX_BUF_SIZE);
    return 1;
}

/* RX 环中待读字节数 */
uint16_t uart2_available(void)
{
    int32_t n = (int32_t)s_rx_head - (int32_t)s_rx_tail;
    if (n < 0) n += (int32_t)UART2_RX_BUF_SIZE;
    return (uint16_t)n;
}

/* USART2 中断：RXNE 收字节入环（环满累加 s_rx_drops）、清 ORE 溢出标志、TXE 续发下一字节 */
void USART2_IRQHandler(void)
{
    if (USART_GetFlagStatus(USART2, USART_FLAG_RXNE) != RESET)
    {
        uint8_t  b   = (uint8_t)(USART2->DR & 0xFFu);
        uint16_t next = (uint16_t)((s_rx_head + 1u) % UART2_RX_BUF_SIZE);
        if (next != s_rx_tail)
        {
            s_rxbuf[s_rx_head] = b;
            s_rx_head = next;
        }
        else
        {
            s_rx_drops++;      /* RX 环满：丢字节（诊断用，非 0 就是被冲爆了） */
        }
    }
    if (USART_GetFlagStatus(USART2, USART_FLAG_ORE) != RESET)
    {
        (void)USART2->SR;
        (void)USART2->DR;
    }
    if (USART_GetITStatus(USART2, USART_IT_TXE) != RESET)
    {
        if (s_tx_head != s_tx_tail)
        {
            USART_SendData(USART2, s_txbuf[s_tx_head]);
            s_tx_head = (uint16_t)((s_tx_head + 1u) % UART2_TX_BUF_SIZE);
        }
        else
        {
            USART_ITConfig(USART2, USART_IT_TXE, DISABLE);
        }
    }
}
