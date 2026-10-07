/*
 * link.cpp - 板间串口链路实现（UART1, 1 Mbps, GPIO17=RX / GPIO18=TX）
 *   发帧：link_send* 组好帧直接写 UART；收帧：link_wait_frame 喂解析器并等超时。
 *   帧间多出来的字节存在 s_left 里下次继续解析，保证一个字节都不丢。
 *   依赖：proto.h（组帧 + 流式解析）、driver/uart.h。
 *   调用者：player.cpp（task_link 收发协议帧）。
 */
#include "link.h"
#include <Arduino.h>
#include "driver/uart.h"
#include <string.h>

static ProtoParser s_parser;

void link_init(void)
{
    uart_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.baud_rate  = LINK_BAUD;
    cfg.data_bits  = UART_DATA_8_BITS;
    cfg.parity     = UART_PARITY_DISABLE;
    cfg.stop_bits  = UART_STOP_BITS_1;
    cfg.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_APB;      /* APB 80MHz / 1Mbps -> 整数分频, 误差 0 */

    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_1, LINK_RX_BUF, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_1, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_1, LINK_TX_PIN, LINK_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    s_parser.reset();
}

void link_send(uint8_t type, const uint8_t *payload, uint16_t len)
{
    static uint8_t frame[PROTO_MAX_PAYLOAD + PROTO_OVERHEAD];
    uint16_t n = proto_build(frame, (uint16_t)sizeof(frame), type, payload, len);
    if (n == 0u) return;
    uart_write_bytes(UART_NUM_1, (const char *)frame, n);
}

void link_send_u16(uint8_t type, uint16_t v)
{
    uint8_t p[2];
    proto_put_u16(p, v);
    link_send(type, p, 2u);
}

void link_send_u32(uint8_t type, uint32_t v)
{
    uint8_t p[4];
    proto_put_u32(p, v);
    link_send(type, p, 4u);
}

/* 帧之间的续存缓冲：uart_read_bytes 一次可能取回好几帧（STM32 连续发命令、
   或音频帧扎堆时）。解析出第一帧就返回时必须把 chunk 里剩下的字节留下，
   否则这些字节已经从驱动环里取走、会被直接丢掉。 */

static uint8_t  s_left[600];
static uint16_t s_left_n = 0;

bool link_wait_frame(uint8_t *type, const uint8_t **pl, uint16_t *len,
                     uint32_t timeout_ms)
{
    uint8_t  chunk[512];
    uint32_t t0 = millis();

    for (;;) {
        /* 1) 先把上次没喂完的字节喂掉 */
        if (s_left_n != 0u) {
            uint16_t i = 0u;
            while (i < s_left_n) {
                if (s_parser.feed(s_left[i++])) {
                    uint16_t rest = (uint16_t)(s_left_n - i);
                    if (rest != 0u) memmove(s_left, s_left + i, (size_t)rest);
                    s_left_n = rest;
                    *type = s_parser.type();
                    *pl   = s_parser.data();
                    *len  = s_parser.len();
                    return true;
                }
            }
            s_left_n = 0u;
        }

        /* 2) 再取新数据，放进 left 缓冲统一处理 */
        {
            int n = uart_read_bytes(UART_NUM_1, chunk, sizeof(chunk), 0);
            if (n > 0) {
                if (n > (int)sizeof(s_left)) n = (int)sizeof(s_left);
                memcpy(s_left, chunk, (size_t)n);
                s_left_n = (uint16_t)n;
                continue;
            }
        }

        if ((uint32_t)(millis() - t0) >= timeout_ms) return false;
        vTaskDelay(1);
    }
}
