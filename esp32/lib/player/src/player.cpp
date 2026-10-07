/*
 * player.cpp - ESP32-S3 侧播放器：收压缩流 -> Helix 解码 -> I2S 输出
 *
 * 数据流:
 *   UART1(1Mbps) --RESP_AUDIO--> [压缩环 256KB] --Helix--> [PCM环 256KB] --I2S--> MAX98357A
 *
 * 三个任务（player_init 创建）:
 *   link   收协议帧；按信用窗口向 STM32 要音频；周期 ping；上报播放位置
 *   decode 找 MP3 帧同步头 -> Helix 解码 -> 音量/限幅 -> 写 PCM 环
 *   i2s    从 PCM 环取 PCM -> 下混单声道 -> 写 I2S DMA
 *
 * 信用窗口(pull): 本侧只在压缩环有空位时发 CMD_AUDIO_REQ{blocks}，
 *   STM32 收到后按 1KB/次 发 RESP_AUDIO{seq,len,data}。数据不丢不重。
 *
 * seq 不连续 = 换曲/seek/丢帧 -> 硬复位（清两级缓冲 + 重建解码器）。
 * 同采样率换曲走「无缝」路径：不清缓冲，尾部自然播完接上新曲。
 *
 * 依赖: Arduino / FreeRTOS / driver/i2s / mp3dec(Helix) / proto.h / link.h / player.h
 * 调用者: src/main.cpp 的 setup() 里 player_init()
 */
#include <Arduino.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s.h"
#include "esp_heap_caps.h"
#include "mp3dec.h"
#include "proto.h"
#include "link.h"
#include "player.h"

/* ---------------- 引脚（可在 platformio.ini build_flags 里用 -D 覆盖） ----------------
 * 默认按 hardware/wiring-diagram.html：
 *   BCLK=5  LRCK=6  DIN=7  SD=8  LED=48
 * 接线不同时按同名宏覆盖，例如：
 *   -DPIN_I2S_BCLK=8 -DPIN_I2S_LRCK=9 -DPIN_I2S_DIN=10
 * MAX98357A 的 SD 若直接接 3V3（功放常开），加 -DPIN_AMP_SD=-1
 *
 * SD/MODE 不是单纯的使能脚，它同时选声道（MAX98357A 数据手册）：
 *   SD < 0.16V      -> 关断
 *   0.16 ~ 0.77V    -> 输出 (L+R)/2
 *   0.77 ~ 1.4V     -> 只输出右声道
 *   SD > 1.4V       -> 只输出左声道
 * 本机 SD 硬接 3V3 => 只出左声道；固件因此在写 I2S 前把立体声下混成
 * 单声道并复制到左右两槽，任何 SD 档位放出来都是完整的 (L+R)/2 混合。
 */
#ifndef PIN_I2S_BCLK
#define PIN_I2S_BCLK   5
#endif
#ifndef PIN_I2S_LRCK
#define PIN_I2S_LRCK   6
#endif
#ifndef PIN_I2S_DIN
#define PIN_I2S_DIN    7
#endif
#ifndef PIN_AMP_SD
#define PIN_AMP_SD     8        /* -1 = SD 直接接 3V3，不由 MCU 控制 */
#endif
#ifndef PIN_LED
#define PIN_LED        48
#endif

/* ---------------- 参数 ---------------- */
#define IN_CAP          (256u * 1024u)   /* 压缩数据环（PSRAM 有 8MB，留足抗抖动余量） */
#define PCM_CAP         (256u * 1024u)   /* PCM 环     */
#define DEC_BUF         (6u * 1024u)     /* 解码器线性输入缓冲 */
#define PCM_CHUNK       2048u            /* 每次写 I2S 的字节数 */
#define CREDIT_CHUNK    1024u
#define CREDIT_MAX_REQ  16u
/* 在途额度上限：必须 <= STM32 侧 AUDIO_CREDIT_MAX(64)，否则超发的额度会被 STM32
   夹掉、ESP32 却以为"在途"，于是永远不再申请 -> 只靠看门狗喂数据 -> 断续杂音。 */
#define CREDIT_MAX_INFLIGHT (48u * 1024u)
#define PCM_PREBUF      (24u * 1024u)    /* 起播预缓冲：±139ms@44.1k 立体声 */
#define RATE_DEF        44100
#define I2S_DMA_CNT     8
#define I2S_DMA_LEN     512
#define VOL_DEF         80
/* 软限幅：把峰值限制在该电平以内，避免功放在大音量/强高音时硬削波（破音），
   用一点动态余量换输出余量。16384 = 0.5FS = -6dB，越小余量越大、越安静。 */
#define LIM_THR         16384
#define LIM_CLAMP       32000

/* ---------------- 环形缓冲 (SPSC, 容量需 2 的幂) ---------------- */
class Ring {
public:
    bool init(size_t cap) {
        m_cap = cap;
        m_mask = cap - 1u;
        m_buf = (uint8_t *)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
        m_psram = (m_buf != 0);
        if (m_buf == 0) m_buf = (uint8_t *)heap_caps_malloc(cap, MALLOC_CAP_INTERNAL);
        m_rd = 0; m_wr = 0;
        return m_buf != 0;
    }
    size_t size()  { return (size_t)(m_wr - m_rd); }
    size_t space() { return m_cap - size(); }
    bool   psram() { return m_psram; }

    size_t write(const uint8_t *src, size_t n) {
        size_t avail = space();
        if (n > avail) n = avail;
        size_t i = (size_t)(m_wr & m_mask);
        size_t first = m_cap - i;
        if (first > n) first = n;
        memcpy(m_buf + i, src, first);
        if (n > first) memcpy(m_buf, src + first, n - first);
        __sync_synchronize();
        m_wr += n;
        return n;
    }

    size_t peek(uint8_t *dst, size_t n) {
        return peek_at(0u, dst, n);
    }

    /* 从读指针偏移 off 处取数据。给解码器补 scratch 必须用这个：peek 永远从
       m_rd 起，直接往尾巴上追加会把头部字节重复拷一遍。 */
    size_t peek_at(size_t off, uint8_t *dst, size_t n) {
        size_t avail = size();
        if (off >= avail) return 0u;
        avail -= off;
        if (n > avail) n = avail;
        size_t i = (size_t)((m_rd + off) & m_mask);
        size_t first = m_cap - i;
        if (first > n) first = n;
        memcpy(dst, m_buf + i, first);
        if (n > first) memcpy(dst + first, m_buf, n - first);
        return n;
    }

    void skip(size_t n) {
        if (n > size()) n = size();
        __sync_synchronize();
        m_rd += n;
    }

    void flush() {
        __sync_synchronize();
        m_rd = m_wr;
    }

private:
    uint8_t *m_buf = 0;
    size_t   m_cap = 0, m_mask = 0;
    volatile size_t m_rd = 0, m_wr = 0;
    bool     m_psram = false;
};

/* ---------------- 全局状态 ---------------- */
static Ring s_in, s_pcm;

static volatile uint8_t  s_playing = 0;        /* 来自 RESP_STATE */
static volatile uint16_t s_st_track = 0;       /* STM32 当前曲目(0起) */
static volatile uint16_t s_st_total = 0;
static volatile uint8_t  s_st_mode = 0;
static volatile uint8_t  s_st_vol = VOL_DEF;

static volatile int32_t  s_rate_req = 0;       /* 解码器要求的采样率 */
static volatile int32_t  s_rate_cur = 0;       /* I2S 当前采样率     */
static volatile int32_t  s_rate_pend = 0;      /* 候选新采样率(连续2帧才确认) */
static volatile uint32_t s_rate_pend_cnt = 0;

static volatile uint8_t  s_need_reset = 0;     /* link 置位，decode 执行 */
static volatile uint32_t s_flush_req = 0;      /* decode 置位，i2s 执行  */
static volatile uint32_t s_flush_done = 0;
static volatile uint32_t s_reset_count = 0;
static volatile uint32_t s_last_reset_ms = 0;

static volatile uint32_t s_seq_expect = 0;     /* 期望的下一个 seq */
static volatile uint8_t  s_seq_valid = 0;

static volatile uint32_t s_written_us = 0;     /* 已交给 DMA 的音频时长(本曲起点算) */
static volatile uint16_t s_pos_track = 0;      /* s_written_us 对应的曲目 */
static volatile uint32_t s_base_ms = 0;        /* 本曲起点对应的绝对毫秒(seek 用) */
static volatile uint8_t  s_base_armed = 0;     /* 复位后短时间内接受 RESP_TIME 作为起点 */
static volatile uint32_t s_base_arm_ms = 0;
static volatile uint32_t s_base_req = 0;       /* 本次复位应使用的起点(seek 目标) */
static volatile uint8_t  s_base_req_ok = 0;    /* 起点是否可信(不可信就不上报位置) */
static volatile uint8_t  s_base_ok = 0;
static volatile uint32_t s_last_time_val = 0;  /* 最近一次 RESP_TIME 的位置 */
static volatile uint32_t s_last_time_at = 0;
static volatile uint32_t s_track_change_at = 0;/* 最近一次 RESP_STATE 换曲时刻 */

/* 字节绝对计数：rx = 已收进压缩环的字节，dec = 解码器已消费的字节 */
static volatile uint64_t s_rx_total = 0;
static volatile uint64_t s_dec_total = 0;
/* PCM 绝对计数：dec = 已产出，play = 已播(写进 DMA) */
static volatile uint64_t s_pcm_dec_total = 0;
static volatile uint64_t s_pcm_play_total = 0;

/* 无缝换曲：收到 RESP_END 后下一个 seq 跳变视为换曲边界，不丢缓冲 */
static volatile uint8_t  s_prebuf = 1u;         /* 1 = 等 PCM 填到阈值再开声 */
static volatile uint8_t  s_end_seen = 0;
static volatile uint8_t  s_split_pending = 0;
static volatile uint64_t s_split_at = 0;

/* 到某个 PCM 播放点再生效的边界事件 */
static volatile uint8_t  s_evt_valid = 0;
static volatile uint64_t s_evt_pcm = 0;
static volatile uint8_t  s_evt_reset_pos = 0;
static volatile int32_t  s_evt_rate = 0;

/* 统计 */
static volatile uint32_t s_stat_audio = 0, s_stat_abytes = 0, s_stat_drop = 0;
static volatile uint32_t s_stat_dec_err = 0, s_stat_underrun = 0, s_stat_frames = 0;
static volatile uint16_t s_stat_end_track = 0;
static volatile uint8_t  s_stat_end_reason = 0;
static volatile uint32_t s_stat_end = 0, s_stat_pong = 0;
static volatile uint16_t s_stat_drops32 = 0;
static volatile uint32_t s_stat_overrun = 0;   /* 压缩环满导致的字节丢失次数 */
static volatile uint32_t s_stat_ovr_bytes = 0; /* 因环满被丢弃的字节数        */
static volatile uint32_t s_stat_dec_ok = 0;    /* 成功解出的帧数              */
static volatile uint32_t s_stat_resync = 0;    /* 重锁帧网格(清解码器)次数    */
static volatile uint32_t s_stat_pcm_dry = 0;   /* I2S 取不到 PCM（饿死）次数 */
static volatile uint32_t s_req_out = 0;        /* 累计请求出去的 1KB 块数 */
static volatile uint32_t s_req_wd  = 0;        /* 看门狗强制请求次数      */
static volatile uint32_t s_last_audio_ms = 0;  /* 最近收到音频帧的时刻    */
static volatile uint32_t s_last_resync_ms = 0; /* 上次重锁帧网格的时刻    */
static volatile uint32_t s_stat_skip_bytes = 0;/* 重锁/丢弃掉的非帧字节总数 */
static volatile uint32_t s_stat_nosync = 0;    /* 缓冲区里找不到同步头次数 */
static volatile uint32_t s_stat_holes = 0;     /* 音频 seq 空洞(真丢帧)次数*/
static volatile uint32_t s_hole_log = 0;       /* HOLE 已打印条数          */

/* 真正进了压缩环的字节数。信用窗口必须用它来对齐在途额度：被丢掉的字节不能算
   "已收到"，否则 inflight 偏小 -> 继续授权 -> 丢得越多授权越多（正反馈）。 */
static inline uint32_t rx_eff_bytes(void)
{
    return (s_stat_abytes > s_stat_ovr_bytes) ? (s_stat_abytes - s_stat_ovr_bytes) : 0u;
}

static volatile int32_t  s_vol_scale = (int32_t)((uint32_t)VOL_DEF * 65536u / 100u);
static volatile int32_t  s_lim_g     = 65536;   /* 限幅器当前增益 (Q16) */

static HMP3Decoder s_dec = 0;

/* ---------------- 工具 ---------------- */
static uint32_t now_ms(void) { return (uint32_t)millis(); }

static void led_set(uint8_t r, uint8_t g, uint8_t b)
{
    if (PIN_LED < 0) return;
    neopixelWrite(PIN_LED, r, g, b);
}

static void request_reset(const char *why)
{
    (void)why;
    if (s_need_reset == 0) {
        s_need_reset = 1;
        s_seq_valid = 0;
    }
}

/* ---------------- I2S ---------------- */
static void i2s_hw_init(void)
{
    i2s_config_t cfg;
    i2s_pin_config_t pins;

    memset(&cfg, 0, sizeof(cfg));
    cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    cfg.sample_rate          = RATE_DEF;
    cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count        = I2S_DMA_CNT;
    cfg.dma_buf_len          = I2S_DMA_LEN;
    cfg.use_apll             = false;
    cfg.tx_desc_auto_clear   = true;
    cfg.fixed_mclk           = 0;

    ESP_ERROR_CHECK(i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL));

    memset(&pins, 0, sizeof(pins));
    pins.mck_io_num   = I2S_PIN_NO_CHANGE;
    pins.bck_io_num   = PIN_I2S_BCLK;
    pins.ws_io_num    = PIN_I2S_LRCK;
    pins.data_out_num = PIN_I2S_DIN;
    pins.data_in_num  = I2S_PIN_NO_CHANGE;
    ESP_ERROR_CHECK(i2s_set_pin(I2S_NUM_0, &pins));

    i2s_zero_dma_buffer(I2S_NUM_0);
    s_rate_cur = RATE_DEF;

#if (PIN_AMP_SD >= 0)
    pinMode(PIN_AMP_SD, OUTPUT);
    digitalWrite(PIN_AMP_SD, HIGH);      /* 功放使能（高=工作） */
#else
    /* SD 直接接 3V3：功放常开，不占 GPIO */
#endif
}

/* ---------------- 开机自检音 ----------------
 * 只为验证 I2S -> MAX98357A -> 喇叭 这一段的硬件通路：听到一声「哔」说明
 * 功放/喇叭/引脚接线正常，问题不在这一段。
 */
#define BEEP_MS   300
#define BEEP_HZ   1000
#define BEEP_AMP  7000.0f
static void boot_beep(void)
{
    static int16_t chunk[256 * 2];
    uint32_t total = (uint32_t)((uint64_t)RATE_DEF * BEEP_MS / 1000ull);
    uint32_t fade  = (uint32_t)((uint64_t)RATE_DEF * 8u / 1000ull);   /* 8ms 淡入淡出防咔哒 */
    uint32_t done  = 0u;

    if (fade == 0u) fade = 1u;
    printf("[PLAY] boot beep %d Hz %d ms\r\n", BEEP_HZ, BEEP_MS);

    while (done < total) {
        uint32_t n = total - done;
        if (n > 256u) n = 256u;
        for (uint32_t i = 0u; i < n; i++) {
            uint32_t k = done + i;
            float    env = 1.0f;
            float    ph  = 2.0f * 3.14159265f * (float)BEEP_HZ * (float)k / (float)RATE_DEF;
            int16_t  s;
            if (k < fade)               env = (float)k / (float)fade;
            else if (k > total - fade)  env = (float)(total - k) / (float)fade;
            s = (int16_t)(sinf(ph) * BEEP_AMP * env);
            chunk[2 * i]     = s;
            chunk[2 * i + 1] = s;
        }
        size_t wr = 0;
        if (i2s_write(I2S_NUM_0, chunk, (size_t)n * 4u, &wr, portMAX_DELAY) != ESP_OK) break;
        done += n;
    }
    vTaskDelay(60);                      /* 等最后几个 DMA 块播完 */
    i2s_zero_dma_buffer(I2S_NUM_0);
}

/* ---------------- 边界事件 ---------------- */
static portMUX_TYPE s_evt_mux = portMUX_INITIALIZER_UNLOCKED;

/* 记录一个"到某个 PCM 播放点再生效"的事件：
 *   reset_pos=1 -> 进度归零(换曲/seek 边界)
 *   rate>0      -> 切换 I2S 采样率
 * 事件点用绝对 PCM 字节计数表示，由 I2S 任务在播到该点时应用。 */
static void set_event(uint64_t pcm, uint8_t reset_pos, int32_t rate)
{
    portENTER_CRITICAL(&s_evt_mux);
    if (s_evt_valid == 0u) {
        s_evt_pcm = pcm;
        s_evt_reset_pos = reset_pos;
        s_evt_rate = rate;
        s_evt_valid = 1u;
    } else {
        if (reset_pos != 0u) s_evt_reset_pos = 1u;
        if (rate > 0)        s_evt_rate = rate;
    }
    portEXIT_CRITICAL(&s_evt_mux);
}

/* ---------------- MP3 帧头校验（同步搜索） ----------------
 * Helix 的 MP3FindSyncWord 只做 12 位匹配(FF Ex)，压缩数据里随机撞上就能"对上"。
 * 一旦锁到假头，解码器会从错误的帧边界一路解垃圾（听感=持续杂音 + 采样率乱跳），
 * 而且很难自己爬回真帧网格。这里要求"候选头 + 按它算出的帧长处的下一个头"
 * 版本/层一致，才能真正锁上，换曲/seek/丢包后的重锁也就变成瞬间的事。 */
static const uint16_t s_br_v1[16]  = {0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0};
static const uint16_t s_br_v2[16]  = {0, 8,16,24,32,40,48,56, 64, 80, 96,112,128,144,160,0};
static const uint16_t s_sr_v1[4]   = {44100u, 48000u, 32000u, 0u};
static const uint16_t s_sr_v2[4]   = {22050u, 24000u, 16000u, 0u};
static const uint16_t s_sr_v25[4]  = {11025u, 12000u,  8000u, 0u};

/* 解析 MPEG1/2/2.5 Layer III 帧头；合法返回帧长(字节)，否则 0 */
static int mp3_hdr_len(const uint8_t *b, int *sr_out, int *ver_out)
{
    int ver, layer, brx, srx, pad, br, sr, len;
    if ((b[0] != 0xFFu) || ((b[1] & 0xE0u) != 0xE0u)) return 0;
    ver   = (b[1] >> 3) & 3;      /* 0=MPEG2.5 1=保留 2=MPEG2 3=MPEG1 */
    layer = (b[1] >> 1) & 3;      /* 1 = Layer III */
    if ((ver == 1) || (layer != 1)) return 0;
    brx = (b[2] >> 4) & 15;
    srx = (b[2] >> 2) & 3;
    pad = (b[2] >> 1) & 1;
    if ((brx == 0) || (brx == 15) || (srx == 3)) return 0;
    br = (ver == 3) ? s_br_v1[brx] : s_br_v2[brx];
    if (br == 0) return 0;
    sr = (ver == 3) ? s_sr_v1[srx] : ((ver == 2) ? s_sr_v2[srx] : s_sr_v25[srx]);
    if (sr == 0) return 0;
    len = ((ver == 3) ? 144 : 72) * br * 1000 / sr + pad;   /* Layer III 帧长 */
    if (sr_out)  *sr_out  = sr;
    if (ver_out) *ver_out = ver;
    return len;
}

/* 在 buf[0..n) 里找一个可信的帧起点；找不到返回 -1。
 * 数据不够校验下一个头时先接受候选（避免尾部卡死）。 */
static int find_frame_sync(const uint8_t *buf, int n)
{
    int i;
    if (n < 4) return -1;
    for (i = 0; i + 4 <= n; i++) {
        int sr = 0, ver = 0;
        int flen = mp3_hdr_len(buf + i, &sr, &ver);
        if (flen <= 0) continue;
        if (i + flen + 4 <= n) {
            int sr2 = 0, ver2 = 0;
            if (mp3_hdr_len(buf + i + flen, &sr2, &ver2) <= 0) continue;
            if (ver2 != ver) continue;         /* VBR 码率可变，但版本/层必须一致 */
        }
        return i;
    }
    return -1;
}


/* ---------------- 解码任务 ---------------- */
static void task_decode(void *arg)
{
    (void)arg;
    uint8_t *scratch = (uint8_t *)heap_caps_malloc(DEC_BUF, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    int16_t *pcm_out = (int16_t *)heap_caps_malloc(2304 * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    int16_t *stage   = (int16_t *)heap_caps_malloc(2304 * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    int fill = 0, used = 0;

    if ((scratch == 0) || (pcm_out == 0) || (stage == 0)) {
        printf("[PLAY] decode: no memory\r\n");
        vTaskDelete(NULL);
        return;
    }

    s_dec = MP3InitDecoder();
    printf("[PLAY] helix decoder %s\r\n", s_dec ? "ready" : "FAIL");

    for (;;) {
        /* --- 硬复位（seek / 手动切歌 / 丢帧）：丢弃全部已缓冲音频 --- */
        if (s_need_reset) {
            s_flush_req++;
            uint32_t t0 = now_ms();
            while ((s_flush_done < s_flush_req) && ((uint32_t)(now_ms() - t0) < 400u))
                vTaskDelay(1);
            s_dec_total += (uint64_t)s_in.size();   /* 丢弃的字节也算已消费 */
            s_in.flush();
            if (s_dec) { MP3FreeDecoder(s_dec); s_dec = 0; }
            s_dec = MP3InitDecoder();
            fill = 0; used = 0;
            s_split_pending = 0;
            s_end_seen = 0;
            s_written_us = 0;
            s_pos_track = s_st_track;
            s_base_ms = s_base_req;                 /* seek 目标位置 */
            s_base_ok = s_base_req_ok;
            s_base_req = 0;
            s_base_req_ok = 0;
            s_base_armed = 1;
            s_base_arm_ms = now_ms();
            s_prebuf = 1u;                          /* 复位后重新预缓冲 */
            s_reset_count++;
            s_need_reset = 0;
            printf("[PLAY] hard reset #%lu track=%u base=%lu ms\r\n",
                   (unsigned long)s_reset_count, (unsigned)(s_st_track + 1u),
                   (unsigned long)s_base_ms);
            continue;
        }

        /* --- 把上一轮解掉的字节出队；到换曲点就记事件（无缝切歌） --- */
        if (used > 0) {
            s_in.skip((size_t)used);
            s_dec_total += (uint64_t)used;
            if (fill > used) memmove(scratch, scratch + used, (size_t)(fill - used));
            fill -= used;
            used = 0;

            if ((s_split_pending != 0) && (s_dec_total >= s_split_at)) {
                s_split_pending = 0;
                set_event(s_pcm_dec_total, 1u, 0);
                if (s_dec) { MP3FreeDecoder(s_dec); s_dec = 0; }
                s_dec = MP3InitDecoder();           /* 清 bit reservoir，防跨曲串扰 */
                printf("[PLAY] seamless boundary at pcm %lu bytes\r\n",
                       (unsigned long)s_pcm_dec_total);
            }
        }

        /* --- PCM 环空间 / 压缩环数据 --- */
        if (s_pcm.space() < 6144u) { vTaskDelay(2); continue; }
        if (s_in.size() == 0u && (fill - used) < 16) { vTaskDelay(1); continue; }

        if (fill < (int)DEC_BUF) {
            size_t n = s_in.peek_at((size_t)fill, scratch + fill, DEC_BUF - (size_t)fill);
            fill += (int)n;
        }
        if ((fill - used) < 64) { vTaskDelay(1); continue; }

        /* --- 找帧同步头（双帧校验，见 find_frame_sync 注释） --- */
        int off = find_frame_sync(scratch + used, (int)(fill - used));
        if (off < 0) {
            /* 找不到可信帧头：只保留尾部 512 字节(帧长最大 ~1441，留足再校验的余量)，
               其余确认是垃圾的字节丢掉，等更多数据。 */
            s_stat_nosync++;
            if ((fill - used) > 1024) {
                s_stat_skip_bytes += (uint32_t)(fill - used - 512);
                used = fill - 512;
            }
            vTaskDelay(1);
            continue;
        }
        if (off > 0) {
            /* 跳过字节 = 解码器与真帧网格错位了。清 bit reservoir 重建解码器：
               旧残留数据接着用会解出杂音，重建只是这一帧先给静音，干净得多。 */
            uint32_t nowr = now_ms();
            s_stat_resync++;
            s_stat_skip_bytes += (uint32_t)off;
            if ((s_dec != 0) && ((uint32_t)(nowr - s_last_resync_ms) > 200u)) {
                MP3FreeDecoder(s_dec);
                s_dec = MP3InitDecoder();
                s_last_resync_ms = nowr;
            }
        }
        used += off;

        /* --- 解一帧 --- */
        unsigned char *ip = scratch + used;
        int left = fill - used;
        int before = left;
        int err = MP3Decode(s_dec, &ip, &left, pcm_out, 0);
        int consumed = before - left;

        if (err == ERR_MP3_INDATA_UNDERFLOW) { s_stat_underrun++; vTaskDelay(1); continue; }
        if ((err != 0) || (consumed <= 0)) {
            /* 解码器已按自己的规则前进了 consumed 字节（例如 MAINDATA_UNDERFLOW 时整帧
               都已进 bit reservoir）。这里若只前进 1 字节，下一轮就从帧中间找同步头，
               而 FF Ex 只是 12 位匹配，很容易撞上假头解出垃圾帧（听感 = 杂音 +
               采样率乱跳）。尊重解码器真实消费量，保持帧网格对齐；consumed==0
               （如 INVALID_FRAMEHEADER 什么都不消费）才退化成前进 1 字节。 */
            s_stat_dec_err++;
            used += (consumed > 0) ? consumed : 1;
            continue;
        }
        used += consumed;
        s_stat_dec_ok++;

        MP3FrameInfo fi;
        MP3GetLastFrameInfo(s_dec, &fi);
        if ((fi.samprate <= 0) || (fi.outputSamps <= 0)) continue;

        /* 采样率变化：在同一个 PCM 点切 I2S 时钟（不丢已缓冲音频）。
           连续 2 帧给出同一个新采样率才真的切：流中间误判一帧就让 I2S 时钟来回跳，
           听感是"滋啦/机器人声"。真换曲时新采样率会连续出现很多帧，不会漏。 */
        if (fi.samprate != s_rate_req) {
            if (fi.samprate == s_rate_pend) {
                if (s_rate_pend_cnt < 4u) s_rate_pend_cnt++;
            } else {
                s_rate_pend = fi.samprate;
                s_rate_pend_cnt = 1u;
            }
            if ((s_rate_req == 0) || (s_rate_pend_cnt >= 2u)) {
                if (s_rate_req > 0)
                    printf("[PLAY] samplerate %ld -> %d\r\n", (long)s_rate_req, fi.samprate);
                set_event(s_pcm_dec_total, 0u, fi.samprate);
                s_rate_req      = fi.samprate;
                s_rate_pend     = 0;
                s_rate_pend_cnt = 0u;
            }
        } else {
            s_rate_pend     = 0;
            s_rate_pend_cnt = 0u;
        }

        int nch = (fi.nChans == 1) ? 1 : 2;
        int per = fi.outputSamps / nch;
        if (per > 1152) per = 1152;
        if (per <= 0) continue;

        if (nch == 2) {
            memcpy(stage, pcm_out, (size_t)per * 4u);
        } else {
            for (int i = 0; i < per; i++) { stage[2 * i] = pcm_out[i]; stage[2 * i + 1] = pcm_out[i]; }
        }

        {
            size_t total = (size_t)per * 4u;
            size_t done = 0;
            const uint8_t *src = (const uint8_t *)stage;
            uint32_t t0 = now_ms();
            while (done < total) {
                size_t w = s_pcm.write(src + done, total - done);
                done += w;
                if (done < total) {
                    if ((uint32_t)(now_ms() - t0) > 500u) break;
                    vTaskDelay(1);
                }
            }
            s_pcm_dec_total += (uint64_t)done;
        }
        s_stat_frames++;
    }
}

/* ---------------- I2S 输出任务 ---------------- */
static void task_i2s(void *arg)
{
    (void)arg;
    uint8_t *buf = (uint8_t *)heap_caps_malloc(PCM_CHUNK, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (buf == 0) { printf("[PLAY] i2s: no dma mem\r\n"); vTaskDelete(NULL); return; }

    uint8_t paused_done = 0;
    uint32_t last_vol = 0xFFFFFFFFu;

    for (;;) {
        /* 硬复位：清 DMA + 清 PCM 环 + 丢弃未播计数 */
        if (s_flush_req != s_flush_done) {
            portENTER_CRITICAL(&s_evt_mux);
            s_evt_valid = 0u;
            s_evt_reset_pos = 0u;
            s_evt_rate = 0;
            portEXIT_CRITICAL(&s_evt_mux);
            i2s_zero_dma_buffer(I2S_NUM_0);
            s_pcm.flush();
            s_pcm_play_total = s_pcm_dec_total;
            s_written_us = 0;
            s_flush_done = s_flush_req;
            paused_done = 0;
            continue;
        }

        /* 边界事件：播放进度追到事件点再生效 */
        if (s_evt_valid != 0u) {
            if (s_pcm_play_total >= s_evt_pcm) {
                int32_t nr;
                uint8_t rp;
                portENTER_CRITICAL(&s_evt_mux);
                nr = s_evt_rate;
                rp = s_evt_reset_pos;
                s_evt_valid = 0u;
                s_evt_reset_pos = 0u;
                s_evt_rate = 0;
                portEXIT_CRITICAL(&s_evt_mux);

                if ((nr > 0) && (nr != s_rate_cur)) {
                    i2s_set_clk(I2S_NUM_0, (uint32_t)nr, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);
                    i2s_zero_dma_buffer(I2S_NUM_0);
                    s_rate_cur = nr;
                    printf("[PLAY] i2s clk = %ld Hz\r\n", (long)nr);
                }
                if (rp != 0u) {
                    s_written_us = 0;
                    s_pos_track = s_st_track;
                    s_base_ms = 0;
                    s_base_ok = 1u;
                    printf("[PLAY] pos reset -> track %u\r\n", (unsigned)(s_pos_track + 1u));
                }
                continue;
            }
        }

        /* 采样率还没确定（复位后第一帧之前）就先不喂 */
        if (s_rate_cur <= 0) { vTaskDelay(2); continue; }

        /* 未播放：静音并停喂 */
        if (s_playing == 0u) {
            if (paused_done == 0u) { i2s_zero_dma_buffer(I2S_NUM_0); paused_done = 1; }
            vTaskDelay(10);
            continue;
        }
        paused_done = 0;

        /* 起播预缓冲：攒够再开声，避免开头断音 */
        if (s_prebuf != 0u) {
            if (s_pcm.size() < PCM_PREBUF) { vTaskDelay(5); continue; }
            s_prebuf = 0u;
            printf("[PLAY] prebuffer done, start\r\n");
        }

        if (s_pcm.size() == 0u) { s_stat_pcm_dry++; vTaskDelay(2); continue; }

        size_t n = s_pcm.peek(buf, PCM_CHUNK);

        /* 不跨越事件点，保证切换发生在干净边界 */
        if (s_evt_valid != 0u) {
            uint64_t room = s_evt_pcm - s_pcm_play_total;
            if ((uint64_t)n > room) n = (size_t)room;
            if (n < 4u) { vTaskDelay(1); continue; }
        }
        if (n < 4u) { vTaskDelay(1); continue; }

        /* 立体声 -> 单声道下混（(L+R)/2 复制到左右两个槽）。
           原因：MAX98357A 的 SD/MODE 引脚既管开/关又管选声道：
             <0.16V 关断 | 0.16~0.77V 输出 (L+R)/2 | 0.77~1.4V 只出右声道 | >1.4V 只出左声道。
           本机 SD 直接接 3V3（>1.4V，只出左声道）——若固件仍送立体声，右声道内容就丢了。
           下混成单声道后，无论 SD 落在哪个区间，放出来的都是完整的 (L+R)/2 混合，音质一致。 */
        {
            int16_t *p    = (int16_t *)buf;
            size_t   frames = n / 4u;
            for (size_t i = 0; i < frames; i++) {
                int32_t l = p[2u * i];
                int32_t r = p[2u * i + 1u];
                int16_t m = (int16_t)((l + r) >> 1);
                p[2u * i]      = m;
                p[2u * i + 1u] = m;
            }
        }

        /* 音量（软件增益，MAX98357A 无数字音量） */
        uint8_t vol = s_st_vol;
        if (vol != last_vol) {
            if (vol > 100u) vol = 100u;
            s_vol_scale = (int32_t)((uint32_t)vol * 65536u / 100u);
            last_vol = vol;
        }
        int32_t g = s_vol_scale;
        if (g != 65536) {
            int16_t *p = (int16_t *)buf;
            size_t ns = n / 2u;
            for (size_t i = 0; i < ns; i++) {
                int32_t v = ((int32_t)p[i] * g) >> 16;
                if (v > 32767) v = 32767;
                if (v < -32768) v = -32768;
                p[i] = (int16_t)v;
            }
        }

        /* 软限幅：块内先找峰值 -> 目标增益 -> 块内线性插值平滑过渡（不改变波形形状），
           最后再做 ±LIM_CLAMP 安全夹紧。目的是让功放在大音量/强高音下不再硬削波（破音）。 */
        {
            int16_t *p  = (int16_t *)buf;
            size_t   ns = n / 2u;
            int32_t  pk = 0;
            int32_t  tgt;
            for (size_t i = 0; i < ns; i++) {
                int32_t a = (int32_t)p[i];
                if (a < 0) a = -a;
                if (a > pk) pk = a;
            }
            tgt = 65536;
            if (pk > (int32_t)LIM_THR) tgt = (int32_t)(((int64_t)LIM_THR << 16) / (int64_t)pk);
            if (tgt > 65536) tgt = 65536;
            if (tgt > s_lim_g) tgt = s_lim_g + ((tgt - s_lim_g) >> 2);   /* 释放慢一点，避免抽气感 */
            {
                int32_t g0 = s_lim_g;
                int32_t dg = tgt - g0;
                for (size_t i = 0; i < ns; i++) {
                    int32_t gg = g0 + (int32_t)(((int64_t)dg * (int64_t)i) / (int64_t)(ns > 0u ? ns : 1u));
                    int32_t v  = (int32_t)(((int64_t)p[i] * gg) >> 16);
                    if (v >  LIM_CLAMP) v =  LIM_CLAMP;
                    if (v < -LIM_CLAMP) v = -LIM_CLAMP;
                    p[i] = (int16_t)v;
                }
                s_lim_g = tgt;
            }
        }

        size_t wr = 0;
        if (i2s_write(I2S_NUM_0, buf, n, &wr, portMAX_DELAY) != ESP_OK) { vTaskDelay(2); continue; }
        s_pcm.skip(wr);
        s_pcm_play_total += (uint64_t)wr;
        if ((s_rate_cur > 0) && (wr > 0u)) {
            uint32_t us = (uint32_t)(((uint64_t)wr * 1000000ull) / ((uint64_t)s_rate_cur * 4ull));
            s_written_us += us;
        }
    }
}

/* ---------------- 协议帧处理（link 任务） ---------------- */
static void on_frame(uint8_t type, const uint8_t *pl, uint16_t len)
{
    switch (type) {
    case RESP_AUDIO: {
        if (len < 6u) break;
        uint32_t seq = proto_get_u32(pl);
        uint16_t n   = proto_get_u16(pl + 4u);
        if ((uint32_t)n + 6u > (uint32_t)len) n = (uint16_t)(len - 6u);

        if (s_need_reset) { s_stat_drop++; break; }   /* 复位期间丢帧 */

        if (s_seq_valid == 0) {
            /* 开机/复位后的第一帧：直接接受并建立期望值。此时不能判「seq 不连续」，
               否则每帧都会触发硬复位 -> 无限复位。 */
            s_end_seen = 0;
        } else if (seq != s_seq_expect) {
            if ((s_end_seen != 0) && (s_seq_valid != 0)) {
                /* 上一曲已整首发完 -> 这是无缝换曲边界：缓冲不丢，接着解 */
                s_end_seen = 0;
                s_split_pending = 1;
                s_split_at = s_rx_total;
                printf("[PLAY] seamless boundary at rx %lu\r\n", (unsigned long)s_rx_total);
            } else {
                uint32_t now = now_ms();
                uint8_t  ok = 0u;
                uint32_t base = 0u;
                s_end_seen = 0;
                if ((uint32_t)(now - s_track_change_at) > 1000u) {
                    /* 非切歌造成的 seq 断流 = 音频帧真丢了 */
                    s_stat_holes++;
                    if (s_hole_log < 10u) {
                        s_hole_log++;
                        printf("[PLAY] HOLE seq=%lu expect=%lu lost=%lu\r\n",
                               (unsigned long)seq, (unsigned long)s_seq_expect,
                               (unsigned long)((seq > s_seq_expect) ? (seq - s_seq_expect) : 0u));
                    }
                }
                if ((uint32_t)(now - s_track_change_at) <= 1000u) {
                    ok = 1u; base = 0u;                 /* 手动切歌：新曲从 0 开始 */
                } else if ((uint32_t)(now - s_last_time_at) <= 500u) {
                    ok = 1u; base = s_last_time_val;    /* seek：RESP_TIME 刚给过目标位置 */
                }
                s_base_req = base;
                s_base_req_ok = ok;
                s_req_out = rx_eff_bytes() / CREDIT_CHUNK;  /* 认定在途额度已归还 */
                printf("[PLAY] seq %lu (expect %lu) -> hard reset base=%lu%s\r\n",
                       (unsigned long)seq, (unsigned long)s_seq_expect,
                       (unsigned long)base, ok ? "" : " (unknown)");
                request_reset("seq");
                s_stat_drop++;
                break;
            }
        }
        {
            size_t w = s_in.write(pl + 6u, n);
            s_rx_total += (uint64_t)n;
            s_stat_audio++;
            s_stat_abytes += (uint32_t)n;   /* 收到的字节都算，保证在途额度对齐 */
            s_last_audio_ms = now_ms();
            s_seq_expect = seq + 1u;
            s_seq_valid = 1u;
            if (w < n) {
                /* 环满：只丢这一帧的尾部。字节流在中间断开，解码器找下一个同步头
                   自己就能接上（约 26ms 杂音），比 hard reset 丢 1 秒多音频+爆音好得多。 */
                s_stat_overrun++;
                s_stat_ovr_bytes += (uint32_t)(n - w);
                if (s_stat_overrun <= 10u)
                    printf("[PLAY] ring full, lost %u B (x%lu)\r\n",
                           (unsigned)(n - w), (unsigned long)s_stat_overrun);
            }
        }
        break;
    }

    case RESP_STATE:
        if (len >= 7u) {
            uint16_t tr = proto_get_u16(pl);
            uint8_t  st = pl[4];
            s_st_total = proto_get_u16(pl + 2u);
            s_st_mode  = pl[5];
            s_st_vol   = pl[6];
            if (tr != s_st_track) {
                printf("[PLAY] track -> %u/%u (playing=%u)\r\n",
                       (unsigned)(tr + 1u), (unsigned)s_st_total, st);
                s_st_track = tr;
                s_track_change_at = now_ms();
            }
            if ((st != 0u) && (s_playing == 0u))
            {
                printf("[PLAY] resume\r\n");
                /* STM32 暂停时会作废旧额度：这里把在途额度归零，立刻重新请求 */
                s_req_out = rx_eff_bytes() / CREDIT_CHUNK;
                s_last_audio_ms = 0;
            }
            if ((st == 0u) && (s_playing != 0u)) printf("[PLAY] pause\r\n");
            s_playing = (st != 0u) ? 1u : 0u;
        }
        break;

    case RESP_TIME:
        if (len >= 8u) {
            uint32_t ms = proto_get_u32(pl);
            uint32_t now = now_ms();
            s_last_time_val = ms;
            s_last_time_at = now;
            /* 复位刚发生：此刻 ESP32 还没收到新流，RESP_TIME 就是准确的绝对位置 */
            if ((s_base_armed != 0) && ((uint32_t)(now - s_base_arm_ms) < 300u)) {
                s_base_ms = ms;
                s_pos_track = s_st_track;
                s_base_ok = 1u;
                s_base_armed = 0;
                printf("[PLAY] base pos = %lu ms\r\n", (unsigned long)ms);
            }
        }
        break;

    case RESP_END:
        if (len >= 3u) {
            s_stat_end_track = proto_get_u16(pl);
            s_stat_end_reason = pl[2];
            s_stat_end++;
            s_end_seen = 1u;
            printf("[PLAY] end track %u reason %u\r\n",
                   (unsigned)(s_stat_end_track + 1u), (unsigned)s_stat_end_reason);
        }
        break;

    case RESP_PONG:
        if (len >= 6u) {
            s_stat_pong++;
            s_stat_drops32 = proto_get_u16(pl + 4u);
        }
        break;

    default:
        break;
    }
}

/* ---------------- 链路任务 ---------------- */
static void task_link(void *arg)
{
    (void)arg;
    uint32_t t_last_req = 0, t_last_ping = 0, t_last_sync = 0;
    uint32_t ping_tick = 0;

    for (;;) {
        uint8_t type = 0;
        const uint8_t *pl = 0;
        uint16_t len = 0;

        if (link_wait_frame(&type, &pl, &len, 5u)) {
            on_frame(type, pl, len);
        }

        uint32_t now = now_ms();

        /* 压缩环有空位就向 STM32 要数据（信用窗口）
         * 在途额度模型：只补"已请求但还没到"的差额，避免每 10ms 灌一帧
         * CMD_AUDIO_REQ 把 STM32 的 1KB RX 环冲爆（那会让 credit 永远是 0）。
         * s_req_out  = 已请求出去的块数（1 块 = 1KB）
         * s_stat_abytes = 已收到的音频字节
         * 二者之差就是"STM32 还欠我们多少"。 */
        if ((uint32_t)(now - t_last_req) >= 10u) {
            t_last_req = now;
            size_t   sp   = s_in.space();
            uint32_t sent = s_req_out * CREDIT_CHUNK;
            uint32_t recv = rx_eff_bytes();     /* 只算真进了环的字节 */
            uint32_t inflight = (sent > recv) ? (sent - recv) : 0u;

            uint32_t want = 0u;

            /* 看门狗：播放中 600ms 收不到音频 -> 额度可能丢了，先重新对齐在途再补一次 */
            if ((s_playing != 0u) && ((uint32_t)(now - s_last_audio_ms) > 600u)
                && (sp >= (96u * 1024u))) {   /* 环里必须真有空间，否则重对齐后可能冲爆环 */
                s_last_audio_ms = now;
                s_req_out = recv / CREDIT_CHUNK;      /* 重新对齐，避免在途额度无限漂移 */
                inflight  = 0u;
                s_req_wd++;
                want = (uint32_t)(sp / CREDIT_CHUNK);
                if (want > CREDIT_MAX_REQ) want = CREDIT_MAX_REQ;
            }
            /* 正常补额度：只在"环空余 - 在途额度"的余量内申请，保证不会冲爆环。
               并要求环里至少有 96KB 空位（>= 最大在途 48KB 的两倍），彻底杜绝环满丢字节。 */
            else if (sp >= (96u * 1024u)) {
                uint32_t room = (sp > inflight) ? ((uint32_t)sp - inflight) : 0u;
                uint32_t cap  = (CREDIT_MAX_INFLIGHT > inflight) ? (CREDIT_MAX_INFLIGHT - inflight) : 0u;
                uint32_t lim  = (room < cap) ? room : cap;
                want = lim / CREDIT_CHUNK;
                if (want > CREDIT_MAX_REQ) want = CREDIT_MAX_REQ;
                if (want < 2u) want = 0u;
            }
            if (want > 0u) {
                s_req_out += want;
                link_send_u16(CMD_AUDIO_REQ, (uint16_t)want);
            }
        }

        /* 周期 ping，确认链路与 STM32 丢帧统计 */
        if ((uint32_t)(now - t_last_ping) >= 2000u) {
            t_last_ping = now;
            link_send_u32(CMD_PING, ++ping_tick);
        }

        /* 上报真实播放位置（供 STM32 显示精确进度） */
        if ((uint32_t)(now - t_last_sync) >= 500u) {
            /* s_written_us 只统计已交给 I2S DMA 的音频时长，PCM 环里还没送出去的
             * 本来就不在其中，所以这里只加 seek 起点 s_base_ms，不再扣环内余量
             * （扣两遍会让上报位置落后于真实出声位置）。 */
            uint32_t pm = (uint32_t)(s_written_us / 1000ull);
            pm += s_base_ms;
            t_last_sync = now;
            if ((s_base_ok != 0u) && ((s_playing != 0u) || (s_seq_valid != 0u))) {
                uint8_t p[10];
                proto_put_u16(p, s_pos_track);
                proto_put_u32(p + 2u, pm);
                proto_put_u32(p + 6u, (uint32_t)(s_written_us / 1000ull));
                link_send(CMD_SYNC_POS, p, 10u);
            }
        }

        /* 状态灯 */
        if ((s_playing != 0u) && (s_stat_frames > 0)) led_set(0, 24, 0);
        else if (s_playing != 0u)                led_set(20, 20, 0);
        else                                     led_set(0, 0, 16);

    }
}

/* ---------------- 初始化 ---------------- */
void player_init(void)
{
    Serial.printf("[PLAY] psram=%u KB free=%u KB\r\n",
                  (unsigned)(ESP.getPsramSize() / 1024u), (unsigned)(ESP.getFreePsram() / 1024u));
    printf("[PLAY] i2s pins bclk=%d lrck=%d din=%d sd=%d led=%d\r\n",
           PIN_I2S_BCLK, PIN_I2S_LRCK, PIN_I2S_DIN, PIN_AMP_SD, PIN_LED);

    if (!s_in.init(IN_CAP))   printf("[PLAY] in ring FAIL\r\n");
    if (!s_pcm.init(PCM_CAP)) printf("[PLAY] pcm ring FAIL\r\n");
    printf("[PLAY] rings in=%u KB%s pcm=%u KB%s\r\n",
           (unsigned)(IN_CAP / 1024u), s_in.psram() ? "(psram)" : "(int)",
           (unsigned)(PCM_CAP / 1024u), s_pcm.psram() ? "(psram)" : "(int)");

#if (PIN_LED >= 0)
    pinMode(PIN_LED, OUTPUT);
    led_set(0, 0, 16);
#endif

    i2s_hw_init();
    boot_beep();
    link_init();

    xTaskCreatePinnedToCore(task_link,   "link",   4096, NULL, 12, NULL, 0);
    xTaskCreatePinnedToCore(task_i2s,    "i2s",    4096, NULL, 10, NULL, 1);
    xTaskCreatePinnedToCore(task_decode, "decode", 8192, NULL,  8, NULL, 1);
}
