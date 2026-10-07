/*
 * bsp_spid.c - 板间「下载存卡」专用 SPI 从机驱动（STM32F407 侧，SPI2 + DMA1）
 *
 * 设计要点（配合 bsp_spid.h 的协议说明读）：
 *   1) SPI2 从机，软件 NSS（SSM=1 + SSI=1），不接硬件 NSS，规避 F4 从机
 *      「NSS 拉低早于数据装载 -> 首字节丢失」的经典坑；CS(PB12) 只做 EXTI
 *      输入，用于事务边界对齐。
 *   2) RX/TX 各用一条 DMA（DMA1 Stream3/Stream4, Channel0），Normal 模式，
 *      每帧重新 Arm。**DMA 搬运期间 CPU 完全空闲**，2 KB 一帧在 8 MHz 下
 *      约 2 ms 不占 CPU，只有写 SD 卡的零点几毫秒占 CPU。
 *   3) 严格「一帧一应答」：RX DMA 传输完成中断只做三件事——停 RX 流、
 *      拉低 READY、置 s_frame_ready；真正的解析 + 写卡 + 组织应答放在主循环
 *      （spid_frame_done 里重新 Arm 两条 DMA 再拉高 READY）。因此不会出现
 *      主循环还没处理完就被下一帧覆盖的情况。
 *   4) CS 上升沿（事务结束）时若 RX 没收到完整 2064 B，说明主机中途放弃
 *      （例如 ESP32 复位），此时重新 Arm 让下一次事务自然对齐。
 *
 * 注意：本文件与 app/netdl.c 分工——这里只管「搬一帧的字节」，不管协议语义。
 */

#include "bsp_spid.h"
#include <stdio.h>

/* ---------------- 引脚/外设映射 ---------------- */
#define SPID_SPI            SPI2
#define SPID_SPI_CLK        RCC_APB1Periph_SPI2

#define SPID_GPIO           GPIOB
#define SPID_GPIO_CLK       RCC_AHB1Periph_GPIOB

#define SPID_SCK_PIN        GPIO_Pin_13
#define SPID_SCK_SRC        GPIO_PinSource13
#define SPID_MISO_PIN       GPIO_Pin_14
#define SPID_MISO_SRC       GPIO_PinSource14
#define SPID_MOSI_PIN       GPIO_Pin_15
#define SPID_MOSI_SRC       GPIO_PinSource15

#define SPID_CS_PIN         GPIO_Pin_12
#define SPID_CS_SRC         GPIO_PinSource12
#define SPID_RDY_PIN        GPIO_Pin_11

#define SPID_AF             GPIO_AF_SPI2

#define SPID_DMA            DMA1
#define SPID_DMA_CLK        RCC_AHB1Periph_DMA1
#define SPID_RX_STREAM      DMA1_Stream3
#define SPID_TX_STREAM      DMA1_Stream4
#define SPID_RX_CHANNEL     DMA_Channel_0      /* 参考手册：SPI2_RX = DMA1 Stream3 CH0 */
#define SPID_TX_CHANNEL     DMA_Channel_0      /* 参考手册：SPI2_TX = DMA1 Stream4 CH0 */
#define SPID_RX_IRQn        DMA1_Stream3_IRQn

#define SPID_RX_FLAGS       (DMA_FLAG_TCIF3 | DMA_FLAG_HTIF3 | DMA_FLAG_TEIF3 | \
                             DMA_FLAG_DMEIF3 | DMA_FLAG_FEIF3)
#define SPID_TX_FLAGS       (DMA_FLAG_TCIF4 | DMA_FLAG_HTIF4 | DMA_FLAG_TEIF4 | \
                             DMA_FLAG_DMEIF4 | DMA_FLAG_FEIF4)

#define SPID_CS_EXTI_LINE   EXTI_Line12
#define SPID_CS_IRQn        EXTI15_10_IRQn

/* ---------------- 缓冲（必须放主板 SRAM，CCM 不能给 DMA 用） ----------------
 * 工程分散加载把 *( .ccmram ) 放 0x10000000（CCM），其余 .bss 在 0x20000000
 * 主板 SRAM —— 这两个数组是普通 .bss，DMA1 可达。注意：CCM(0x10000000) 上的
 * 缓冲 DMA1 读写不到，这两个数组一旦挪进 .ccmram 段，SPI 就收不到数据。 */
static uint8_t s_rx[SPID_FRAME];
static uint8_t s_tx[SPID_FRAME];

static volatile uint8_t s_frame_ready = 0u;   /* RX 收满一帧，等主循环处理 */
static volatile uint8_t s_arming      = 0u;   /* 主循环正在重新 Arm，EXTI 别来捣乱 */
static uint32_t s_frames    = 0u;
static uint32_t s_errs      = 0u;
static uint32_t s_dma_errs  = 0u;

/* ------------------------------------------------------------------ */
/* 拉高 READY(PB11)：告诉 ESP32 两条 DMA 已就绪，可以发下一帧 */
static void spid_rdy_high(void)
{
    GPIO_SetBits(SPID_GPIO, SPID_RDY_PIN);
}

/* 拉低 READY：正在收/处理一帧，主机必须等待 */
static void spid_rdy_low(void)
{
    GPIO_ResetBits(SPID_GPIO, SPID_RDY_PIN);
}

/* 冲掉 SPI 里可能残留的字节/溢出标志，再重新装载两条 DMA 并启动 */
static void spid_dma_rearm(void)
{
    DMA_Cmd(SPID_TX_STREAM, DISABLE);
    DMA_Cmd(SPID_RX_STREAM, DISABLE);
    while (DMA_GetCmdStatus(SPID_TX_STREAM) != DISABLE) { }
    while (DMA_GetCmdStatus(SPID_RX_STREAM) != DISABLE) { }

    DMA_ClearFlag(SPID_TX_STREAM, SPID_TX_FLAGS);
    DMA_ClearFlag(SPID_RX_STREAM, SPID_RX_FLAGS);

    /* 清 RXNE / OVR（先读 DR 清 RXNE，再读 SR+DR 清 OVR） */
    (void)SPID_SPI->DR;
    (void)SPID_SPI->SR;
    (void)SPID_SPI->DR;

    DMA_SetCurrDataCounter(SPID_TX_STREAM, (uint16_t)SPID_FRAME);
    DMA_SetCurrDataCounter(SPID_RX_STREAM, (uint16_t)SPID_FRAME);

    DMA_Cmd(SPID_TX_STREAM, ENABLE);   /* TX 先开：TXE 为空，DMA 会预载第 1 个字节 */
    DMA_Cmd(SPID_RX_STREAM, ENABLE);
}

/* ------------------------------------------------------------------ */
/* 初始化 SPI2 从机：引脚/EXTI/DMA/NVIC，并 Arm 好第一帧（READY 拉高） */
void spid_init(void)
{
    GPIO_InitTypeDef  gpio;
    SPI_InitTypeDef   spi;
    DMA_InitTypeDef   dma;
    NVIC_InitTypeDef  nvic;
    EXTI_InitTypeDef  exti;

    /* ---- 时钟 ---- */
    RCC_AHB1PeriphClockCmd(SPID_GPIO_CLK | SPID_DMA_CLK, ENABLE);
    RCC_APB1PeriphClockCmd(SPID_SPI_CLK, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_SYSCFG, ENABLE);

    /* ---- SCK / MISO / MOSI: AF5 复用推挽，高速 ---- */
    GPIO_PinAFConfig(SPID_GPIO, SPID_SCK_SRC,  SPID_AF);
    GPIO_PinAFConfig(SPID_GPIO, SPID_MISO_SRC, SPID_AF);
    GPIO_PinAFConfig(SPID_GPIO, SPID_MOSI_SRC, SPID_AF);

    GPIO_StructInit(&gpio);
    gpio.GPIO_Pin   = SPID_SCK_PIN | SPID_MISO_PIN | SPID_MOSI_PIN;
    gpio.GPIO_Mode  = GPIO_Mode_AF;
    gpio.GPIO_OType = GPIO_OType_PP;
    gpio.GPIO_PuPd  = GPIO_PuPd_NOPULL;
    gpio.GPIO_Speed = GPIO_Speed_100MHz;
    GPIO_Init(SPID_GPIO, &gpio);

    /* ---- CS(PB12): 普通输入上拉（不接 SPI 硬件 NSS，只给 EXTI 用） ---- */
    GPIO_StructInit(&gpio);
    gpio.GPIO_Pin  = SPID_CS_PIN;
    gpio.GPIO_Mode = GPIO_Mode_IN;
    gpio.GPIO_PuPd = GPIO_PuPd_UP;
    GPIO_Init(SPID_GPIO, &gpio);

    /* ---- READY(PB11): 推挽输出，初值拉低（还没准备好） ---- */
    GPIO_StructInit(&gpio);
    gpio.GPIO_Pin   = SPID_RDY_PIN;
    gpio.GPIO_Mode  = GPIO_Mode_OUT;
    gpio.GPIO_OType = GPIO_OType_PP;
    gpio.GPIO_PuPd  = GPIO_PuPd_NOPULL;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(SPID_GPIO, &gpio);
    spid_rdy_low();

    /* ---- DMA：RX = 外设->内存，TX = 内存->外设，字节宽度，Normal 模式 ---- */
    DMA_DeInit(SPID_RX_STREAM);
    DMA_DeInit(SPID_TX_STREAM);

    DMA_StructInit(&dma);
    dma.DMA_Channel            = SPID_RX_CHANNEL;
    dma.DMA_PeripheralBaseAddr = (uint32_t)&(SPID_SPI->DR);
    dma.DMA_Memory0BaseAddr    = (uint32_t)s_rx;
    dma.DMA_DIR                = DMA_DIR_PeripheralToMemory;
    dma.DMA_BufferSize         = (uint16_t)SPID_FRAME;
    dma.DMA_PeripheralInc      = DMA_PeripheralInc_Disable;
    dma.DMA_MemoryInc          = DMA_MemoryInc_Enable;
    dma.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
    dma.DMA_MemoryDataSize     = DMA_MemoryDataSize_Byte;
    dma.DMA_Mode               = DMA_Mode_Normal;
    dma.DMA_Priority           = DMA_Priority_VeryHigh;
    dma.DMA_FIFOMode           = DMA_FIFOMode_Disable;
    dma.DMA_FIFOThreshold      = DMA_FIFOThreshold_HalfFull;
    dma.DMA_MemoryBurst        = DMA_MemoryBurst_Single;
    dma.DMA_PeripheralBurst    = DMA_PeripheralBurst_Single;
    DMA_Init(SPID_RX_STREAM, &dma);

    dma.DMA_Channel            = SPID_TX_CHANNEL;
    dma.DMA_PeripheralBaseAddr = (uint32_t)&(SPID_SPI->DR);
    dma.DMA_Memory0BaseAddr    = (uint32_t)s_tx;
    dma.DMA_DIR                = DMA_DIR_MemoryToPeripheral;
    dma.DMA_BufferSize         = (uint16_t)SPID_FRAME;
    dma.DMA_Priority           = DMA_Priority_High;
    DMA_Init(SPID_TX_STREAM, &dma);

    /* RX 只开「传输完成」中断；TX 不需要中断（长度相同，必然同时结束） */
    DMA_ITConfig(SPID_RX_STREAM, DMA_IT_TC, ENABLE);

    /* ---- SPI2 从机：Mode0，8 bit，软件 NSS 且内部选中 ---- */
    SPI_I2S_DeInit(SPID_SPI);
    SPI_StructInit(&spi);
    spi.SPI_Direction         = SPI_Direction_2Lines_FullDuplex;
    spi.SPI_Mode              = SPI_Mode_Slave;
    spi.SPI_DataSize          = SPI_DataSize_8b;
    spi.SPI_CPOL              = SPI_CPOL_Low;
    spi.SPI_CPHA              = SPI_CPHA_1Edge;
    spi.SPI_NSS               = SPI_NSS_Soft;      /* SSM = 1 */
    spi.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_2;   /* 从机模式下无效，随意 */
    spi.SPI_FirstBit          = SPI_FirstBit_MSB;
    spi.SPI_CRCPolynomial     = 7;
    SPI_Init(SPID_SPI, &spi);

    /* SSI = 1：软件 NSS 下把内部 NSS 置为「选中」，SPI 才会开始收发 */
    SPI_NSSInternalSoftwareConfig(SPID_SPI, SPI_NSSInternalSoft_Set);

    SPI_I2S_DMACmd(SPID_SPI, SPI_I2S_DMAReq_Rx | SPI_I2S_DMAReq_Tx, ENABLE);
    SPI_Cmd(SPID_SPI, ENABLE);

    /* ---- CS(PB12) 上升沿中断：事务结束，用于对齐检查 ---- */
    SYSCFG_EXTILineConfig(EXTI_PortSourceGPIOB, EXTI_PinSource12);
    EXTI_StructInit(&exti);
    exti.EXTI_Line    = SPID_CS_EXTI_LINE;
    exti.EXTI_Mode    = EXTI_Mode_Interrupt;
    exti.EXTI_Trigger = EXTI_Trigger_Rising;
    exti.EXTI_LineCmd = ENABLE;
    EXTI_Init(&exti);

    /* ---- NVIC ---- */
    nvic.NVIC_IRQChannel                   = SPID_RX_IRQn;
    nvic.NVIC_IRQChannelPreemptionPriority = 2;
    nvic.NVIC_IRQChannelSubPriority        = 0;
    nvic.NVIC_IRQChannelCmd                = ENABLE;
    NVIC_Init(&nvic);

    nvic.NVIC_IRQChannel                   = SPID_CS_IRQn;
    nvic.NVIC_IRQChannelPreemptionPriority = 3;
    nvic.NVIC_IRQChannelSubPriority        = 0;
    NVIC_Init(&nvic);

    /* ---- 首帧就绪 ---- */
    s_frame_ready = 0u;
    spid_dma_rearm();
    spid_rdy_high();

    printf("[SPID] SPI2 slave ready, frame=%u B\r\n", (unsigned)SPID_FRAME);
}

/* ------------------------------------------------------------------ */
/* 1 = 已收到完整一帧，等主循环处理（此时 READY 已是低） */
uint8_t spid_frame_ready(void)
{
    return s_frame_ready;
}

/* 刚收到的那一帧缓冲（2064 B，DMA 已停，主循环可安全读） */
const uint8_t *spid_rx_frame(void)
{
    return (const uint8_t *)s_rx;
}

/* 应答缓冲（2064 B，主循环填好后交给 spid_frame_done 发送） */
uint8_t *spid_tx_frame(void)
{
    return s_tx;
}

/* 处理完毕：重新 Arm TX/RX DMA 并拉高 READY，放行下一帧 */
void spid_frame_done(void)
{
    s_arming = 1u;
    spid_rdy_low();
    spid_dma_rearm();
    s_frame_ready = 0u;
    s_arming = 0u;
    spid_rdy_high();
}

/* 异常恢复：丢弃当前帧（计一次错误）后重新 Arm */
void spid_abort_frame(void)
{
    s_errs++;
    spid_frame_done();
}

/* ---------------- 诊断接口 ---------------- */
uint8_t  spid_ready(void)    { return (uint8_t)(GPIO_ReadOutputDataBit(SPID_GPIO, SPID_RDY_PIN) != Bit_RESET); }  /* 当前 READY 电平 */
uint32_t spid_frames(void)   { return s_frames; }     /* 收到的完整帧数 */
uint32_t spid_errs(void)     { return s_errs; }       /* 帧校验失败/CS 提前结束的次数 */
uint32_t spid_dma_errs(void) { return s_dma_errs; }   /* DMA 传输错误数 */

/* ------------------------------------------------------------------ */
/* RX 传输完成：停 RX 流、拉低 READY、通知主循环。
   这里刻意不解析内容——把解析和写 SD 卡放到主循环，避免在中断里长时间阻塞。 */
void DMA1_Stream3_IRQHandler(void)
{
    if (DMA_GetITStatus(SPID_RX_STREAM, DMA_IT_TCIF3) != RESET)
    {
        DMA_ClearITPendingBit(SPID_RX_STREAM, DMA_IT_TCIF3);
        DMA_Cmd(SPID_RX_STREAM, DISABLE);
        spid_rdy_low();
        s_frame_ready = 1u;
        s_frames++;
    }
    if (DMA_GetITStatus(SPID_RX_STREAM, DMA_IT_TEIF3) != RESET)
    {
        DMA_ClearITPendingBit(SPID_RX_STREAM, DMA_IT_TEIF3);
        s_dma_errs++;
    }
    if (DMA_GetITStatus(SPID_RX_STREAM, DMA_IT_DMEIF3) != RESET)
    {
        DMA_ClearITPendingBit(SPID_RX_STREAM, DMA_IT_DMEIF3);
        s_dma_errs++;
    }
}

/* CS 上升沿 = 一次事务结束。若 RX 没收到完整一帧，说明主机中途放弃
   （ESP32 复位/重连），重新 Arm 让下一次事务从 0 开始，避免长期错位。 */
void EXTI15_10_IRQHandler(void)
{
    if (EXTI_GetITStatus(SPID_CS_EXTI_LINE) != RESET)
    {
        EXTI_ClearITPendingBit(SPID_CS_EXTI_LINE);

        if ((s_arming == 0u) && (s_frame_ready == 0u) &&
            (GPIO_ReadInputDataBit(SPID_GPIO, SPID_CS_PIN) != Bit_RESET))
        {
            if ((DMA_GetCmdStatus(SPID_RX_STREAM) != DISABLE) &&
                (DMA_GetCurrDataCounter(SPID_RX_STREAM) != 0u))
            {
                s_errs++;                 /* 事务被提前结束，帧不完整 */
                spid_dma_rearm();
            }
        }
    }
}
