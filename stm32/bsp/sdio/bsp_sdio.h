#ifndef __BSP_SDIO_H
#define __BSP_SDIO_H

/*
 * bsp_sdio.h - STM32F407 SDIO 4 位 SD/TF 卡底层驱动（全阻塞、轮询 FIFO）
 *
 * 作用：卡初始化、单块读写、卡在位查询，供 FatFS 的 diskio.c 调用。
 *
 * 接线（天空星核心板板载 TF 卡座，SDIO 硬连，不可改）：
 *   SDIO_CK  = PC12   SDIO_CMD = PD2
 *   SDIO_D0  = PC8    SDIO_D1  = PC9    SDIO_D2 = PC10   SDIO_D3 = PC11
 *   TF_DET   = PD3    （插卡 = 低电平）
 *   以上引脚全部复用 AF12。
 *
 * SDIOCLK = PLL48CK = 48 MHz，SDIO_CK = 48/(2+CLKDIV)：
 *   识别时钟: CLKDIV = 0x76 -> 400 kHz，1 位
 *   工作时钟: CLKDIV = 0x02 -> 12 MHz，4 位（轮询留余量，不用 DMA）
 *
 * 本驱动全部阻塞：sd_read_blocks/sd_write_blocks 调用期间一直轮询 FIFO。
 * 调用者：FatFS 的 diskio.c；上层 audio_srv 负责把它放进自己的任务节奏里。
 */
#include "stm32f4xx.h"

typedef enum
{
    SD_OK                  = 0,
    SD_ERR_NO_CARD         = -1,
    SD_ERR_INIT            = -2,
    SD_ERR_CMD_TIMEOUT     = -3,
    SD_ERR_CMD_CRC         = -4,
    SD_ERR_ILLEGAL_CMD     = -5,
    SD_ERR_DATA_TIMEOUT    = -6,
    SD_ERR_DATA_CRC        = -7,
    SD_ERR_RX_OVERRUN      = -8,
    SD_ERR_TX_UNDERRUN     = -9,
    SD_ERR_ADDR_RANGE      = -10,
    SD_ERR_RESPONSE        = -11,
    SD_ERR_UNSUPPORTED     = -12,
    SD_ERR_DMA             = -13,   /* 缓冲未按 4 字节对齐 */
} sd_err_t;

/* SD 卡类型 */
#define SD_TYPE_UNKNOWN   0u
#define SD_TYPE_SDSC      1u   /* 标准容量, 字节寻址 (≤2GB) */
#define SD_TYPE_SDHC      2u   /* 高容量, 块寻址 (≥2GB, SDHC/SDXC) */

/* 卡上电初始化；成功返回 SD_OK，失败返回 sd_err_t 负值 */
sd_err_t  sd_init(void);
/* 读 nblk 个 512 B 块到 buf（buf 必须 4 字节对齐） */
sd_err_t  sd_read_blocks(uint8_t *buf, uint32_t lba, uint32_t nblk);
/* 写 nblk 个 512 B 块（buf 必须 4 字节对齐） */
sd_err_t  sd_write_blocks(const uint8_t *buf, uint32_t lba, uint32_t nblk);

/* 单块读发生重试的累计次数（0 = 读路径完全健康） */
uint32_t  sd_rd_retries(void);

uint32_t  sd_block_count(void);     /* 卡容量, 单位 512B 块 */
uint32_t  sd_block_size(void);      /* 恒为 512 */
uint8_t   sd_is_present(void);      /* PD3: 1=卡在位 */
uint8_t   sd_is_initialized(void);   /* 是否已成功初始化过 */

#endif /* __BSP_SDIO_H */
