#ifndef __NETDL_H
#define __NETDL_H

/*
 * netdl.h - 「网页上传存 TF 卡」的 STM32 侧服务
 *
 * 链路：ESP32-S3（WiFi + WebServer）--SPI2 主机--> STM32F407（SPI2 从机）
 *       --> app/netdl.c 解析帧 --> FatFS f_write --> TF 卡
 *
 * ESP32 侧的数据来源只有一条：手机/电脑浏览器打开 http://<ip>/upload 选本地
 * mp3/wav，POST 给 ESP32，由 ESP32 经 SPI 帧链路（T_BEGIN/T_DATA/T_END/T_ABORT）
 * 转发过来，STM32 侧不关心里面是本地文件还是网络来源。
 * 老浏览器走原生表单提交时主机不知道总大小，T_BEGIN 的 total 可能为 0，
 * 收尾以实际落盘字节数为准（见 dl_end()）。
 *
 * 分工：
 *   bsp/spid/bsp_spid.{h,c}  只管「搬一帧 2064 字节」：SPI2 从机 + DMA +
 *                            READY 流控 + CS 对齐，不含任何协议语义。
 *   本模块（netdl）          解析 bsp_spid 交上来的帧、维护 FATFS 文件、
 *                            组织应答帧，并对外暴露进度。
 *
 * 帧格式与状态/错误码全部定义在 bsp_spid.h（SPID_T_xxx / SPID_ST_xxx /
 * SPID_E_xxx / SPID_M_xxx / SPID_S_xxx）。
 *
 * 写卡策略：
 *   先写 "<base>/<名字>.part"，收完 T_END 后 f_sync + f_close + f_rename
 *   成最终名字；中途 T_ABORT 或 20 s 没收到帧 -> f_close + f_unlink 删掉
 *   半成品。这样歌单扫描永远不会看到一个写了一半的文件。
 *   <base> 取自 playlist 的扫描基准目录（pl_base()：/MUSIC 或根目录）。
 *
 * 调用（main.c）：
 *   netdl_init();
 *   for (;;) { netdl_poll(); ... }
 *   下载完成后 netdl_take_done() 返回 1 -> 主循环重新 pl_scan() 刷新歌单。
 */

#include <stdint.h>

/* 状态（netdl_state 返回值） */
#define NETDL_IDLE   0u   /* 空闲，没在下载 */
#define NETDL_OPEN   1u   /* 已收到 T_BEGIN，正在收数据块 */
#define NETDL_DONE   2u   /* 刚刚成功收完一首（一次性事件，用 netdl_take_done 取走） */
#define NETDL_ERROR  3u   /* 上一次下载失败（一次性事件，用 netdl_take_done 取走） */

/* 初始化（内部会 Reset 应答缓冲） */
void     netdl_init(void);

/* 卡/文件系统是否就绪（main.c 在 f_mount 成功后置 1；未就绪时一律回 NOCARD） */
void     netdl_set_fs_ready(uint8_t ready);

/* 主循环调用：有帧就处理一帧并回帧 */
void     netdl_poll(void);

/* 状态查询 */
uint8_t  netdl_state(void);
uint8_t  netdl_busy(void);          /* 1 = 正在下载 */
uint32_t netdl_written(void);       /* 已写入字节数 */
uint16_t netdl_percent(void);       /* 0..100（total 未知时返回 0） */
const char *netdl_name(void);       /* 当前/最近的文件名（已去掉 .part） */

/* 取走「下载结束」事件：返回 1 = 成功收完，2 = 失败/取消，0 = 无事件。
   返回 1 时把最终文件名与字节数写进 name 与 size 两个出参（都可传 0）。 */
uint8_t  netdl_take_done(const char **name, uint32_t *size);

/* 最近一条状态文字（ASCII，取走「下载结束」事件后用来解释失败原因） */
const char *netdl_msg(void);

#endif /* __NETDL_H */
