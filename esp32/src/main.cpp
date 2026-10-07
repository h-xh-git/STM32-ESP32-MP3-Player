/*
 * main.cpp - ESP32-S3 侧主程序（双板 MP3 播放器）
 *
 * 职责：从 UART1 收 STM32 送来的 MP3 压缩流，Helix 解码，I2S 输出到 MAX98357A。
 * 界面/按键/歌单/SD 卡全部由 STM32 负责，本板不做 UI。
 *
 * 接线（见 hardware/wiring-diagram.html）：
 *   STM32 PA2(TX) -> GPIO17(RX) |  STM32 PA3(RX) <- GPIO18(TX)
 *   GPIO5=BCLK  GPIO6=LRCK  GPIO7=DIN  GPIO8 空闲（MAX98357A 的 SD/MODE 直接接 3V3）
 */
#include <Arduino.h>
#include "player.h"
#include "netdl.h"     /* 网页上传歌曲 -> SPI 推送 -> STM32 写 TF 卡 */
#include "wifimgr.h"   /* 多 WiFi：NVS 持久化 + 网页配置表单 */

void setup()
{
    Serial.begin(115200);
    delay(200);
    Serial.println();
    Serial.println("ESP32-S3 dual-board MP3 player  (link 1Mbps GPIO17/18)");
    Serial.println("I2S: BCLK=5 LRCK=6 DIN=7 AMP_SD=8  -> MAX98357A");
    Serial.println("SPI: SCK=12 MOSI=11 MISO=13 CS=10 READY=9  -> STM32 (web upload)");

    player_init();

    /* 多 WiFi：从 NVS 读回账号，STA 依次尝试；全不通就开配网热点 + 网页配置。
       网页地址串口会打印（http://<sta-ip>/ 或 http://192.168.4.1/），改动立刻写 NVS。 */
    wifimgr_init();

    /* 上传链路：SPI2 主机 + 网页 /upload 上传页（见 netdl.h 模块头） */
    netdl_init();
}

void loop()
{
    wifimgr_poll();                     /* 网页请求 + 自动重连状态机（非阻塞） */
    vTaskDelay(pdMS_TO_TICKS(20));
}
