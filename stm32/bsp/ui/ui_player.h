#ifndef __UI_PLAYER_H
#define __UI_PLAYER_H

/*
 * ui_player.h - 320x240 横版 MP3 播放器界面（主界面 / 歌单浮层 / 全屏歌词页）
 *
 * 主界面版面（横版 320x240）：
 *   y   0.. 25   状态栏：播放/暂停圆点 + 音量数字 + SD + 播放模式
 *   y  26..191   主区：侧边音量条(24 px，调音量时弹出 1.5 s) | 封面 144x144 |
 *                信息区(歌名 / 歌手 + 序号 / 歌词 4 行 / 进度条 / 时间)
 *   y 192..239   控制栏 48 px：[⏮][▶][⏭] 音量 | [歌词][模式][歌单]
 *   歌单浮层   y 26..239，状态栏之下铺满
 *   全屏歌词页 整屏，5 行视窗（当前行 1.5x 放大居中高亮）+ 顶部歌名 + 底部进度/时间/音量
 *
 * 依赖：playlist.h 的 pl_entry_t；实现与增量刷新策略见 ui_player.c。
 * 调用者：app/main.c（界面帧循环与按键/远程命令分发）。
 */
#include <stdint.h>
#include "playlist.h"

typedef struct
{
    uint8_t  playing;      /* 1 = 播放中 */
    uint8_t  mode;         /* 0 列表循环 1 单曲循环 2 随机 */
    uint8_t  volume;       /* 0..100 */
    uint8_t  sd_ok;        /* 1 = TF 卡可用（状态栏 SD 点亮绿） */
    uint8_t  link_ok;      /* 1 = 与 ESP32 的板间链路在线 */
    uint16_t index;        /* 当前曲目序号(1 起), 0 = 无 */
    uint16_t total;        /* 歌单总曲目数 */
    uint32_t pos_ms;       /* 当前播放位置(ms)，断点续播/进度条用 */
    uint32_t total_ms;     /* 曲目总时长(ms)，0 = 未知 */
    const char       *title;    /* UTF-8 歌名 */
    const char       *artist;   /* UTF-8 歌手（无则传 "" 或 NULL） */
    const pl_entry_t *list;
    uint16_t list_n;
} ui_player_state_t;

/* 复位增量刷新缓存；ui_player_full() 负责整屏铺底 */
void ui_player_init(void);
/* 整屏重画主界面（四块依次铺底） */
void ui_player_full(const ui_player_state_t *st);
/* 界面帧循环入口：按状态差量做局部刷新，now_ms 用于音量条 1.5 s 自动隐藏与切页淡变计时 */
void ui_player_update(const ui_player_state_t *st, uint32_t now_ms);

/* 调音量时调用：刷新音量显示并弹出左侧音量条（1.5 s 后自动隐藏） */
void ui_player_vol_flash(const ui_player_state_t *st);

/* 换曲后封面变了：只重画封面那一块（浮层/歌词页/切页淡变中跳过，退出时会顺带重画） */
void ui_player_cover_refresh(void);

/* 歌单浮层 */
void     ui_player_list_open(const ui_player_state_t *st);
void     ui_player_list_move(int8_t dir, const ui_player_state_t *st);  /* -1 上, +1 下（脏刷新） */
uint16_t ui_player_list_sel(void);                 /* 当前选中下标(0 起) */
void     ui_player_list_close(const ui_player_state_t *st);
uint8_t  ui_player_list_is_open(void);

/* 全屏歌词模式（控制栏「歌词」按钮 = 旋钮按下 PD12）
   进入：整屏居中滚动歌词，当前行 1.5 倍放大并高亮；
   退出：PD12 再按一次，回到主界面。 */
void    ui_player_lyrics_open(const ui_player_state_t *st);
void    ui_player_lyrics_close(const ui_player_state_t *st);
uint8_t ui_player_lyrics_is_open(void);

#endif /* __UI_PLAYER_H */
