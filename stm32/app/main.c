/*
 * app/main.c - 主程序：外设/服务初始化、按键与编码器处理、播放状态机、主循环调度
 *
 * 作用：上电依次初始化各外设与 app 服务（TFT/按键/蜂鸣器/编码器/SD/FATFS/歌单/
 *       断点续播/板间链路/SPI 从机），随后进主循环：扫描按键与编码器 -> 按播放
 *       模式切歌 -> 收 ESP32 帧 -> 刷新 UI -> 推进封面分片解码 -> 处理上传存卡 ->
 *       写断点续播检查点。上电默认暂停，按 K1 才开始播放。
 *
 * 硬件要点（引脚）：
 *   按键 P2：K1=播放/暂停  K2=上一曲  K3=下一曲  K4=歌单浮层开关  K5=切播放模式
 *            歌单浮层打开时：K2=上移 K3=下移 K1=选中并播放 K4/K5=关闭浮层
 *            歌词页中不叠加歌单浮层，K1/K2/K3 仍可播放/上一曲/下一曲
 *            K5 播放模式取值 0=列表循环 1=单曲循环 2=随机；上/下一曲到两端后不回绕
 *   旋转编码器：PC6/PC7（TIM8 正交解码）=音量加减（每格 +-5），旋钮按下 PD12=歌词页开关
 *   背光 PB10（TIM2_CH3 10 kHz PWM，开屏用 60 ms 淡入）；蜂鸣器 PB8（低电平触发）
 *   调试串口 USART1 PA9/PA10 115200；板间链路 USART2 PA2/PA3 1 Mbps
 *   存卡链路 SPI2 从机 PB13/PB14/PB15 + CS PB12 + READY PB11（ESP32-S3 做主机）
 *
 * 协议要点：USART2 帧 [0xAA][0x55][type][len LE][payload][crc16]；SPI2 每帧固定 2064 B。
 * 依赖：board、bsp_*（tick/uart/lcd/key/beep/encoder/sdio/uart2/proto/spid）、
 *       app/playlist、app/mp3info、app/audio_srv、app/play_resume、app/netdl、
 *       app/lyrics、app/cover、bsp/ui/ui_player。
 * 调用关系：startup_stm32f40xx.s 复位后进 main()；本文件对外只提供 main()，
 *           各模块通过回调反向通知（见 on_remote_cmd）。
 */
#include "board.h"
#include "bsp_tick.h"
#include "bsp_uart.h"
#include "bsp_lcd.h"
#include "bsp_key.h"
#include "bsp_beep.h"
#include "bsp_encoder.h"
#include "bsp_sdio.h"
#include "bsp_uart2.h"
#include "bsp_proto.h"
#include "playlist.h"
#include "mp3info.h"
#include "audio_srv.h"
#include "play_resume.h"      /* 断点续播：当前曲目 + 文件内偏移存片内 Flash */
#include "bsp_spid.h"         /* 板间下载链路：SPI2 从机（ESP32-S3 主机推数据） */
#include "netdl.h"            /* 下载存卡服务：解析帧 + FATFS 写 TF 卡 */
#include "ui_player.h"
#include "lyrics.h"
#include "cover.h"
#include "ff.h"

#include <stdio.h>
#include <string.h>

static FATFS s_fs;
static char  s_artist[24];          /* 信息区显示（文件大小） */

static uint8_t  s_rxbuf[256];       /* 收到的帧（命令，都很小） */
static uint8_t  s_rem_cmd = 0u;     /* ESP32 远程控制命令（暂存到主循环处理） */
static uint32_t s_rem_arg = 0u;
static uint32_t s_rnd     = 0x12345678u;

/* 文件大小 -> "512KB" / "5.7MB" */
static void fmt_size(uint32_t size, char *out)
{
    if (size < (1024u * 1024u))
    {
        sprintf(out, "%luKB", (unsigned long)((size + 512u) / 1024u));
    }
    else
    {
        uint32_t kb = size / 1024u;
        sprintf(out, "%lu.%luMB",
                (unsigned long)(kb / 1024u),
                (unsigned long)((kb % 1024u) * 10u / 1024u));
    }
}

/* 随机下一首（简易 LCG，足够"随机播放"用） */
static uint16_t rnd_track(uint16_t total)
{
    if (total == 0u) return 0u;
    s_rnd = (s_rnd * 1103515245u) + 12345u;
    return (uint16_t)(((s_rnd >> 16) & 0x7FFFu) % (uint32_t)total);
}

/* audio_srv 收到 ESP32 控制命令时回调（只暂存，回到主循环再执行） */
static void on_remote_cmd(uint8_t cmd, uint32_t arg)
{
    s_rem_cmd = cmd;
    s_rem_arg = arg;
}

/* 播放/暂停切换（K1） */
static void toggle_play(ui_player_state_t *st)
{
    st->playing = (uint8_t)((st->playing != 0u) ? 0u : 1u);
    audio_srv_set_playing(st->playing);

    /* 断点续播：按下暂停的这一瞬间就把进度落盘（用户最可能在这里关机） */
    if (st->playing == 0u) (void)play_resume_save(audio_srv_pos_ms(), 1u);
}

/* 播放模式切换 0=列表循环 1=单曲循环 2=随机（K5） */
static void toggle_mode(ui_player_state_t *st)
{
    st->mode = (uint8_t)((st->mode + 1u) % 3u);
}

/* 切到第 idx0 首：先开音频出声 -> 登记续播 -> 载歌词 -> 起封面，并填好界面字段 */
static void set_track(ui_player_state_t *st, uint16_t idx0)
{
    const pl_entry_t *e = pl_get(idx0);
    char     path[96];
    uint32_t dur;

    /* 断点续播：切歌前先把上一首的进度落盘。
       必须在 audio_srv_open() 之前调用，此时 audio_srv_pos_ms() 还是上一首的位置；
       首次上电时 play_resume 内部没有登记曲目，会直接跳过。 */
    (void)play_resume_save(audio_srv_pos_ms(), 1u);

    if (e == 0)
    {
        st->index = 0u; st->title = "--"; st->artist = "";
        st->total_ms = 0u; st->pos_ms = 0u;
        lrc_clear();
        audio_srv_close();
        play_resume_open_track();
        return;
    }
    st->index  = (uint16_t)(idx0 + 1u);
    st->title  = e->name;
    st->pos_ms = 0u;

    /* 真实时长：解析 MP3 帧头/Xing 帧数；解析失败才退回 128 kbps 估算 */
    pl_path(idx0, path, (uint32_t)sizeof(path));
    dur = mp3_duration_ms(path);
    if (dur == 0u) dur = (uint32_t)(e->size / 16u);
    st->total_ms = dur;

    fmt_size(e->size, s_artist);
    st->artist = s_artist;

    /* 先开音频：切歌第一目标是尽快出声，不能被 lrc_load() 和封面解码挡在后面
       （封面是分片解码，最长 0.5 s，不能同步跑在 audio_srv_open() 前面）。 */
    (void)audio_srv_open(idx0, dur);
    audio_srv_set_playing(st->playing);

    /* 断点续播：登记新曲目（此后周期检查点写的就是这一首） */
    play_resume_set_track(idx0);

    /* 载入同名 .lrc（没有就清空，界面回到占位文案） */
    lrc_clear();
    lrc_load(path);

    /* 真实封面：先找 MP3 内嵌 ID3v2 APIC，再回退同目录 cover.jpg / <同名>.jpg。
       cover_begin() 只定位源 + 解析到 SOS（毫秒级），MCU 扫描交给主循环 cover_poll(2)
       分片推进：这里先把封面区刷成占位图，解完后主循环再刷新一次（见 cover_poll 处）。 */
    if (cover_begin(path) != 0u)
    {
        ui_player_cover_refresh();          /* 解码中：显示占位图，不挡出声 */
    }
    else
    {
        ui_player_cover_refresh();          /* 无封面：直接显示占位图 */
    }

    printf("[MP3] track %u/%u  %lu B  %lu:%02lu  %lu kbps  %lu Hz%s\r\n",
           (unsigned)(idx0 + 1u), (unsigned)pl_count(), (unsigned long)e->size,
           (unsigned long)(dur / 60000u), (unsigned long)((dur / 1000u) % 60u),
           (unsigned long)mp3_last_bitrate(), (unsigned long)mp3_last_samplerate(),
           (mp3_last_vbr() != 0u) ? " VBR" : "");
}

/* 固件入口：初始化外设与各服务，然后跑主循环 */
int main(void)
{
    FRESULT  fr;
    uint16_t n = 0u;
    uint16_t cur = 0u;
    uint32_t ms      = 0u;
    uint8_t  rsm_ok    = 0u;        /* 断点续播：是否读回一条有效记录 */
    uint16_t rsm_track = 0u;
    uint32_t rsm_pos   = 0u;
    uint8_t  dl_rescan = 0u;        /* 下载完成 -> 等不播放时重扫歌单 */
    uint32_t dl_ui_ms  = 0u;
    uint16_t dl_ui_pct = 0xFFFFu;
    char     s_dl_info[24];
    ui_player_state_t st;

    board_init();
    tick_init();
    uart1_init(115200u);

    printf("\r\n");
    printf("========================================\r\n");
    printf("[MP3] STM32F407 boot  SYSCLK=%lu Hz\r\n", (unsigned long)SystemCoreClock);
    printf("========================================\r\n");

    lcd_init();                       /* 面板初始化，背光保持关闭、GRAM 已填黑 */
    lcd_set_backlight(0u);            /* 显式关背光：全部初始化 + 首帧画完后再开 */
    key_init();
    beep_init();                      /* PB8 有源蜂鸣器（按键提示音，低电平触发） */
    enc_init();                       /* 旋转编码器：PC6/PC7 TIM8 正交解码 + PD12 按下 */
    printf("[MP3] encoder: TIM8 PC6/PC7 + SW PD12\r\n");

    uart2_init(1000000u);
    proto_init();
    audio_srv_init();
    audio_srv_set_cmd_cb(on_remote_cmd);
    printf("[MP3] link init: USART2 1 Mbps\r\n");

    /* 网页上传存卡链路：SPI2 从机 + 上传写卡服务（与上面 UART 链路互不干扰） */
    spid_init();
    netdl_init();

    memset(&st, 0, sizeof(st));
    st.playing = 0u;
    st.mode    = 0u;
    st.volume  = 70u;
    st.title   = "\346\255\243\345\234\250\345\210\235\345\247\213\345\214\226...";
    st.artist  = "";
    st.list    = (const pl_entry_t *)0;
    st.list_n  = 0u;
    ui_player_init();
    cover_init();
    ui_player_full(&st);

    if (sd_init() != SD_OK)
    {
        st.title = "SD \345\215\241\345\210\235\345\247\213\345\214\226\345\244\261\350\264\245";
        ui_player_full(&st);
        printf("[MP3] !! sd_init FAILED\r\n");
        netdl_set_fs_ready(0u);
    }
    else
    {
        fr = f_mount(&s_fs, "0:", 1);
        printf("[MP3] f_mount(\"0:\") -> %d\r\n", (int)fr);

        if (fr == FR_OK)
        {
            netdl_set_fs_ready(1u);      /* 下载服务可以写卡了 */
            n = pl_scan();
            printf("[MP3] playlist: %u song(s)\r\n", (unsigned)n);

            st.sd_ok  = 1u;
            st.total  = n;
            st.list   = pl_get(0u);
            st.list_n = n;
            /* ---- 断点续播：读回上次的曲目序号 + 该曲已播毫秒 ---- */
            (void)play_resume_init();
            rsm_ok = play_resume_find_saved(&rsm_track, &rsm_pos);

            if ((n != 0u) && (rsm_ok != 0u) && (rsm_track < n))
            {
                cur = rsm_track;
                set_track(&st, cur);
                if ((rsm_pos > 0u) && (st.total_ms != 0u))
                {
                    if (rsm_pos > st.total_ms) rsm_pos = st.total_ms;
                    audio_srv_seek_ms(rsm_pos);   /* ms -> 文件字节 -> 对齐 MP3 帧头 */
                    st.pos_ms = rsm_pos;
                    printf("[RSM] resume track %u/%u at %lu ms\r\n",
                           (unsigned)(cur + 1u), (unsigned)n, (unsigned long)rsm_pos);
                }
                else
                {
                    printf("[RSM] resume track %u/%u from start\r\n",
                           (unsigned)(cur + 1u), (unsigned)n);
                }
            }
            else
            {
                if (rsm_ok != 0u)
                    printf("[RSM] saved track not in playlist -> start from #1\r\n");
                set_track(&st, 0u);
                if (n == 0u) play_resume_clear();   /* 歌单空：写墓碑，下次不恢复 */
            }
            if (n == 0u) { st.title = "\346\234\252\346\211\276\345\210\260 MP3 \346\226\207\344\273\266"; st.artist = ""; }
        }
        else
        {
            st.title  = "\346\226\207\344\273\266\347\263\273\347\273\237\346\214\202\350\275\275\345\244\261\350\264\245";
            st.artist = "";
            printf("[MP3] !! f_mount failed (%d)\r\n", (int)fr);
            netdl_set_fs_ready(0u);
        }
        ui_player_full(&st);
    }

    st.playing = 0u;   /* 上电默认暂停，按 K1 才开始播放 */
    audio_srv_set_playing(0u);
    ui_player_full(&st);
    printf("[MP3] K1=play/pause K2=prev K3=next K4=list K5=mode ENC=turn vol / push PD12=lyrics\r\n");

    /* ===== 全部初始化完成、首帧 UI 已画好：现在才点背光 =====
       这样上电过程中屏幕一直是黑的，不会闪一下或露出花屏。
       背光是 TIM2_CH3(PB10) 10 kHz PWM：这里用 60 ms 阻塞淡入，
       上电时主循环还没开始，阻塞几十毫秒没有副作用。 */
    lcd_backlight_fade(LCD_BL_FULL, 60u);
    printf("[MP3] init complete -> backlight ON (60 ms PWM fade-in)\r\n");

    for (;;)
    {
        uint8_t k;

        ms = tick_ms();
        k = key_scan();
        if (k != KEY_NONE) beep_key();          /* 按键提示音 */

        /* ---- 旋转编码器：旋转 = 音量 +-5，按下(PD12) = 歌词页开关 ---- */
        {
            int8_t es = enc_step();
            if (es != 0)
            {
                int16_t v = (int16_t)st.volume + ((int16_t)es * 5);
                beep_key();                     /* 音量每变化一格提示一声 */
                if (v < 0)   v = 0;
                if (v > 100) v = 100;
                st.volume = (uint8_t)v;
            }
        }
        {
            uint8_t sw = enc_sw_scan();     /* 始终扫描, 避免模态期间攒下"陈旧的按下" */
            if ((sw != 0u) && (!ui_player_list_is_open()))
            {
                beep_ms(BEEP_MS_ACT);           /* 进/出歌词页，提示音略长一点 */
                /* 旋钮按下(PD12)：主界面 = 进入全屏歌词；歌词模式内 = 返回主界面 */
                if (ui_player_lyrics_is_open()) ui_player_lyrics_close(&st);
                else                            ui_player_lyrics_open(&st);
            }
        }

        if (ui_player_list_is_open())
        {
            /* ---- 歌单模态 ---- */
            if (k == KEY_2)      { ui_player_list_move(-1, &st); }   /* 脏刷新：只重画受影响的行 */
            else if (k == KEY_3) { ui_player_list_move(1,  &st); }
            else if (k == KEY_1)
            {
                beep_ms(BEEP_MS_ACT);
                cur = ui_player_list_sel();
                set_track(&st, cur);
                ui_player_list_close(&st);
            }
            else if ((k == KEY_4) || (k == KEY_5)) { beep_ms(BEEP_MS_ACT); ui_player_list_close(&st); }   /* K4: 歌单键再按一次关闭 */
        }
        else
        {
            /* ---- 正常控制 ---- */
            if ((k == KEY_2) && (st.total != 0u))
            {
                if (cur > 0u) { cur--; set_track(&st, cur); }                       /* 到第一首再按不再回绕到最后 */
            }
            else if ((k == KEY_3) && (st.total != 0u))
            {
                if ((uint16_t)(cur + 1u) < st.total) { cur++; set_track(&st, cur); } /* 到最后一首再按不再回绕到第一 */
            }
            else if (k == KEY_1) { toggle_play(&st); }           /* K1 = 切换播放/暂停 */
            else if (k == KEY_5) { toggle_mode(&st); }           /* K5 = 切播放模式 */
            else if ((k == KEY_4) && (!ui_player_lyrics_is_open())) { beep_ms(BEEP_MS_ACT); ui_player_list_open(&st); }  /* K4 = 歌单（歌词模式内不叠加） */
        }

        /* ---- ESP32 远程控制（预留：串口助手也能用来验证） ---- */
        if (s_rem_cmd != 0u)
        {
            uint8_t  c = s_rem_cmd;
            uint32_t a = s_rem_arg;
            s_rem_cmd = 0u;

            switch (c)
            {
            case CMD_PLAY:
                if ((st.total != 0u) && (a < (uint32_t)st.total))
                {
                    cur = (uint16_t)a; st.playing = 1u; set_track(&st, cur);
                }
                break;
            case CMD_PAUSE:  st.playing = 0u; (void)play_resume_save(audio_srv_pos_ms(), 1u); audio_srv_set_playing(0u); break;
            case CMD_RESUME: st.playing = 1u; audio_srv_set_playing(1u); break;
            case CMD_STOP:   st.playing = 0u; audio_srv_set_playing(0u); audio_srv_seek_ms(0u); (void)play_resume_save(0u, 1u); break;
            case CMD_NEXT:
                if ((st.total != 0u) && ((uint16_t)(cur + 1u) < st.total)) { cur++; st.playing = 1u; set_track(&st, cur); }
                break;
            case CMD_PREV:
                if ((st.total != 0u) && (cur > 0u)) { cur--; st.playing = 1u; set_track(&st, cur); }
                break;
            case CMD_VOL:  st.volume = (uint8_t)((a > 100u) ? 100u : a); break;
            case CMD_MODE: st.mode   = (uint8_t)((a % 3u)); break;
            case CMD_SEEK: audio_srv_seek_ms(a); break;
            default: break;
            }
        }

        /* ---- 一首播完：按播放模式自动切下一首 ---- */
        {
            uint16_t fin;
            if (audio_srv_take_finished(&fin) != 0u)
            {
                if (st.total != 0u)
                {
                    uint16_t nxt;
                    if (st.mode == 1u)      nxt = fin;                              /* 单曲循环 */
                    else if (st.mode == 2u) nxt = rnd_track(st.total);               /* 随机 */
                    else                    nxt = (uint16_t)((fin + 1u) % st.total); /* 列表循环 */
                    cur = nxt;
                    st.playing = 1u;
                    set_track(&st, cur);
                }
                else
                {
                    st.playing = 0u;
                    audio_srv_set_playing(0u);
                }
            }
        }

        /* ---- 播放位置：取实际送出的音频数据折算 ---- */
        if (st.total_ms != 0u)
        {
            uint32_t pm = audio_srv_pos_ms();
            st.pos_ms = (pm > st.total_ms) ? st.total_ms : pm;
        }

        ui_player_update(&st, ms);

        /* ---- 封面分片解码：每轮最多 2 ms，解完刷新一次封面区 ---- */
        if (cover_busy())
        {
            uint8_t cr = cover_poll(2u);
            if (cr == 1u)
            {
                ui_player_cover_refresh();      /* 解码完成 */
            }
            else if (cr == 2u)
            {
                ui_player_cover_refresh();      /* 换曲后源失效/无封面 */
            }
        }

        /* ---- 收帧 ----
         * 一次主循环把 RX 环里的帧尽量取空（最多 16 帧，别把 UI 卡住）。
         * 每轮必须尽量取空：一旦 UI 刷新把主循环拖慢，RX 环（1KB）会被 ESP32
         * 的请求帧冲爆 -> USART2 静默丢字节 -> 请求帧 CRC 全错被丢弃 ->
         * credit 永远是 0 -> 一帧音频都发不出去。 */
        {
            uint8_t  ptype;
            uint16_t plen;
            uint32_t guard = 0u;
            while (proto_recv(&ptype, s_rxbuf, (uint16_t)sizeof(s_rxbuf), &plen) != 0u)
            {
                audio_srv_on_frame(ptype, s_rxbuf, plen);
                guard++;
                if (guard >= 16u) break;
            }
        }

        audio_srv_notify_state(cur, st.total, st.playing, st.mode, st.volume);
        audio_srv_poll();

        /* ---- 网页上传存卡：SPI2 从机收帧 -> FATFS 写 TF 卡 ---- */
        netdl_poll();
        {
            const char *dn  = (const char *)0;
            uint32_t    dsz = 0u;
            uint8_t     dev = netdl_take_done(&dn, &dsz);

            if (dev == 1u)
            {
                printf("[NETDL] saved %s  %lu B -> playlist rescan pending\r\n",
                       (dn != 0) ? dn : "?", (unsigned long)dsz);
                dl_rescan = 1u;
            }
            else if (dev == 2u)
            {
                printf("[NETDL] aborted/failed: %s\r\n", netdl_msg());
            }
        }

        /* 上传/下载中的进度：不在播放时借用标题/信息区显示（每 2 s 检查一次） */
        if (netdl_busy() != 0u)
        {
            uint16_t pct = netdl_percent();
            if ((uint32_t)(ms - dl_ui_ms) >= 2000u)
            {
                dl_ui_ms = ms;
                if ((st.playing == 0u) && (pct != dl_ui_pct))
                {
                    const char *keep_t = st.title;
                    const char *keep_a = st.artist;
                    dl_ui_pct = pct;
                    (void)snprintf(s_dl_info, sizeof(s_dl_info), "DL %u%% %luKB",
                                   (unsigned)pct, (unsigned long)(netdl_written() / 1024u));
                    st.title  = netdl_name();
                    st.artist = s_dl_info;
                    ui_player_full(&st);
                    st.title  = keep_t;
                    st.artist = keep_a;
                }
            }
        }

        /* 下载完的新歌：等不播放时再重扫歌单，避免打断正在播的曲目 */
        if ((dl_rescan != 0u) && (st.playing == 0u))
        {
            dl_rescan = 0u;
            n = pl_scan();
            st.total  = n;
            st.list   = pl_get(0u);
            st.list_n = n;
            if ((n != 0u) && (cur >= n)) cur = (uint16_t)(n - 1u);
            printf("[NETDL] playlist rescanned: %u song(s)\r\n", (unsigned)n);
        }

        /* ---- 断点续播：周期检查点（默认 1 s，位置真的前进后才写） ---- */
        (void)play_resume_tick(ms);
        /* 日志区写满时在暂停/空闲期间提前滚动，避免播放中阻塞 */
        play_resume_prime(ms);

        beep_poll();                    /* 提示音到点自动关（非阻塞） */

        delay_ms(1u);
    }
}
