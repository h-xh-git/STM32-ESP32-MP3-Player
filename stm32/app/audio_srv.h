#ifndef __AUDIO_SRV_H
#define __AUDIO_SRV_H

/*
 * audio_srv.h - STM32 侧音频服务（SD 取流 -> 组帧 -> 发给 ESP32）
 *
 * 工作机制：信用窗口 + ESP32 主动拉取（pull）
 *   ESP32 只在它的 PSRAM 环形缓冲有空闲时发 CMD_AUDIO_REQ{blocks}，
 *   本模块把 blocks 累加为信用额度；主循环每 1 ms 调 audio_srv_poll()：
 *     有额度 && UART2 TX 环装得下 -> f_read 1 KB
 *        -> 发 RESP_AUDIO{seq u32, len u16, data}
 *   额度用完即停（等 ESP32 下次请求），全程非阻塞、不丢帧、无需重传。
 *   文件读完发 RESP_END，由 main 决定下一首（列表循环/单曲循环/随机）。
 *
 * 位置：pos_ms = 已送出的文件字节按总时长线性折算（32/64 位安全）。
 * seek：按字节比例定位后再向前找 0xFFEx 帧同步头，并让 seq 跳变，
 *       使 ESP32 能识别不连续并重置解码器。
 */
#include <stdint.h>

#define AUDIO_CHUNK       1024u   /* 每帧音频数据字节数               */
#define AUDIO_CREDIT_MAX  64u     /* 未兑现额度上限 (= 64 KB 音频)    */
#define AUDIO_SYNC_WIN    512u    /* seek 后搜索帧同步头的窗口        */
#define AUDIO_TIME_MS     1000u   /* 播放中每隔多久推一次 RESP_TIME  */

void     audio_srv_init(void);

/* 打开第 idx0 首(0 起)并从头开始；返回 1 成功 */
uint8_t  audio_srv_open(uint16_t idx0, uint32_t dur_ms);
void     audio_srv_close(void);

/* 播放/暂停：暂停时不再申请数据（ESP32 缓冲区播完自然安静） */
void     audio_srv_set_playing(uint8_t on);

/* 跳到 ms 位置（自动对齐 MP3 帧头，清空额度并让 seq 跳变） */
void     audio_srv_seek_ms(uint32_t ms);

/* 主循环每 1 ms 调用一次：推时间/按额度取流发送 */
void     audio_srv_poll(void);

/* 收到一帧（main 从 proto_recv 转发过来） */
void     audio_srv_on_frame(uint8_t type, const uint8_t *pl, uint16_t len);

/* ESP32 发来的播放控制命令回调：cmd = CMD_xxx, arg = 参数 */
void     audio_srv_set_cmd_cb(void (*cb)(uint8_t cmd, uint32_t arg));

/* 状态变化才真正发 RESP_STATE（内部缓存比较，可每轮无脑调用） */
void     audio_srv_notify_state(uint16_t track0, uint16_t total,
                                uint8_t playing, uint8_t mode, uint8_t vol);

/* 取走"一首播完"事件（每个事件只返回一次），idx0 = 完成的曲目序号 */
uint8_t  audio_srv_take_finished(uint16_t *idx0);

/* 当前播放位置 / 总时长 (ms) */
uint32_t audio_srv_pos_ms(void);

#endif /* __AUDIO_SRV_H */
