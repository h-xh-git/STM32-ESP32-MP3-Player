#ifndef ESP_WIFIMGR_H
#define ESP_WIFIMGR_H

/*
 * wifimgr.h - ESP32-S3 多 WiFi 管理（NVS 持久化 + 网页配置表单）
 *
 * ============================ 它做四件事 ============================
 *   1) 两组数组保存多组账号： s_ssid[WIFIMGR_MAX][33] / s_pass[WIFIMGR_MAX][65]
 *   2) 网页表单（HTTP 80 端口）提交新的 WiFi 名称 + 密码
 *   3) 每次改动立刻写入 NVS（命名空间 "wifi"）—— 断电不丢，不是普通全局数组
 *   4) 开机先从 NVS 读回账号，STA 模式依次尝试连接；全不通就开配网热点
 *
 * ============================ 怎么用 ============================
 *   setup():  wifimgr_init();          // 读 NVS + 起网页 + 开始后台连接
 *   loop():   wifimgr_poll();          // 非阻塞状态机（每轮调一次）
 *   上传中:   wifimgr_set_busy(true/false);      // 传输期间不切 WiFi 模式
 *
 * ============================ 网页 ============================
 *   已连上路由器：  http://<STA 拿到的 IP>/          （串口会打印，另有 http://mp3player.local/）
 *   没连上路由器：  手机连热点 ESP32-MP3-Setup / mp3setup123，打开 http://192.168.4.1/
 *   页面功能：扫描附近网络、填 SSID + 密码保存、查看/删除已保存账号、查看上传状态
 *
 * 依赖：Arduino / WiFi / WebServer / DNSServer / Preferences(NVS) / ESPmDNS。
 * 调用者：src/main.cpp（wifimgr_init / wifimgr_poll）；netdl 借出 WebServer 挂
 *         /upload 上传页，并在传输期间调 wifimgr_set_busy。
 *
 * ============================ 给别的模块挂网页路由 ============================
 *   配置网页（80 端口）由本模块持有。别的模块想往同一个网页上加自己的页面
 *   （例如 netdl 的 /upload 上传页），用下面的 wifimgr_web() 拿句柄即可：
 *
 *       WebServer *web = wifimgr_web();
 *       if (web != NULL) { web->on("/mypage", HTTP_GET, my_handler); }
 *
 *   返回 NULL = 网页还没起来；在 begin() 前后调用都行（路由表是按请求查的）。
 */

#include <stdint.h>

class WebServer;   /* 只暴露指针，头文件不拖 WebServer.h */

/* 配置网页的 WebServer 实例（未初始化时返回 NULL） */
WebServer *wifimgr_web(void);

/* ---------------- 容量 ---------------- */
#define WIFIMGR_MAX        8u       /* 最多保存 8 组账号（数组第一维） */
#define WIFIMGR_SSID_LEN   33u      /* 802.11 SSID 上限 32 字符 + 结尾 0 */
#define WIFIMGR_PASS_LEN   65u      /* 密码上限 64 字符 + 结尾 0 */
#define WIFIMGR_NAME_LEN   40u      /* 存进 NVS 的主机名/mDNS 名长度 */

/* ---------------- 超时 ---------------- */
#define WIFIMGR_TRY_MS     12000u   /* 后台状态机：每个候选账号的等待时长 */
#define WIFIMGR_RETRY_MS   15000u   /* 全部失败后，隔多久再轮一遍 */
#define WIFIMGR_AP_HOLD_MS 20000u   /* 连上后热点再留多久（方便回头改配置） */

/* ---------------- 首次上电的兜底账号 ----------------
 * 只有 NVS 里一个账号都没有时才用它。留空 = 直接进配网热点模式。
 * 也可以在 platformio.ini 的 build_flags 里 -DWIFIMGR_DEFAULT_SSID="xx"。 */
#ifndef WIFIMGR_DEFAULT_SSID
  #ifdef NETDL_WIFI_SSID
    #define WIFIMGR_DEFAULT_SSID  NETDL_WIFI_SSID
  #else
    #define WIFIMGR_DEFAULT_SSID  ""
  #endif
#endif
#ifndef WIFIMGR_DEFAULT_PASS
  #ifdef NETDL_WIFI_PASS
    #define WIFIMGR_DEFAULT_PASS  NETDL_WIFI_PASS
  #else
    #define WIFIMGR_DEFAULT_PASS  ""
  #endif
#endif

/* ---------------- 配网热点（可被 NVS 里的值覆盖） ---------------- */
#ifndef WIFIMGR_AP_SSID
#define WIFIMGR_AP_SSID    "ESP32-MP3-Setup"
#endif
#ifndef WIFIMGR_AP_PASS
#define WIFIMGR_AP_PASS    "mp3setup123"     /* 至少 8 位，否则会开成无密码热点 */
#endif
#ifndef WIFIMGR_WEB_PORT
#define WIFIMGR_WEB_PORT   80
#endif
#ifndef WIFIMGR_HOSTNAME
#define WIFIMGR_HOSTNAME   "mp3player"
#endif

/* ---------------- 生命周期 ---------------- */
void wifimgr_init(void);                                    /* 读 NVS + 起网页 + 起连接 */
void wifimgr_poll(void);                                    /* 主循环调用，非阻塞 */
void wifimgr_set_busy(bool busy);                           /* true = 正在下载，别切模式 */

/* ---------------- 账号表（只读） ---------------- */
int         wifimgr_count(void);                            /* 已保存账号数 */
const char *wifimgr_ssid(int i);                            /* 第 i 组 SSID（无效返回 ""） */
const char *wifimgr_pass(int i);                            /* 第 i 组密码（无效返回 ""） */
int         wifimgr_active(void);                           /* 上次连上的下标，-1 = 无 */

/* ---------------- 增删改（都会写 NVS） ---------------- */
int  wifimgr_add(const char *ssid, const char *pass);       /* 返回下标；-1 = 失败（表满/空名） */
bool wifimgr_remove(int i);                                 /* 删除第 i 组 */
bool wifimgr_connect_index(int i);                          /* 立刻切到第 i 组并重连 */

/* ---------------- 状态 ---------------- */
bool        wifimgr_connected(void);
const char *wifimgr_ip(void);                               /* STA IP，未连接为 "0.0.0.0" */
int         wifimgr_rssi(void);
bool        wifimgr_ap_on(void);
const char *wifimgr_ap_ssid(void);
const char *wifimgr_ap_pass(void);
const char *wifimgr_ap_ip(void);                            /* 热点网关 IP，没开为 "" */
uint32_t    wifimgr_ok_count(void);                         /* 连接成功次数 */
uint32_t    wifimgr_fail_count(void);                       /* 轮完全部失败的次数 */

#endif /* ESP_WIFIMGR_H */
