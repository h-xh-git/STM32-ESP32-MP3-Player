#ifndef ESP_PLAYER_H
#define ESP_PLAYER_H

/*
 * player.h - ESP32-S3 侧播放器
 *   UART1 收 STM32 送来的 MP3 压缩流 -> Helix 解码 -> I2S -> MAX98357A
 *   三任务：link(收帧/要额度) / decode(解码) / i2s(输出)
 *   依赖：lib/link、lib/proto、lib/mp3dec，以及 Arduino/FreeRTOS/I2S 驱动。
 *   调用者：src/main.cpp（setup 里调 player_init）。
 */
#include <stdint.h>

void player_init(void);

#endif /* ESP_PLAYER_H */
