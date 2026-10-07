/*=======================================================================
 * diskio.c - FatFs R0.16 底层磁盘 I/O，对接 bsp_sdio（STM32F407 SDIO 4-bit）
 * -----------------------------------------------------------------------
 * 本文件替代 FatFs 官方模板，放在 ff16/source/ 内与 ff.c 同目录。
 *
 * R0.16 要点：
 *   - disk_read/disk_write 的扇区参数类型是 LBA_t（FF_LBA64=0 时 = DWORD）；
 *   - 原型已在 diskio.h 中声明，本文件只提供定义，不要重复声明；
 *   - get_fattime() 仅 FF_FS_NORTC==0 时需要。
 *
 * 配套 ffconf.h 取值：FF_CODE_PAGE=437、FF_USE_LFN=1、FF_FS_TINY=1、
 * FF_FS_NORTC=1、FF_VOLUMES=1、FF_STR_VOLUME_ID=0、FF_MIN_SS=FF_MAX_SS=512、
 * FF_FS_EXFAT=0、FF_LBA64=0、FF_USE_TRIM=0、FF_FS_LOCK=0、FF_FS_REENTRANT=0。
 * ffunicode.c 必须加入工程（FF_USE_LFN>=1）。
 *
 * 依赖：bsp_sdio（SDIO 4 位轮询驱动）+ ff.h/diskio.h。
 * 调用者：ff.c（FatFs 内核）按 diskio.h 的原型调用 disk_status/disk_initialize/
 *         disk_read/disk_write/disk_ioctl。
 *=======================================================================*/
#include "ff.h"
#include "diskio.h"
#include "bsp_sdio.h"
#include <string.h>

#define DEV_SD   0        /* 卷 0 = SD 卡 */

/* 4 字节对齐的暂存扇区：SDIO 以 4 字节字访问 FIFO，轮询读写也要求缓冲
 * 4 字节对齐，而 FatFs 的扇区缓冲只保证 BYTE 对齐，故未对齐时经此中转。 */
static uint32_t s_scratch[512 / 4];

/*-----------------------------------------------------------------------*/
/* 取驱动器状态：返回 STA_NOINIT / STA_NODISK / 0(就绪)                  */
/*-----------------------------------------------------------------------*/
DSTATUS disk_status (BYTE pdrv)
{
    if (pdrv != DEV_SD)          return STA_NOINIT;
    if (sd_is_present() == 0u)   return STA_NODISK;
    if (sd_is_initialized() == 0u) return STA_NOINIT;
    return 0;                    /* 就绪 */
}

/*-----------------------------------------------------------------------*/
/* 初始化驱动器：卡不在位或 sd_init 失败都返回 STA_NOINIT                */
/*-----------------------------------------------------------------------*/
DSTATUS disk_initialize (BYTE pdrv)
{
    if (pdrv != DEV_SD)          return STA_NOINIT;
    if (sd_is_present() == 0u)   return STA_NODISK;
    if (sd_is_initialized() == 0u)
    {
        if (sd_init() != SD_OK)  return STA_NOINIT;
    }
    return 0;                    /* 就绪 */
}

/*-----------------------------------------------------------------------*/
/* 读扇区（count 个 512 B 扇区）                                         */
/*-----------------------------------------------------------------------*/
DRESULT disk_read (BYTE pdrv, BYTE* buff, LBA_t sector, UINT count)
{
    if (pdrv != DEV_SD)            return RES_PARERR;
    if (sd_is_initialized() == 0u) return RES_NOTRDY;
    if (count == 0u)               return RES_OK;

    /* 已 4 字节对齐：直接多块读写 */
    if (((uint32_t)buff & 0x3u) == 0u)
        return (sd_read_blocks(buff, (uint32_t)sector, (uint32_t)count) == SD_OK)
               ? RES_OK : RES_ERROR;

    /* 未对齐：逐扇区经对齐暂存区中转 */
    {
        UINT i;
        for (i = 0u; i < count; i++)
        {
            if (sd_read_blocks((BYTE*)s_scratch, (uint32_t)(sector + i), 1u) != SD_OK)
                return RES_ERROR;
            memcpy(buff + (i * 512u), s_scratch, 512u);
        }
    }
    return RES_OK;
}

/*-----------------------------------------------------------------------*/
/* 写扇区（count 个 512 B 扇区）                                         */
/*-----------------------------------------------------------------------*/
DRESULT disk_write (BYTE pdrv, const BYTE* buff, LBA_t sector, UINT count)
{
    if (pdrv != DEV_SD)            return RES_PARERR;
    if (sd_is_initialized() == 0u) return RES_NOTRDY;
    if (count == 0u)               return RES_OK;

    if (((uint32_t)buff & 0x3u) == 0u)
        return (sd_write_blocks(buff, (uint32_t)sector, (uint32_t)count) == SD_OK)
               ? RES_OK : RES_ERROR;

    {
        UINT i;
        for (i = 0u; i < count; i++)
        {
            memcpy(s_scratch, buff + (i * 512u), 512u);
            if (sd_write_blocks((const BYTE*)s_scratch, (uint32_t)(sector + i), 1u) != SD_OK)
                return RES_ERROR;
        }
    }
    return RES_OK;
}

/*-----------------------------------------------------------------------*/
/* 设备控制：CTRL_SYNC / GET_SECTOR_COUNT / GET_SECTOR_SIZE / GET_BLOCK_SIZE */
/*-----------------------------------------------------------------------*/
DRESULT disk_ioctl (BYTE pdrv, BYTE cmd, void* buff)
{
    if (pdrv != DEV_SD)            return RES_PARERR;
    if (sd_is_initialized() == 0u) return RES_NOTRDY;

    switch (cmd)
    {
    case CTRL_SYNC:                       /* 写完即落盘，无需额外同步 */
        return RES_OK;

    case GET_SECTOR_COUNT:
        *(DWORD*)buff = (DWORD)sd_block_count();
        return RES_OK;

    case GET_SECTOR_SIZE:
        *(WORD*)buff = (WORD)sd_block_size();
        return RES_OK;

    case GET_BLOCK_SIZE:                  /* 擦除块粒度：1 个扇区 */
        *(DWORD*)buff = 1u;
        return RES_OK;

    default:
        return RES_PARERR;
    }
}

#if FF_FS_NORTC == 0
/* 仅 FF_FS_NORTC==0 时 FatFs 会调用；本项目 NORTC=1，故默认不编译。 */
DWORD get_fattime (void)
{
    return ((DWORD)(2026u - 1980u) << 25) | ((DWORD)1u << 21) | ((DWORD)1u << 16);
}
#endif
