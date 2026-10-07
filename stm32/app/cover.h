/*
 * cover.h - 封面解码对外接口
 *
 * 作用：声明封面解码器的入口与查询接口（复位 / 同步或异步解码 / 取 144x144
 *       RGB565 像素）。
 * 支持与不支持：baseline(SOF0/SOF1)、灰度与 YCbCr、4:4:4/4:2:2/4:2:0/4:1:1、
 *       DHT/DQT/DRI；渐进、算术、无损不支持（见下方说明）。
 * 硬件要点：纯软件解码，不占任何引脚；输出缓冲 144x144x2 = 41472 B，静态分配。
 * 依赖：stdint.h；实现 cover.c 依赖 FatFS（读源）与 bsp_tick（分片预算计时）。
 * 调用：app/main.c（begin/poll/busy）与 bsp/ui/ui_player.c（ready/pixels）。
 */
#ifndef __COVER_H
#define __COVER_H

#include <stdint.h>

/* ==================================================================
 * 真实封面：迷你 baseline JPEG 解码器 -> 144x144 RGB565
 *
 * 支持：baseline(SOF0/SOF1) 8bit、灰度(1 分量) / YCbCr(3 分量)、
 *       4:4:4 / 4:2:2 / 4:2:0 / 4:1:1（Hmax,Vmax <= 4 且 Vmax <= 2）、
 *       DHT / DQT / DRI + RSTn 重启。
 * 不支持：渐进(SOF2)、算术、无损、16bit 量化、多扫描 —— 返回 0，界面保留占位图。
 * 来源：MP3 内嵌 ID3v2 APIC（v2.2 PIC / v2.3 APIC / v2.4 APIC）；
 *       失败后回退同目录 cover.jpg / folder.jpg / <同名>.jpg（含 .jpeg 与大小写）。
 * ================================================================== */

#define COVER_PX   144u          /* 输出边长，与 ui_player.c 的 COVER_S 一致 */
#define COVER_MAX_DIM 1600u      /* 源图边长上限，超过则拒绝（保留占位图） */

/* 复位解码器状态（上电调用一次） */
void            cover_init(void);

/* 同步：解完才返回。1 = 解码成功，0 = 无封面（界面留占位图）。
   切歌走下面的异步接口，封面解码不会挡住出声： */
uint8_t         cover_load(const char *mp3_path);

/* 异步：cover_begin() 只定位源 + 解析头部；main 在主循环里用
   cover_poll(budget_ms) 分片推进，解完再刷新封面区。
     cover_begin : 1 = 有封面正在解；0 = 没有可用封面
     cover_poll  : 0 = 还要继续；1 = 解完且成功；2 = 解完但失败 / 没有在解
     cover_busy  : 1 = 当前有解码未完成
   budget_ms = 0 表示不限时间（一次解完）。 */
uint8_t         cover_begin(const char *mp3_path);
uint8_t         cover_poll(uint32_t budget_ms);
uint8_t         cover_busy(void);
uint8_t         cover_ready(void);
/* 取解码结果（144x144 RGB565 行主序，静态缓冲，只在 cover_ready() 为 1 时有效） */
const uint16_t *cover_pixels(void);                 /* COVER_PX*COVER_PX RGB565 行主序 */


#endif /* __COVER_H */
