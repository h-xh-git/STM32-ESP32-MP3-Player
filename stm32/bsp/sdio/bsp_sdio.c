#include "bsp_sdio.h"
#include "board.h"
#include <stdio.h>
#include <string.h>

/*
 * bsp_sdio.c - STM32F407 SDIO 4 位 SD/TF 卡底层驱动（轮询 + 单块）
 *
 * 作用：卡上电初始化（CMD0/CMD8/ACMD41/CMD2/3/9/7/ACMD6）、单块读写
 *       （CMD17/CMD24）以及卡在位检测，给 FatFS 的 diskio.c 当底层。
 *
 * 硬件：SDIO 4 位模式 —— PC8(D0) PC9(D1) PC10(D2) PC11(D3) PC12(CK)
 *       PD2(CMD)，全部复用 AF12；PD3 是卡在位检测输入（上拉，插好为低）。
 *       上电用 400 kHz 1 位识别，识别完成后切 12 MHz 4 位。
 *
 * 协议/时序要点：
 *   1) 数据路径(DPSM)必须在发命令之前使能，否则卡已开始送数据而 SDIO 还没
 *      准备好接收 -> 读回垃圾 -> FatFS 报 FR_NO_FILESYSTEM。
 *   2) 不用 DMA、不用多块传输/CMD12：轮询 RX/TX FIFO + 单块循环。12 MHz
 *      4 位下 512 B 约 85 us，对 40 KB/s 的 MP3 播放绰绰有余。
 *   3) 单块读偶发 DATAEND-early，读侧对同一块最多重试 3 次（sd_read_blocks）。
 *
 * 依赖：STM32F4xx 标准外设库（SDIO/GPIO/RCC）+ bsp_tick 的 delay_ms。
 * 调用者：FatFS 的 diskio.c（disk_initialize/disk_read/disk_write）。
 */

#define SDIO_CLK_DIV_INIT      0x76u          /* 400 kHz @48 MHz */
#define SDIO_CLK_DIV_XFER      0x02u          /* 12 MHz @48 MHz（轮询留余量） */

#define SD_CMD_TIMEOUT         0x100000UL
#define SD_ACMD41_RETRIES      50u
#define SD_DATA_TIMEOUT        0x2000000UL

#define OCR_HCS                (1UL << 30)
#define OCR_BUSY               (1UL << 31)
#define OCR_CCS                (1UL << 30)
#define OCR_27_36              0x00FF8000UL

static volatile uint8_t  s_init_ok = 0;
static uint32_t          s_rd_retry = 0;   /* 单块读重试次数（诊断） */
static uint32_t          s_drain_log = 0;  /* DATAEND 后补取字的日志次数 */
static uint8_t           s_type    = SD_TYPE_UNKNOWN;
static uint32_t          s_rca     = 0;
static uint32_t          s_blocks  = 0;

/* ------------------------------------------------------------------ */
/* SDIO 引脚：PC8..PC12/PD2 复用 AF12，PD3 作卡在位输入（上拉） */
static void sd_gpio_init(void)
{
    GPIO_InitTypeDef gi;
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_SDIO, ENABLE);
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOC | RCC_AHB1Periph_GPIOD, ENABLE);

    GPIO_PinAFConfig(GPIOC, GPIO_PinSource8,  GPIO_AF_SDIO);
    GPIO_PinAFConfig(GPIOC, GPIO_PinSource9,  GPIO_AF_SDIO);
    GPIO_PinAFConfig(GPIOC, GPIO_PinSource10, GPIO_AF_SDIO);
    GPIO_PinAFConfig(GPIOC, GPIO_PinSource11, GPIO_AF_SDIO);
    GPIO_PinAFConfig(GPIOC, GPIO_PinSource12, GPIO_AF_SDIO);
    GPIO_PinAFConfig(GPIOD, GPIO_PinSource2,  GPIO_AF_SDIO);

    GPIO_StructInit(&gi);
    gi.GPIO_Mode  = GPIO_Mode_AF;
    gi.GPIO_OType = GPIO_OType_PP;
    gi.GPIO_PuPd  = GPIO_PuPd_UP;
    gi.GPIO_Speed = GPIO_Speed_50MHz;
    gi.GPIO_Pin = GPIO_Pin_8 | GPIO_Pin_9 | GPIO_Pin_10 | GPIO_Pin_11 | GPIO_Pin_12;
    GPIO_Init(GPIOC, &gi);
    gi.GPIO_Pin = GPIO_Pin_2;
    GPIO_Init(GPIOD, &gi);

    gi.GPIO_Mode = GPIO_Mode_IN;
    gi.GPIO_OType = GPIO_OType_PP;
    gi.GPIO_PuPd  = GPIO_PuPd_UP;
    gi.GPIO_Speed = GPIO_Speed_2MHz;
    gi.GPIO_Pin = GPIO_Pin_3;
    GPIO_Init(GPIOD, &gi);
}

/* 配 SDIO 时钟分频与总线宽度（DIV_INIT=400 kHz 1 位 -> DIV_XFER=12 MHz 4 位） */
static void sd_clk_config(uint8_t clkdiv, uint32_t buswide)
{
    SDIO_InitTypeDef si;
    si.SDIO_ClockEdge           = SDIO_ClockEdge_Rising;
    si.SDIO_ClockBypass         = SDIO_ClockBypass_Disable;
    si.SDIO_ClockPowerSave      = SDIO_ClockPowerSave_Disable;
    si.SDIO_BusWide             = buswide;
    si.SDIO_HardwareFlowControl = SDIO_HardwareFlowControl_Disable;
    si.SDIO_ClockDiv            = clkdiv;
    SDIO_Init(&si);
}

/* ------------------------------------------------------------------ */
/* 发一条命令并等响应标志；超时用循环计数（SD_CMD_TIMEOUT）实现 */
static sd_err_t sd_cmd(uint8_t idx, uint32_t arg, uint32_t resp_type)
{
    SDIO_CmdInitTypeDef c;
    uint32_t to = SD_CMD_TIMEOUT;

    SDIO_ClearFlag(SDIO_FLAG_CCRCFAIL | SDIO_FLAG_CTIMEOUT |
                   SDIO_FLAG_CMDREND | SDIO_FLAG_CMDSENT);

    c.SDIO_Argument  = arg;
    c.SDIO_CmdIndex  = idx;
    c.SDIO_Response  = resp_type;
    c.SDIO_Wait      = SDIO_Wait_No;
    c.SDIO_CPSM       = SDIO_CPSM_Enable;
    SDIO_SendCommand(&c);

    if (resp_type == SDIO_Response_No)
    {
        while (SDIO_GetFlagStatus(SDIO_FLAG_CMDSENT) == RESET)
        {
            if (to-- == 0u) return SD_ERR_CMD_TIMEOUT;
        }
    }
    else
    {
        while (1)
        {
            if (SDIO_GetFlagStatus(SDIO_FLAG_CMDREND) != RESET) break;
            if (SDIO_GetFlagStatus(SDIO_FLAG_CTIMEOUT) != RESET) return SD_ERR_CMD_TIMEOUT;
            if (SDIO_GetFlagStatus(SDIO_FLAG_CCRCFAIL) != RESET) return SD_ERR_CMD_CRC;
            if (to-- == 0u) return SD_ERR_CMD_TIMEOUT;
        }
    }
    return SD_OK;
}

/* 取 R1/R6/R7 响应（RESP1 寄存器） */
static uint32_t sd_resp1(void) { return SDIO_GetResponse(SDIO_RESP1); }

/* ACMD41：CMD55 + CMD41 并轮询 OCR 忙位，SD 上电初始化最后一步 */
static sd_err_t sd_app_cmd_41(uint32_t arg)
{
    SDIO_CmdInitTypeDef c;
    uint32_t to = SD_CMD_TIMEOUT;
    sd_err_t e;

    e = sd_cmd(55u, s_rca, SDIO_Response_Short);
    if (e != SD_OK) return e;

    SDIO_ClearFlag(SDIO_FLAG_CCRCFAIL | SDIO_FLAG_CTIMEOUT |
                   SDIO_FLAG_CMDREND | SDIO_FLAG_CMDSENT);
    c.SDIO_Argument = arg; c.SDIO_CmdIndex = 41u;
    c.SDIO_Response = SDIO_Response_Short; c.SDIO_Wait = SDIO_Wait_No;
    c.SDIO_CPSM = SDIO_CPSM_Enable;
    SDIO_SendCommand(&c);

    while (1)
    {
        if (SDIO_GetFlagStatus(SDIO_FLAG_CMDREND) != RESET) return SD_OK;
        if (SDIO_GetFlagStatus(SDIO_FLAG_CCRCFAIL) != RESET)
        {
            SDIO_ClearFlag(SDIO_FLAG_CCRCFAIL);
            return SD_OK;                 /* R3: CRC 无效属正常 */
        }
        if (SDIO_GetFlagStatus(SDIO_FLAG_CTIMEOUT) != RESET) return SD_ERR_CMD_TIMEOUT;
        if (to-- == 0u) return SD_ERR_CMD_TIMEOUT;
    }
}

/* 通用应用命令：先发 CMD55 前缀，再发指定的 ACMD */
static sd_err_t sd_app_cmd(uint8_t acmd_idx, uint32_t acmd_arg)
{
    sd_err_t e = sd_cmd(55u, s_rca, SDIO_Response_Short);
    if (e != SD_OK) return e;
    return sd_cmd(acmd_idx, acmd_arg, SDIO_Response_Short);
}

/* ------------------------------------------------------------------ */
/* 数据路径：必须在发命令之前调用                                       */
/* ------------------------------------------------------------------ */
static void sd_data_start(uint32_t len, uint8_t read)
{
    SDIO_DataInitTypeDef d;

    /* 上一次异常传输可能把字留在 RX FIFO 里 -> 新传输前先清干净 */
    {
        uint32_t g = 0u;
        while ((SDIO_GetFlagStatus(SDIO_FLAG_RXDAVL) != RESET) && (g < 64u))
        { (void)SDIO_ReadData(); g++; }
    }

    SDIO_ClearFlag(SDIO_FLAG_DCRCFAIL | SDIO_FLAG_DTIMEOUT | SDIO_FLAG_RXOVERR |
                   SDIO_FLAG_TXUNDERR | SDIO_FLAG_DATAEND | SDIO_FLAG_DBCKEND |
                   SDIO_FLAG_STBITERR);
    d.SDIO_DataTimeOut    = 0x0FFFFFFFu;
    d.SDIO_DataLength     = len;
    d.SDIO_DataBlockSize  = SDIO_DataBlockSize_512b;
    d.SDIO_TransferDir    = read ? SDIO_TransferDir_ToSDIO : SDIO_TransferDir_ToCard;
    d.SDIO_TransferMode   = SDIO_TransferMode_Block;
    d.SDIO_DPSM           = SDIO_DPSM_Enable;
    SDIO_DataConfig(&d);
}

/* 关数据路径(DPSM)，结束一次数据阶段 */
static void sd_data_stop(void)
{
    SDIO_DataInitTypeDef d;
    d.SDIO_DataTimeOut    = 0u;
    d.SDIO_DataLength     = 0u;
    d.SDIO_DataBlockSize  = SDIO_DataBlockSize_1b;
    d.SDIO_TransferDir    = SDIO_TransferDir_ToCard;
    d.SDIO_TransferMode   = SDIO_TransferMode_Block;
    d.SDIO_DPSM           = SDIO_DPSM_Disable;
    SDIO_DataConfig(&d);
}

/* 轮询 RX FIFO 收 len 字节（len 为 4 的倍数，buf 4 字节对齐） */
static sd_err_t sd_fifo_read(uint8_t *buf, uint32_t len)
{
    uint32_t *w    = (uint32_t *)buf;
    uint32_t need  = len / 4u;
    uint32_t got   = 0u;
    uint32_t spin  = 0u;

    while (got < need)
    {
        /* 快路径：只有一次标志读取 + FIFO 读取 */
        if (SDIO_GetFlagStatus(SDIO_FLAG_RXDAVL) != RESET)
        {
            w[got++] = SDIO_ReadData();
            spin = 0u;
        }
        else
        {
            /* 慢路径：无数据时才查错误，避免拖慢快路径 */
            if (SDIO_GetFlagStatus(SDIO_FLAG_DCRCFAIL) != RESET)
            { printf("[SD] DCRCFAIL %lu/%lu\r\n", (unsigned long)got, (unsigned long)need);
              SDIO_ClearFlag(SDIO_FLAG_DCRCFAIL); return SD_ERR_DATA_CRC; }
            if (SDIO_GetFlagStatus(SDIO_FLAG_RXOVERR) != RESET)
            { printf("[SD] RXOVERR %lu/%lu\r\n", (unsigned long)got, (unsigned long)need);
              SDIO_ClearFlag(SDIO_FLAG_RXOVERR); return SD_ERR_RX_OVERRUN; }
            if (SDIO_GetFlagStatus(SDIO_FLAG_DTIMEOUT) != RESET)
            { printf("[SD] DTIMEOUT %lu/%lu\r\n", (unsigned long)got, (unsigned long)need);
              SDIO_ClearFlag(SDIO_FLAG_DTIMEOUT); return SD_ERR_DATA_TIMEOUT; }
            if (SDIO_GetFlagStatus(SDIO_FLAG_DATAEND) != RESET)
            {
                /* DATAEND 已置位 = 硬件按 DataLength 传完了，但 RXDAVL 可能在
                   最后一个字上提前清零。缺口很小时直接按剩余字数把 FIFO 取空。 */
                uint32_t miss = need - got;
                uint32_t k;
                if (miss <= 8u)
                {
                    for (k = 0u; (k < 5000u) && (got < need); k++)
                    {
                        if (SDIO_GetFlagStatus(SDIO_FLAG_RXDAVL) != RESET)
                        { w[got++] = SDIO_ReadData(); k = 0u; }
                    }
                }
                if (got == need)
                {
                    if (s_drain_log < 5u)
                    { s_drain_log++; printf("[SD] DATAEND-late %lu word(s) recovered\r\n", (unsigned long)miss); }
                    break;                       /* 数据取全，算成功 */
                }
                printf("[SD] DATAEND-early %lu/%lu\r\n", (unsigned long)got, (unsigned long)need);
                return SD_ERR_DATA_TIMEOUT;
            }
            if (++spin > 0x200000u)
            { printf("[SD] FIFO-timeout %lu/%lu STA=%08lX\r\n",
                     (unsigned long)got, (unsigned long)need, (unsigned long)SDIO->STA);
              return SD_ERR_DATA_TIMEOUT; }
        }
    }
    SDIO_ClearFlag(SDIO_FLAG_DATAEND | SDIO_FLAG_DCRCFAIL | SDIO_FLAG_DTIMEOUT |
                   SDIO_FLAG_RXOVERR | SDIO_FLAG_TXUNDERR | SDIO_FLAG_DBCKEND);
    return SD_OK;
}

/* 轮询 TX FIFO 发 len 字节 */
static sd_err_t sd_fifo_write(const uint8_t *buf, uint32_t len)
{
    const uint32_t *w = (const uint32_t *)buf;
    uint32_t need = len / 4u;
    uint32_t put  = 0u;
    uint32_t guard = 0u;

    while (put < need)
    {
        if (SDIO_GetFlagStatus(SDIO_FLAG_TXUNDERR) != RESET)
        { SDIO_ClearFlag(SDIO_FLAG_TXUNDERR); return SD_ERR_TX_UNDERRUN; }
        if (SDIO_GetFlagStatus(SDIO_FLAG_DTIMEOUT) != RESET)
        { SDIO_ClearFlag(SDIO_FLAG_DTIMEOUT); return SD_ERR_DATA_TIMEOUT; }

        if (SDIO_GetFlagStatus(SDIO_FLAG_TXFIFOF) == RESET)
        {
            SDIO_WriteData(w[put++]);
            guard = 0u;
        }
        else
        {
            if (++guard > SD_DATA_TIMEOUT) return SD_ERR_DATA_TIMEOUT;
        }
    }

    /* 等数据发完（含 CRC 状态） */
    {
        uint32_t to = SD_DATA_TIMEOUT;
        while (SDIO_GetFlagStatus(SDIO_FLAG_DATAEND) == RESET)
        {
            if (SDIO_GetFlagStatus(SDIO_FLAG_DCRCFAIL) != RESET)
            { SDIO_ClearFlag(SDIO_FLAG_DCRCFAIL); return SD_ERR_DATA_CRC; }
            if (SDIO_GetFlagStatus(SDIO_FLAG_TXUNDERR) != RESET)
            { SDIO_ClearFlag(SDIO_FLAG_TXUNDERR); return SD_ERR_TX_UNDERRUN; }
            if (SDIO_GetFlagStatus(SDIO_FLAG_DTIMEOUT) != RESET)
            { SDIO_ClearFlag(SDIO_FLAG_DTIMEOUT); return SD_ERR_DATA_TIMEOUT; }
            if (to-- == 0u) return SD_ERR_DATA_TIMEOUT;
        }
    }
    SDIO_ClearFlag(SDIO_FLAG_DATAEND | SDIO_FLAG_DCRCFAIL | SDIO_FLAG_DTIMEOUT |
                   SDIO_FLAG_RXOVERR | SDIO_FLAG_TXUNDERR | SDIO_FLAG_DBCKEND);
    return SD_OK;
}

/* ------------------------------------------------------------------ */
/* 上电初始化整条流程：CMD0/8/ACMD41/CMD2/3/9/7/ACMD6 -> 切 12 MHz 4 位 */
sd_err_t sd_init(void)
{
    uint32_t ocr;
    uint32_t i;

    s_init_ok = 0; s_type = SD_TYPE_UNKNOWN; s_rca = 0; s_blocks = 0;

    if (sd_is_present() == 0)
    {
        printf("[SD] no TF card (PD3=high)\r\n");
        return SD_ERR_NO_CARD;
    }

    sd_gpio_init();
    SDIO_SetPowerState(SDIO_PowerState_OFF);
    delay_ms(2);
    sd_clk_config(SDIO_CLK_DIV_INIT, SDIO_BusWide_1b);
    SDIO_SetPowerState(SDIO_PowerState_ON);
    SDIO_ClockCmd(ENABLE);
    delay_ms(2);

    { sd_err_t e = sd_cmd(0u, 0u, SDIO_Response_No);
      if (e != SD_OK) return SD_ERR_INIT; }

    { sd_err_t e = sd_cmd(8u, 0x000001AAu, SDIO_Response_Short);
      if (e == SD_OK)
      {
          uint32_t r = sd_resp1();
          if ((r & 0x000001FFu) != 0x000001AAu) return SD_ERR_RESPONSE;
      }
    }

    ocr = 0;
    for (i = 0u; i < SD_ACMD41_RETRIES; i++)
    {
        sd_err_t e = sd_app_cmd_41(OCR_HCS | OCR_27_36);
        if (e == SD_OK)
        {
            ocr = sd_resp1();
            if ((ocr & OCR_BUSY) != 0u) break;
        }
        delay_ms(10);
    }
    if (i >= SD_ACMD41_RETRIES) { printf("[SD] ACMD41 timeout\r\n"); return SD_ERR_INIT; }

    s_type = ((ocr & OCR_CCS) != 0u) ? SD_TYPE_SDHC : SD_TYPE_SDSC;

    { sd_err_t e = sd_cmd(2u, 0u, SDIO_Response_Long);
      if (e != SD_OK) { printf("[SD] CMD2 %d\r\n", (int)e); return SD_ERR_INIT; } }

    { sd_err_t e = sd_cmd(3u, 0u, SDIO_Response_Short);
      if (e != SD_OK) { printf("[SD] CMD3 %d\r\n", (int)e); return SD_ERR_INIT; }
      s_rca = sd_resp1() & 0xFFFF0000u;   /* 新 RCA 在 R6 响应的高 16 位 */ }

    { sd_err_t e = sd_cmd(9u, s_rca, SDIO_Response_Long);
      if (e != SD_OK) { printf("[SD] CMD9 %d\r\n", (int)e); return SD_ERR_INIT; }
      {
          uint32_t r1 = SDIO_GetResponse(SDIO_RESP1);
          uint32_t r2 = SDIO_GetResponse(SDIO_RESP2);
          uint32_t r3 = SDIO_GetResponse(SDIO_RESP3);
          uint8_t csd_struct = (uint8_t)((r1 >> 30) & 0x3u);
          /* csd_struct==1 即 CSD v2.0（SDHC/SDXC），只有它有 CSIZE 字段 */
          if (csd_struct == 1u)
          {
              uint32_t csize = ((r2 & 0x3Fu) << 16) | (r3 >> 16);
              s_blocks = (csize + 1u) * 1024u;
          }
          else s_blocks = 0;
      } }

    { sd_err_t e = sd_cmd(7u, s_rca, SDIO_Response_Short);
      if (e != SD_OK) { printf("[SD] CMD7 %d\r\n", (int)e); return SD_ERR_INIT; } }

    { sd_err_t e = sd_app_cmd(6u, 2u);
      if (e != SD_OK) { printf("[SD] ACMD6 %d\r\n", (int)e); return SD_ERR_INIT; } }

    sd_clk_config(SDIO_CLK_DIV_XFER, SDIO_BusWide_4b);
    delay_ms(2);
    s_init_ok = 1;
    printf("[SD] init OK: %s, %lu blocks = %lu MB\r\n",
           (s_type == SD_TYPE_SDHC) ? "SDHC/SDXC" : "SDSC",
           (unsigned long)s_blocks, (unsigned long)(s_blocks / 2048u));
    return SD_OK;
}

/* ------------------------------------------------------------------ */
/* 地址换算：SDHC/SDXC 直接用块号，SDSC 要乘 512 转成字节地址 */
static uint32_t sd_addr(uint32_t lba)
{
    return (s_type == SD_TYPE_SDHC) ? lba : (lba * 512u);
}

/* 单块循环读 nblk 个 512 B 块，同一块失败最多重试 3 次 */
sd_err_t sd_read_blocks(uint8_t *buf, uint32_t lba, uint32_t nblk)
{
    uint32_t i;

    if (!s_init_ok) return SD_ERR_NO_CARD;
    if (nblk == 0u) return SD_OK;
    if (s_blocks != 0u && (lba + nblk) > s_blocks) return SD_ERR_ADDR_RANGE;

    for (i = 0u; i < nblk; i++)
    {
        sd_err_t e = SD_ERR_DATA_TIMEOUT;
        uint8_t *p = buf + (i * 512u);
        uint32_t attempt;
        if ((((uint32_t)p) & 0x3u) != 0u) return SD_ERR_DMA;

        /* 单块读偶发 DATAEND-early（约每几十 KB 一次）-> 同一块重试最多 3 次。
           单块只有 512B，重试代价极小，而 1 次失败会让 FatFS 丢整个文件流。 */
        for (attempt = 0u; attempt < 3u; attempt++)
        {
            sd_data_start(512u, 1u);                   /* 先使能 DPSM */
            e = sd_cmd(17u, sd_addr(lba + i), SDIO_Response_Short);
            if (e == SD_OK) e = sd_fifo_read(p, 512u);
            sd_data_stop();
            if (e == SD_OK) break;
            s_rd_retry++;
        }
        if (e != SD_OK)
        {
            printf("[SD] read failed lba=%lu err=%d (3 retries)\r\n", (unsigned long)(lba + i), (int)e);
            return e;
        }
    }
    return SD_OK;
}

/* 单块循环写 nblk 个 512 B 块（写失败不重试，直接上报） */
sd_err_t sd_write_blocks(const uint8_t *buf, uint32_t lba, uint32_t nblk)
{
    uint32_t i;

    if (!s_init_ok) return SD_ERR_NO_CARD;
    if (nblk == 0u) return SD_OK;
    if (s_blocks != 0u && (lba + nblk) > s_blocks) return SD_ERR_ADDR_RANGE;

    for (i = 0u; i < nblk; i++)
    {
        sd_err_t e;
        const uint8_t *p = buf + (i * 512u);
        if ((((uint32_t)p) & 0x3u) != 0u) return SD_ERR_DMA;

        sd_data_start(512u, 0u);                       /* 先使能 DPSM */
        e = sd_cmd(24u, sd_addr(lba + i), SDIO_Response_Short);
        if (e == SD_OK) e = sd_fifo_write(p, 512u);
        sd_data_stop();
        if (e != SD_OK) return e;
        /* 写完后卡可能忙，留一点时间 */
        delay_ms(1);
    }
    return SD_OK;
}

/* ------------------------------------------------------------------ */
/* 诊断：累计单块读重试次数 */
uint32_t sd_rd_retries(void)
{
    return s_rd_retry;
}

/* ---------------- 状态查询 ---------------- */
uint32_t sd_block_count(void) { return s_blocks; }   /* 卡容量（512 B 块数） */
uint32_t sd_block_size(void)  { return 512u; }        /* 固定 512 B/块 */

/* 卡在位检测：PD3 为低 = 卡插好（每次回读前重配一次输入脚） */
uint8_t sd_is_present(void)
{
    GPIO_InitTypeDef gi;
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOD, ENABLE);
    gi.GPIO_Pin = GPIO_Pin_3; gi.GPIO_Mode = GPIO_Mode_IN;
    gi.GPIO_OType = GPIO_OType_PP; gi.GPIO_PuPd = GPIO_PuPd_UP;
    gi.GPIO_Speed = GPIO_Speed_2MHz;
    GPIO_Init(GPIOD, &gi);
    return (GPIO_ReadInputDataBit(GPIOD, GPIO_Pin_3) == Bit_RESET) ? 1u : 0u;
}

/* 是否已经成功初始化过 */
uint8_t sd_is_initialized(void) { return s_init_ok; }
