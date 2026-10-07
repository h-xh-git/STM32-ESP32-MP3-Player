#ifndef __BSP_ENCODER_H
#define __BSP_ENCODER_H
#include <stdint.h>

/*
 * bsp_encoder.h - 旋转编码器 (EC11 类) 音量旋钮
 *
 *   PC6 = TIM8_CH1 (AF3)   编码器 A 相
 *   PC7 = TIM8_CH2 (AF3)   编码器 B 相
 *   PD12 = 旋钮按下        (GPIO 输入, 内部上拉, 按下接地)
 *
 * 用 TIM8 的硬件编码器模式 (TIM_EncoderMode_TI12) 计数: 定时器硬件自己跟
 * 踪 A/B 两相的边沿, 做 4 倍频, 不用中断也不用主循环轮询 GPIO, 所以手转得
 * 再快也不会丢步或倒计。TIM8 在本工程里没有别的用途，
 * PC6/PC7 也只接编码器，不与 LCD/SDIO/按键冲突。
 *
 * 按下脚为什么不用 PC5/PC8:
 *   - PC5 不配置，留给硬件另作安排；
 *   - PC8 = SDIO_D0 (bsp_sdio.c 把 PC8..PC12 配成 AF_SDIO，传输时切 4-bit)，
 *     接旋钮按下会与 SD 卡抢数据线，所以用 PD12。
 *   GPIOD 在本工程只用了 PD2(SDIO_CMD)/PD3(CD)，PD12 空闲。
 *
 * 机械编码器每转过一格(一段"咔哒"手感)会产生 4 个计数, 所以:
 *
 *   enc_step() = 自上次调用以来转过的档位数, 顺/逆时针对应正/负。
 *                返回值限幅在 ENC_COUNTS_PER_DETENT 的 +-4 档, 主循环偶尔
 *                卡顿一下也不会让音量一次跳几十格。余数会留在内部累加器里,
 *                下次继续凑够一档, 不丢动作。
 *
 * 若装好后发现"顺时针变小、逆时针变大"与手感相反, 把 ENC_DIR_INVERT 改 1。
 */

#define ENC_COUNTS_PER_DETENT   4

void    enc_init(void);
int8_t  enc_step(void);          /* 转过的档位数, 正=顺时针, 0=没动 */
uint8_t enc_sw_scan(void);       /* 按下沿消抖, 每次按下只返回一次 1 */
uint8_t enc_sw_raw(void);        /* 1 = 旋钮此刻被按住 (不消抖) */

#endif /* __BSP_ENCODER_H */
