/*-----------------------------------------------------------------------/
/  Configurations of FatFs Module  -  双板 MP3 播放器项目专用配置
/  基于 FatFs R0.16 官方 ffconf.h（FFCONF_DEF 80386）精简重写，
/  保留 R0.16 需要的全部宏，取值按本项目需求设定。
/-----------------------------------------------------------------------*/

#define FFCONF_DEF	80386	/* Revision ID (R0.16) */

/*----------------------- Function Configurations -----------------------*/

#define FF_FS_READONLY	0	/* 0:读写  1:只读 */
#define FF_FS_MINIMIZE	0	/* 0:全功能（需要 f_opendir/f_readdir/f_lseek） */
#define FF_USE_FIND		0
#define FF_USE_MKFS		0
#define FF_USE_FASTSEEK	0
#define FF_USE_EXPAND	0
#define FF_USE_CHMOD	0
#define FF_USE_LABEL	0
#define FF_USE_FORWARD	0
#define FF_USE_STRFUNC	0
#define FF_PRINT_LLI	0
#define FF_PRINT_FLOAT	0
#define FF_STRF_ENCODE	0

/*------------------ Locale and Namespace Configurations ----------------*/

/* FF_CODE_PAGE: 936 = 简体中文 GBK（DBCS），437 = 美国 OEM（SBCS）。
   本项目实测（armcc -O1 + armlink 全量重链接，含 SPI 下载链路）:
     936 -> Total ROM 389984 B；ffunicode.o 常量表 174774 B（uni2oem936 + oem2uni936
            各约 87 KB）；代码区距 Sector 7(0x08060000，断点续播记录区) 只剩 2676 B。
     437 -> Total ROM 215308 B（**省 174676 B ≈ 170.6 KB**）；代码区余量约 173 KB。
     （开源版又加回 app/gbk2312_map.h 的 16 KB GB2312 码表、删掉调试代码后：Total ROM 224556 B，
      LR_IROM1 结束 0x08036F28，距 Sector 7 余约 164 KB。）
   为什么可以换：本项目 FF_LFN_UNICODE=2，文件名走 LFN（UTF-16 <-> UTF-8），与码页无关；
   码页只影响 8.3 短名(SFN)。ff.c:2972-3007 create_name() 里，扩展字符经 ff_uni2oem
   转不出来时会被替换成 '_' 并置 NS_LOSS|NS_LFN，**LFN 照常创建**，所以中文名文件仍能
   正常创建/读取/在歌单里显示（只有 SFN 变得有损，Windows 与本机 UI 都读 LFN）。
   代价：只有 8.3 短名且非 ASCII 的文件，其 SFN 会退化成 '_' 占位（Windows 与本机 UI 都读 LFN，不受影响）。 */
#define FF_CODE_PAGE	437

/* FF_USE_LFN: 0=禁用 1=BSS静态工作缓冲(非线程安全) 2=栈 3=堆
   本项目用 1：免 malloc、无需 ff_memalloc/ff_memfree。
   注意 R0.16 语义与旧版 R0.12c 不同（旧版 3=静态）。
   FF_USE_LFN>=1 时 ffunicode.c 必须加入工程。 */
#define FF_USE_LFN		1
#define FF_MAX_LFN		255

/* 1=UTF-16 2=UTF-8 3=UTF-32；本项目用 2：fno.fname 直接是 UTF-8，
   ui_text() 按 unicode 查表渲染，无需任何码页转换 */
#define FF_LFN_UNICODE	2

#define FF_LFN_BUF		255
#define FF_SFN_BUF		12

#define FF_FS_RPATH		0	/* 0:不使用相对路径 */
#define FF_PATH_DEPTH	10

/*-------------------- Drive/Volume Configurations ----------------------*/

#define FF_VOLUMES		1
#define FF_STR_VOLUME_ID	0	/* 0:卷路径写作 "0:" */
#define FF_VOLUME_STRS		"RAM","NAND","CF","SD"
#define FF_MULTI_PARTITION	0
#define FF_MIN_SS		512
#define FF_MAX_SS		512
#define FF_LBA64		0	/* 0:32 位 LBA（SDHC 足够）→ LBA_t = DWORD */
#define FF_MIN_GPT		0x10000000
#define FF_USE_TRIM		0

/*---------------------- System Configurations --------------------------*/

#define FF_FS_TINY		1	/* 1:小缓冲，扇区缓冲放在 FATFS 对象里，省 RAM */
#define FF_FS_EXFAT		0	/* 0:不支持 exFAT → FSIZE_t = DWORD */

/* 1 = 不使用时间戳（本板无 RTC），对象用固定时间 */
#define FF_FS_NORTC		1
#define FF_NORTC_MON	1
#define FF_NORTC_MDAY	1
#define FF_NORTC_YEAR	2026	/* 固定时间戳的年份（无 RTC，仅占位） */

#define FF_FS_CRTIME	0
#define FF_FS_NOFSINFO	0
#define FF_FS_LOCK		0	/* 0:不用文件锁（单线程） */
#define FF_FS_REENTRANT	0	/* 0:不重入 → ffsystem.c 无需加入工程 */
#define FF_FS_TIMEOUT	1000

/*--- End of configuration options ---*/
