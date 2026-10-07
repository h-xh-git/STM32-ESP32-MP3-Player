/*
 * netdl.cpp - ESP32-S3 侧「歌曲拉进 TF 卡」
 *
 * 手机/电脑浏览器打开 http://<ip>/upload 选本地 mp3/wav
 *   -> HTTP POST 流式读 -> SPI2 主机固定 2064 B 分帧推送
 *   -> STM32F407 SPI2 从机（app/netdl.c）-> FatFS f_write -> TF 卡
 *
 * 与 STM32 侧的约定（改这里必须同步改 STM32 侧 bsp/spid/bsp_spid.h）：
 *   MOSI: [0]=0xA5 [1]=0x5A [2]=type [3]=flags [4..5]=seq(u16 LE)
 *         [6..7]=len(u16 LE) [8..11]=total(u32 LE) [12..15]=crc32(payload LE)
 *         [16..2063]=payload
 *   MISO: [0]=0x5A [1]=0xA5 [2]=status [3]=errcode [4..5]=ack_type
 *         [6..7]=ack_seq [8..11]=written [12..15]=total [16..79]=msg[64]
 *
 * 流控：每次事务前必须等 DL_READY(GPIO9) 变高。应答滞后一帧：
 *       第 N 帧读回的是第 N-1 帧的结果（第一帧是上电初值，不可信），
 *       所以 T_BEGIN 的结果要用紧随的一帧 T_PING 读回，最后一帧
 *       之后也要再发一帧 T_PING 收尾确认。
 *
 * 依赖：wifimgr（WiFi 连接 + 配置网页，/upload 页借它的 WebServer）、SPI2 主机、GPIO。
 * 调用者：src/main.cpp 的 setup()（netdl_init）；传输由网页 POST /upload 的回调驱动
 *         （netdl_upload_begin / write / end / abort）。
 */

#include "netdl.h"
#include "wifimgr.h"   /* 多 WiFi：NVS 持久化 + 网页配置表单 + 借出配置网页的 WebServer */

#include <Arduino.h>
#include <WebServer.h> /* 网页上传：/upload 路由挂在 wifimgr 的 WebServer 上 */
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* 有些 IDF 版本不导出 DMA_ATTR，这里兜底（只要求 4 字节对齐、放内部 RAM） */
#ifndef DMA_ATTR
#define DMA_ATTR __attribute__((aligned(4)))
#endif

/* ------------------------------------------------------------------ */
/* CRC-32（反射 poly 0xEDB88320，init/xor 0xFFFFFFFF）半字节表，
   必须与 STM32 侧 app/netdl.c 的 netdl_crc32() 完全一致。 */
static const uint32_t s_crc_tab[16] = {
    0x00000000u, 0x77073096u, 0xEE0E612Cu, 0x990951BAu,
    0x076DC419u, 0x706AF48Fu, 0xE963A535u, 0x9E6495A3u,
    0x0EDB8832u, 0x79DCB8A4u, 0xE0D5E91Eu, 0x97D2D988u,
    0x09B64C2Bu, 0x7EB17CBDu, 0xE7B82D07u, 0x90BF1D91u
};

static uint32_t netdl_crc32(const uint8_t *p, uint32_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++) {
        crc ^= (uint32_t)p[i];
        crc = (crc >> 4) ^ s_crc_tab[crc & 0x0Fu];
        crc = (crc >> 4) ^ s_crc_tab[crc & 0x0Fu];
    }
    return crc ^ 0xFFFFFFFFu;
}

static void put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)(v >> 8); }
static void put_u32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint16_t get_u16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ------------------------------------------------------------------ */
/* SPI 收发缓冲：必须放 DMA 可达内存（内部 RAM），加了 DMA_ATTR */
static DMA_ATTR uint8_t s_tx[NETDL_FRAME];
static DMA_ATTR uint8_t s_rx[NETDL_FRAME];
static DMA_ATTR uint8_t s_blk[NETDL_PAYLOAD];

static spi_device_handle_t s_dev = NULL;
static bool s_spi_ok = false;

/* 网页上传（路由挂在 wifimgr 的配置网页上） */
static WebServer *s_web     = NULL;
static bool       s_up_open = false;              /* 上传回调正在进行 */
static bool       s_up_ok   = false;              /* 最近一次上传结果 */
static char       s_up_name[NETDL_NAME_MAX];

static void up_get_page(void);   /* GET  /upload   上传页面 */
static void up_file(void);       /* POST /upload   上传回调（流式，反复调用） */
static void up_done(void);       /* POST /upload   上传结束后的结果页 */
static void up_status(void);     /* GET  /upstatus JSON，给别的页面轮询用 */

static volatile int      s_state   = NETDL_IDLE;
static volatile uint32_t s_written = 0;
static volatile uint32_t s_total   = 0;
static char     s_name[NETDL_NAME_MAX];
static char     s_msg[96];
/* 账号不放在这里了：由 wifimgr 的 NVS 账号表统一管理（wifimgr.h/cpp） */

typedef struct {
    uint8_t  status;
    uint8_t  errcode;
    uint16_t ack_type;
    uint16_t ack_seq;
    uint32_t written;
    uint32_t total;
    char     msg[65];
} netdl_reply_t;

static void msg_set(const char *t)
{
    strncpy(s_msg, t, sizeof(s_msg) - 1);
    s_msg[sizeof(s_msg) - 1] = 0;
}

static void msg_setf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_msg, sizeof(s_msg), fmt, ap);
    va_end(ap);
}

static void parse_reply(const uint8_t *rx, netdl_reply_t *r)
{
    memset(r, 0, sizeof(*r));
    if ((rx[NS_SYNC0] != (uint8_t)NETDL_SYNC1) || (rx[NS_SYNC1] != (uint8_t)NETDL_SYNC0)) {
        r->status = NS_ST_ERROR;
        r->errcode = 0xFFu;
        snprintf(r->msg, sizeof(r->msg),
                 "bad reply sync %02X %02X (STM32 no reply: MISO PB14->GPIO13? GND? new firmware?)",
                 (unsigned)rx[NS_SYNC0], (unsigned)rx[NS_SYNC1]);
        return;
    }
    r->status   = rx[NS_STATUS];
    r->errcode  = rx[NS_ERRCODE];
    r->ack_type = get_u16(rx + NS_ACKTYPE);
    r->ack_seq  = get_u16(rx + NS_ACKSEQ);
    r->written  = get_u32(rx + NS_WRITTEN);
    r->total    = get_u32(rx + NS_TOTAL);
    memcpy(r->msg, rx + NS_MSG, 64);
    r->msg[64] = 0;
}

/* 等 STM32 的 READY 拉高（它把两条 DMA 备好了才会拉高） */
static bool wait_ready(uint32_t timeout_ms)
{
    uint32_t t0 = millis();
    while (gpio_get_level((gpio_num_t)NETDL_READY_PIN) == 0) {
        if ((uint32_t)(millis() - t0) > timeout_ms) return false;
        vTaskDelay(1);
    }
    return true;
}

/* 一次事务：把 s_tx 里的 2064 B 推出去，收回 MISO */
static bool dl_xfer(netdl_reply_t *rep)
{
    if (!s_spi_ok) { msg_set("spi not ready"); return false; }
    if (!wait_ready(3000u)) { msg_set("STM32 READY timeout (link down?)"); return false; }

    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length    = (size_t)NETDL_FRAME * 8u;
    t.tx_buffer = s_tx;
    t.rx_buffer = s_rx;

    esp_err_t e = spi_device_transmit(s_dev, &t);
    if (e != ESP_OK) { msg_setf("spi transmit err %d", (int)e); return false; }

    if (rep) parse_reply(s_rx, rep);
    return true;
}

/* 组一帧并发送；rep 收到的是上一帧的应答（第一帧是上电初值，不可信） */
static bool dl_frame(uint8_t type, uint16_t seq, const uint8_t *payload, uint16_t len,
                     uint32_t total, netdl_reply_t *rep)
{
    if (len > NETDL_PAYLOAD) { msg_set("payload too big"); return false; }

    memset(s_tx, 0, NETDL_FRAME);
    s_tx[NM_SYNC0] = (uint8_t)NETDL_SYNC0;
    s_tx[NM_SYNC1] = (uint8_t)NETDL_SYNC1;
    s_tx[NM_TYPE]  = type;
    s_tx[NM_FLAGS] = 0u;
    put_u16(s_tx + NM_SEQ, seq);
    put_u16(s_tx + NM_LEN, len);
    put_u32(s_tx + NM_TOTAL, total);
    if ((payload != NULL) && (len > 0u)) {
        memcpy(s_tx + NM_PAYLOAD, payload, len);
        put_u32(s_tx + NM_CRC, netdl_crc32(payload, len));
    } else {
        put_u32(s_tx + NM_CRC, 0u);
    }
    return dl_xfer(rep);
}

/* 传输会话：网页上传用 T_BEGIN/T_DATA/T_END 流程，同一时刻只允许一个占用（s_owner）。 */
/* 会话只管 SPI 帧，不碰 HTTP/WebServer。 */
#define NETDL_OWNER_NONE 0u
#define NETDL_OWNER_WEB  2u

static volatile uint16_t s_seq       = 0u;   /* 本会话下一块的 seq */
static volatile uint32_t s_ses_total = 0u;   /* 本会话声明的总大小 */
static volatile uint8_t  s_owner     = NETDL_OWNER_NONE;

static bool ses_take(uint8_t who)
{
    if (s_owner != NETDL_OWNER_NONE) { msg_set("busy: another transfer is running"); return false; }
    s_owner     = who;
    s_seq       = 0u;
    s_ses_total = 0u;
    return true;
}

static void ses_release(void) { s_owner = NETDL_OWNER_NONE; }

/* T_BEGIN */
static bool ses_begin(const char *name, uint32_t total)
{
    netdl_reply_t rep;

    s_ses_total = total;
    /* 应答滞后一帧：T_BEGIN 这一帧读回来的是上一会话/上电初值的残留，不能当结果用。
       先把它丢掉（rep=NULL），再用紧随的一帧 T_PING 把 T_BEGIN 的真实结果读回来。 */
    if (!dl_frame(NT_BEGIN, 0u, (const uint8_t *)name, (uint16_t)strlen(name), total, NULL)) return false;
    if (!dl_frame(NT_PING,  0u, NULL, 0u, total, &rep)) return false;

    if (rep.status == NS_ST_ERROR) {
        msg_setf("STM32 refused BEGIN (%u): %s", (unsigned)rep.errcode, rep.msg);
        printf("[NETDL] STM32 BEGIN error %u (%s)\r\n", (unsigned)rep.errcode, rep.msg);
        return false;
    }
    if (rep.ack_type != NT_BEGIN) {
        printf("[NETDL] warn: BEGIN ack_type=%u (expect %u) - pipeline out of sync\r\n",
               (unsigned)rep.ack_type, (unsigned)NT_BEGIN);
    }
    s_written = rep.written;
    printf("[NETDL] BEGIN ok: %s total=%lu\r\n", name, (unsigned long)total);
    return true;
}

/* T_DATA（s_written 以 STM32 实际落盘字节数为准） */
static bool ses_data(const uint8_t *buf, uint16_t len)
{
    netdl_reply_t rep;

    if (len == 0u) return true;
    if (!dl_frame(NT_DATA, s_seq, buf, len, s_ses_total, &rep)) return false;
    if (rep.status == NS_ST_ERROR) {
        msg_setf("STM32 err %u: %s", (unsigned)rep.errcode, rep.msg);
        printf("[NETDL] STM32 error %u (%s)\r\n", (unsigned)rep.errcode, rep.msg);
        return false;
    }
    s_seq++;
    s_written = rep.written;
    return true;
}

/* T_END + 一帧 T_PING 收尾（应答滞后一帧，必须再发一帧才读得到 END 的结果） */
static bool ses_end(void)
{
    netdl_reply_t rep;

    if (!dl_frame(NT_END, 0u, NULL, 0u, s_ses_total, &rep)) return false;
    if (rep.status == NS_ST_ERROR) { msg_setf("END rejected: %s", rep.msg); return false; }

    if (!dl_frame(NT_PING, 0u, NULL, 0u, s_ses_total, &rep)) return false;
    if (rep.status == NS_ST_ERROR) { msg_setf("final ack failed: %s", rep.msg); return false; }

    s_written = rep.written;
    s_total   = (rep.total != 0u) ? rep.total : s_ses_total;
    return true;
}

static void ses_abort(void)
{
    netdl_reply_t rep;

    (void)dl_frame(NT_ABORT, 0u, NULL, 0u, 0u, &rep);
}

/* ------------------------------------------------------------------ */
static void netdl_spi_init(void)
{
    spi_bus_config_t bus;
    memset(&bus, 0, sizeof(bus));
    bus.mosi_io_num     = NETDL_SPI_MOSI;
    bus.miso_io_num     = NETDL_SPI_MISO;
    bus.sclk_io_num     = NETDL_SPI_SCK;
    bus.quadwp_io_num   = -1;
    bus.quadhd_io_num   = -1;
    bus.max_transfer_sz = NETDL_FRAME + 16;

    esp_err_t e = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (e != ESP_OK) { msg_setf("spi_bus_initialize err %d", (int)e); s_spi_ok = false; return; }

    spi_device_interface_config_t dev;
    memset(&dev, 0, sizeof(dev));
    dev.clock_speed_hz = NETDL_SPI_HZ;
    dev.mode           = 0;                 /* CPOL=0 CPHA=0，与 STM32 侧一致 */
    dev.spics_io_num   = NETDL_SPI_CS;
    dev.queue_size     = 1;

    e = spi_bus_add_device(SPI2_HOST, &dev, &s_dev);
    if (e != ESP_OK) { msg_setf("spi_bus_add_device err %d", (int)e); s_spi_ok = false; return; }

    gpio_config_t io;
    memset(&io, 0, sizeof(io));
    io.pin_bit_mask = (1ULL << NETDL_READY_PIN);
    io.mode         = GPIO_MODE_INPUT;
    io.pull_up_en   = GPIO_PULLUP_DISABLE;    /* 下拉：STM32 不驱动时读到低 = 没准备好 */
    io.pull_down_en = GPIO_PULLDOWN_ENABLE;
    io.intr_type    = GPIO_INTR_DISABLE;
    gpio_config(&io);

    s_spi_ok = true;
    msg_set("idle");
    printf("[NETDL] SPI master: SCK=%d MOSI=%d MISO=%d CS=%d READY=%d @ %d Hz\r\n",
           NETDL_SPI_SCK, NETDL_SPI_MOSI, NETDL_SPI_MISO, NETDL_SPI_CS,
           NETDL_READY_PIN, (int)NETDL_SPI_HZ);
}

void netdl_init(void)
{
    s_state = NETDL_IDLE;
    s_written = 0u;
    s_total = 0u;
    s_name[0] = 0;
    s_owner     = NETDL_OWNER_NONE;
    s_seq       = 0u;
    s_ses_total = 0u;
    s_up_open   = false;
    s_up_ok     = false;
    s_up_name[0] = 0;
    /* 账号由 wifimgr_init() 从 NVS 读回（详见 lib/wifimgr） */
    netdl_spi_init();

    /* 网页上传页挂在 wifimgr 的配置网页（同一个 80 端口）上 */
    s_web = wifimgr_web();
    if (s_web != NULL) {
        s_web->on("/upload",   HTTP_GET,  up_get_page);
        s_web->on("/upload",   HTTP_POST, up_done, up_file);
        s_web->on("/upstatus", HTTP_GET,  up_status);
        printf("[NETDL] web upload page ready: http://<ip>/upload\r\n");
    } else {
        printf("[NETDL] !! wifimgr web server not ready -> /upload disabled\r\n");
    }
}

bool netdl_busy(void)          { return (s_state == NETDL_RUNNING); }
int  netdl_state(void)         { return s_state; }
uint32_t netdl_written(void)   { return s_written; }
uint32_t netdl_total(void)     { return s_total; }
const char *netdl_name(void)   { return s_name; }
const char *netdl_msg(void)    { return s_msg; }

/* ------------------------------------------------------------------ */
/* 网页上传：浏览器选文件 -> HTTP POST（multipart 流式）-> SPI -> TF 卡
 *
 * 会话由 s_owner 独占：同一时刻只允许一个上传在跑。
 * 上传回调是流式的：浏览器每发来一块就调一次 up_file()，不做整文件缓存。
 * 文件总大小由页面放在 URL 里（?size=N），没带就用 0（STM32 收尾时按实际字节数算）。
 */

/* 把浏览器给的路径/名字清成合法文件名（只留 basename，过滤非法字符） */
static void up_name_clean(const char *in, char *out, size_t outsz)
{
    const char *base = in;
    size_t n = 0u;

    if (outsz == 0u) return;
    out[0] = 0;
    if (in == NULL) return;

    for (const char *p = in; *p != 0; p++) {
        if ((*p == '/') || (*p == '\\')) base = p + 1;
    }
    while ((base[n] != 0) && (n + 1u < outsz)) {
        unsigned char c = (unsigned char)base[n];
        if ((c < 0x20u) || (c == 0x7Fu) || (c == '"') || (c == '*') || (c == '?') ||
            (c == ':') || (c == '<') || (c == '>') || (c == '|') || (c == ' '))
        {
            c = (unsigned char)'_';
        }
        out[n++] = (char)c;
    }
    out[n] = 0;
}

bool netdl_upload_begin(const char *name, uint32_t total)
{
    char clean[NETDL_NAME_MAX];

    if (!s_spi_ok)                   { msg_set("spi not ready"); return false; }
    if (s_owner != NETDL_OWNER_NONE) { msg_set("busy: another transfer is running"); return false; }

    up_name_clean(name, clean, sizeof(clean));
    if (clean[0] == 0) { msg_set("empty filename"); return false; }

    if (!ses_take(NETDL_OWNER_WEB)) return false;
    wifimgr_set_busy(true);   /* 传输期间不让 WiFi 状态机切模式/重连 */

    strncpy(s_name, clean, sizeof(s_name) - 1u);
    s_name[sizeof(s_name) - 1u] = 0;
    s_state   = NETDL_RUNNING;
    s_written = 0u;
    s_total   = total;
    msg_set("uploading");

    if (!ses_begin(s_name, total)) {
        s_state = NETDL_FAIL;
        wifimgr_set_busy(false);
        ses_release();
        return false;
    }
    printf("[NETDL] upload begin %s (declared %lu B)\r\n", s_name, (unsigned long)total);
    return true;
}

bool netdl_upload_write(const uint8_t *data, uint32_t len)
{
    if ((s_owner != NETDL_OWNER_WEB) || (s_state != NETDL_RUNNING)) return false;

    while (len > 0u) {
        uint32_t n = (len > (uint32_t)NETDL_PAYLOAD) ? (uint32_t)NETDL_PAYLOAD : len;
        if (!ses_data(data, (uint16_t)n)) { s_state = NETDL_FAIL; return false; }
        data += n;
        len  -= n;
    }
    return true;
}

bool netdl_upload_end(void)
{
    bool ok;

    if (s_owner != NETDL_OWNER_WEB) return false;

    ok = (s_state == NETDL_RUNNING) ? ses_end() : false;
    if (ok) {
        s_state = NETDL_OK;
        msg_setf("ok %lu B", (unsigned long)s_written);
    } else {
        ses_abort();
        s_state = NETDL_FAIL;
    }
    printf("[NETDL] upload %s: %s (%lu B)\r\n", s_name, ok ? "OK" : "FAIL",
           (unsigned long)s_written);
    wifimgr_set_busy(false);
    ses_release();
    return ok;
}

void netdl_upload_abort(void)
{
    if (s_owner != NETDL_OWNER_WEB) return;
    ses_abort();
    s_state = NETDL_FAIL;
    msg_set("upload aborted by browser");
    printf("[NETDL] upload aborted, STM32 drops the .part\r\n");
    wifimgr_set_busy(false);
    ses_release();
}

bool netdl_upload_running(void) { return (s_owner == NETDL_OWNER_WEB); }

/* ---------------- 上传页面 / 结果页 ---------------- */
static void up_page_head(String &p, const char *title)
{
    p += "<!DOCTYPE html><html lang='zh-CN'><head><meta charset='utf-8'>";
    p += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
    p += "<title>";
    p += title;
    p += "</title><style>";
    p += "body{font-family:system-ui,Arial,sans-serif;margin:16px;max-width:680px;line-height:1.5}";
    p += "fieldset{border:1px solid #bbb;border-radius:8px;margin:10px 0;padding:10px}";
    p += "legend{font-weight:600}input,button{font-size:16px;padding:8px;margin:4px 0;box-sizing:border-box}";
    p += "input[type=file],input[type=text]{width:100%}";
    p += "button{background:#2d6cdf;color:#fff;border:0;border-radius:6px;padding:8px 12px}";
    p += "#bar{height:14px;background:#eee;border-radius:7px;overflow:hidden;margin:10px 0;display:none}";
    p += "#bar i{display:block;height:100%;width:0;background:#2d6cdf;transition:width .2s}";
    p += ".ok{color:#0a0}.bad{color:#c00}a{color:#2d6cdf}";
    p += "</style></head><body>";
}

static void up_page_tail(String &p)
{
    p += "<hr><p><a href='/'>&#8592; WiFi 配置页</a> &nbsp;|&nbsp; <a href='/upload'>再传一个</a></p>";
    p += "</body></html>";
}

static void up_get_page(void)
{
    String p;

    if (s_web == NULL) return;

    up_page_head(p, "ESP32 播放器 - 歌曲上传");
    p += "<h2>上传歌曲到 TF 卡</h2>";
    p += "<p>选一个 <b>mp3 / wav</b> 文件：浏览器把它 POST 给 ESP32，ESP32 边收边经 ";
    p += "<b>SPI</b> 转发给 STM32，STM32 用 FATFS 写进 TF 卡歌单目录。";
    p += "写卡时先用 <b>.part</b> 临时名，收完自动改名；中途断开会被自动删掉。</p>";
    if (s_state == NETDL_RUNNING) {
        p += "<p class='bad'>当前有传输在跑（下载或上传），请等它结束再传。</p>";
    }
    p += "<form id='uf' method='post' enctype='multipart/form-data' action='/upload'>";
    p += "<fieldset><legend>选择文件</legend>";
    p += "<input type='file' id='fi' name='file' accept='.mp3,.wav,audio/mpeg,audio/wav'>";
    p += "<p>存到卡上的名字（留空 = 用文件原名，<b>建议用英文名</b>）：<br>";
    p += "<input type='text' name='name' id='nm' placeholder='song.mp3'></p>";
    p += "</fieldset>";
    p += "<button type='submit'>开始上传</button>";
    p += "<div id='bar'><i></i></div>";
    p += "<p id='st'></p>";
    p += "</form>";
    p += "<script>";
    p += "var uf=document.getElementById('uf'),fi=document.getElementById('fi');";
    p += "uf.onsubmit=function(e){";
    p += "if(!fi.files||!fi.files.length){alert('先选一个文件');e.preventDefault();return;}";
    p += "if(!(window.FormData&&window.XMLHttpRequest)){return;}";
    p += "e.preventDefault();";
    p += "var f=fi.files[0],nm=document.getElementById('nm').value||f.name;";
    p += "if(!/\\.(mp3|wav)$/i.test(nm)){alert('文件名必须以 .mp3 或 .wav 结尾');return;}";
    p += "var fd=new FormData();fd.append('file',f);";
    p += "var x=new XMLHttpRequest();";
    p += "x.open('POST','/upload?name='+encodeURIComponent(nm)+'&size='+f.size,true);";
    p += "var bar=document.getElementById('bar'),inb=bar.firstChild,tx=document.getElementById('st');";
    p += "bar.style.display='block';tx.textContent='上传中，请勿关闭页面...';";
    p += "x.upload.onprogress=function(ev){if(ev.lengthComputable){var pc=Math.round(ev.loaded*100/ev.total);";
    p += "inb.style.width=pc+'%';tx.textContent='已发送 '+pc+'%  '+ev.loaded+' / '+ev.total+' B';}};";
    p += "x.onload=function(){document.open();document.write(x.responseText);document.close();};";
    p += "x.onerror=function(){tx.textContent='上传失败：连接断开';};";
    p += "x.send(fd);};";
    p += "</script>";
    up_page_tail(p);
    s_web->send(200, "text/html; charset=utf-8", p);
}

static void up_file(void)
{
    HTTPUpload &up = s_web->upload();

    if (up.status == UPLOAD_FILE_START) {
        String   nm;
        uint32_t sz = 0u;

        if (s_web->hasArg("name")) nm = s_web->arg("name");
        else                       nm = up.filename;
        if (s_web->hasArg("size")) sz = (uint32_t)strtoul(s_web->arg("size").c_str(), NULL, 10);

        s_up_open = netdl_upload_begin(nm.c_str(), sz);
        s_up_ok   = false;
        if (s_up_open) {
            strncpy(s_up_name, netdl_name(), sizeof(s_up_name) - 1u);
            s_up_name[sizeof(s_up_name) - 1u] = 0;
            printf("[NETDL] upload from browser: %s (%lu B declared)\r\n",
                   s_up_name, (unsigned long)sz);
        } else {
            printf("[NETDL] upload refused: %s\r\n", netdl_msg());
        }
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (s_up_open && (up.currentSize > 0u)) {
            if (!netdl_upload_write(up.buf, (uint32_t)up.currentSize)) {
                s_up_open = false;          /* 出错就不再往 SPI 喂 */
                printf("[NETDL] upload write failed: %s\r\n", netdl_msg());
            }
        }
    } else if (up.status == UPLOAD_FILE_END) {
        if (s_up_open) {
            s_up_ok   = netdl_upload_end();
            s_up_open = false;
        }
    } else {                                /* UPLOAD_FILE_ABORTED */
        if (s_up_open) { netdl_upload_abort(); s_up_open = false; }
        s_up_ok = false;
    }
}

static void up_done(void)
{
    String p;

    if (s_web == NULL) return;

    up_page_head(p, "ESP32 播放器 - 上传结果");
    p += "<h2>上传结果</h2><fieldset><legend>结果</legend>";
    if (s_up_ok) {
        p += "<p class='ok'>写卡成功：<b>";
        p += s_up_name;
        p += "</b> — ";
        p += String((unsigned long)netdl_written());
        p += " B</p><p>STM32 已把 <b>.part</b> 改名为正式文件。若当时正在播放，歌单会在停止播放后自动重扫，按 K3 切歌即可听到。</p>";
    } else {
        p += "<p class='bad'>失败：";
        p += netdl_msg();
        p += "</p><p>半成品已删除，可以重新上传。</p>";
    }
    p += "</fieldset>";
    up_page_tail(p);
    s_web->send(200, "text/html; charset=utf-8", p);
}

static void up_status(void)
{
    String j;

    if (s_web == NULL) return;

    j += "{\"state\":";
    j += String((int)netdl_state());
    j += ",\"written\":";
    j += String((unsigned long)netdl_written());
    j += ",\"total\":";
    j += String((unsigned long)netdl_total());
    j += ",\"msg\":\"";
    j += netdl_msg();
    j += "\"}";
    s_web->send(200, "application/json", j);
}
