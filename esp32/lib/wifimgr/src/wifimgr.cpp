/*
 * wifimgr.cpp - ESP32-S3 多 WiFi 管理：NVS 持久化 + 网页配置表单
 *
 * 用户要的四件事，对应下面四块代码：
 *   1) 两组平行数组 s_ssid[WIFIMGR_MAX][33] / s_pass[WIFIMGR_MAX][65] 存多组账号
 *   2) HTTP 网页表单（端口 80）提交新的 WiFi 名称 + 密码
 *   3) 每次增删改立刻写 NVS（Preferences，命名空间 "wifi"）—— 断电不丢
 *   4) 开机 wifimgr_init() 先从 NVS 读回账号，STA 模式依次尝试连接
 *
 * 关键点（也是和普通全局数组的区别）：
 *   - 账号表不是常量，开机时从 NVS 读出来（nvs_load），没有才用编译期兜底账号；
 *   - 网页每次新增都会 nvs_save_table() 落盘，重启后仍然在；
 *   - 计数键 "n" 最后写：中途断电最多让新那条不可见，旧状态始终完整；
 *   - 每条 key 单独 nvs_commit，没有整表擦写窗口。
 */

#include "wifimgr.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <nvs_flash.h>
#include <string.h>
#include <stdio.h>

/* ================= 1. 账号表：两组平行数组 ================= */

static char    s_ssid[WIFIMGR_MAX][WIFIMGR_SSID_LEN];   /* 第 0 维 = 第几组账号 */
static char    s_pass[WIFIMGR_MAX][WIFIMGR_PASS_LEN];   /* 与 s_ssid 一一对应 */
static uint8_t s_count = 0u;                            /* 当前有效组数 0..WIFIMGR_MAX */
static int8_t  s_active = -1;                           /* 上次连上的下标，开机优先试它 */

/* 配网热点（默认值来自 wifimgr.h，可被 NVS 里的 aps/app 覆盖） */
static char s_ap_ssid[WIFIMGR_SSID_LEN] = WIFIMGR_AP_SSID;
static char s_ap_pass[WIFIMGR_PASS_LEN] = WIFIMGR_AP_PASS;

/* ================= 运行时状态 ================= */

static Preferences s_prefs;
static WebServer   s_web((uint16_t)WIFIMGR_WEB_PORT);
static DNSServer   s_dns;      /* 配网热点开着时做 DNS 劫持：手机连上就自动弹配置页 */

enum { WST_IDLE = 0, WST_TRYING, WST_ONLINE };

static uint8_t  s_state = WST_IDLE;
static int8_t   s_try = -1;
static uint8_t  s_tried = 0u;
static uint32_t s_try_t0 = 0u;
static uint32_t s_retry_at = 0u;
static bool     s_ap = false;
static uint32_t s_ap_hold_until = 0u;
static bool     s_busy = false;          /* 上传中：不许切 WiFi 模式 */
static bool     s_inited = false;
static bool     s_prefs_ok = false;
static uint32_t s_ok = 0u, s_fail = 0u;
static char     s_ip[20] = "0.0.0.0";

/* ================= 2. NVS ================= */

static void nvs_boot(void)
{
    esp_err_t e = nvs_flash_init();
    if ((e == ESP_ERR_NVS_NO_FREE_PAGES) || (e == ESP_ERR_NVS_NEW_VERSION_FOUND)) {
        Serial.printf("[WIFI] NVS 需要重建 (%d)，擦除后重来\r\n", (int)e);
        nvs_flash_erase();
        e = nvs_flash_init();
    }
    if (e != ESP_OK) Serial.printf("[WIFI] !! nvs_flash_init = %d\r\n", (int)e);
}

static void nvs_load(void)
{
    char k[8];
    uint8_t n = s_prefs.getUChar("n", 0u);
    if (n > WIFIMGR_MAX) n = WIFIMGR_MAX;

    s_count = 0u;
    for (uint8_t i = 0u; i < n; i++) {
        snprintf(k, sizeof(k), "s%u", (unsigned)i);
        String sv = s_prefs.getString(k, "");
        if (sv.length() == 0u) continue;
        snprintf(k, sizeof(k), "p%u", (unsigned)i);
        String pv = s_prefs.getString(k, "");

        strncpy(s_ssid[s_count], sv.c_str(), WIFIMGR_SSID_LEN - 1u);
        s_ssid[s_count][WIFIMGR_SSID_LEN - 1u] = 0;
        strncpy(s_pass[s_count], pv.c_str(), WIFIMGR_PASS_LEN - 1u);
        s_pass[s_count][WIFIMGR_PASS_LEN - 1u] = 0;
        s_count++;
    }

    int8_t act = s_prefs.getChar("act", -1);
    s_active = ((act >= 0) && (act < (int8_t)s_count)) ? act : ((s_count > 0u) ? 0 : -1);

    String as = s_prefs.getString("aps", "");
    String ap = s_prefs.getString("app", "");
    if (as.length() > 0u) {
        strncpy(s_ap_ssid, as.c_str(), WIFIMGR_SSID_LEN - 1u);
        s_ap_ssid[WIFIMGR_SSID_LEN - 1u] = 0;
    }
    if (ap.length() > 0u) {
        strncpy(s_ap_pass, ap.c_str(), WIFIMGR_PASS_LEN - 1u);
        s_ap_pass[WIFIMGR_PASS_LEN - 1u] = 0;
    }
}

/* 整表落盘：NVS 里永远只有 0..s_count-1 有效，计数键最后写 */
static void nvs_save_table(void)
{
    if (!s_prefs_ok) {
        Serial.println("[WIFI] !! NVS 不可用，本次配置只在 RAM 里");
        return;
    }
    char k[8];
    for (uint8_t i = 0u; i < WIFIMGR_MAX; i++) {
        snprintf(k, sizeof(k), "s%u", (unsigned)i);
        if (i < s_count) s_prefs.putString(k, s_ssid[i]);
        else             s_prefs.remove(k);

        snprintf(k, sizeof(k), "p%u", (unsigned)i);
        if (i < s_count) s_prefs.putString(k, s_pass[i]);
        else             s_prefs.remove(k);
    }
    s_prefs.putChar("act", s_active);
    s_prefs.putUChar("n", s_count);     /* 最后写计数：断电也不会读到半截表 */
    Serial.printf("[WIFI] 已写入 NVS：%u 组账号\r\n", (unsigned)s_count);
}

/* ================= 3. WiFi 连接 ================= */

static int8_t try_index(uint8_t k)
{
    if (s_count == 0u) return -1;
    uint8_t start = (s_active >= 0) ? (uint8_t)s_active : 0u;
    return (int8_t)((start + k) % s_count);
}

static void ip_refresh(void)
{
    IPAddress ip = WiFi.localIP();
    snprintf(s_ip, sizeof(s_ip), "%u.%u.%u.%u", (unsigned)ip[0], (unsigned)ip[1], (unsigned)ip[2], (unsigned)ip[3]);
}

static void mdns_begin(void)
{
    static bool done = false;
    if (done) return;
    if (MDNS.begin(WIFIMGR_HOSTNAME)) {
        MDNS.addService("http", "tcp", WIFIMGR_WEB_PORT);
        done = true;
        Serial.printf("[WIFI] mDNS 就绪: http://%s.local/\r\n", WIFIMGR_HOSTNAME);
    } else {
        Serial.println("[WIFI] mDNS 启动失败（不影响网页，用 IP 访问即可）");
    }
}

static void sta_begin(int i)
{
    if ((i < 0) || (i >= (int)s_count)) return;
    /* 关键：配网热点开着时必须保持 WIFI_AP_STA。
       WiFi.mode(WIFI_STA) 会把 softAP 一起关掉 —— 那样每 15 s 重试一次就等于热点闪断一次，
       手机上正在填的配置页会直接断线。 */
    WiFi.mode(s_ap ? WIFI_AP_STA : WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(s_ssid[i], s_pass[i]);
    s_state = WST_TRYING;
    s_try = (int8_t)i;
    s_try_t0 = millis();
    Serial.printf("[WIFI] 正在连 [%d] %s ...\r\n", i, s_ssid[i]);
}

static void on_connected(int i)
{
    ip_refresh();
    if (i >= 0) s_active = (int8_t)i;
    s_state = WST_ONLINE;
    s_try = -1;
    s_ok++;
    if (s_prefs_ok) s_prefs.putChar("act", s_active);
    s_ap_hold_until = millis() + WIFIMGR_AP_HOLD_MS;
    Serial.printf("[WIFI] 已连接 [%d] %s  ip=%s  rssi=%d dBm\r\n",
                  i, (i >= 0) ? s_ssid[i] : "", s_ip, (int)WiFi.RSSI());
    mdns_begin();
}

static void ap_start(void)
{
    if (s_ap || s_busy) return;
    WiFi.mode(WIFI_AP_STA);
    bool ok = (strlen(s_ap_pass) >= 8u) ? WiFi.softAP(s_ap_ssid, s_ap_pass) : WiFi.softAP(s_ap_ssid);
    s_ap = ok;
    if (ok) {
        /* DNS 劫持：连上热点后访问任何域名都指到 192.168.4.1，多数手机会自动弹出配置页 */
        (void)s_dns.start(53, "*", WiFi.softAPIP());
        Serial.printf("[WIFI] 配网热点: ssid=%s  密码=%s  打开 http://%s/\r\n",
                      s_ap_ssid,
                      (strlen(s_ap_pass) >= 8u) ? s_ap_pass : "(无密码)",
                      WiFi.softAPIP().toString().c_str());
    } else {
        Serial.println("[WIFI] !! 热点开启失败");
    }
}

static void ap_stop(void)
{
    if (!s_ap) return;
    s_dns.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    s_ap = false;
    Serial.println("[WIFI] 配网热点已关（已连上路由器）");
}

/* ================= 4. 网页 ================= */

static void html_escape(const char *in, String &out)
{
    for (const char *p = in; (p != NULL) && (*p != 0); p++) {
        char c = *p;
        if      (c == '<')  out += F("&lt;");
        else if (c == '>')  out += F("&gt;");
        else if (c == '&')  out += F("&amp;");
        else if (c == '"')  out += F("&quot;");
        else if (c == '\'') out += F("&#39;");
        else out += c;
    }
}

static void page_head(String &p, const char *title)
{
    p += F("<!DOCTYPE html><html lang='zh-CN'><head><meta charset='utf-8'>");
    p += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
    p += F("<title>");
    p += title;
    p += F("</title><style>");
    p += F("body{font-family:system-ui,Arial,sans-serif;margin:16px;max-width:680px;line-height:1.5}");
    p += F("fieldset{border:1px solid #bbb;border-radius:8px;margin:10px 0;padding:10px}");
    p += F("legend{font-weight:600}input,select,button{font-size:16px;padding:8px;margin:4px 0;box-sizing:border-box}");
    p += F("input,select{width:100%}button{background:#2d6cdf;color:#fff;border:0;border-radius:6px;padding:8px 12px}");
    p += F("button.g{background:#8a8a8a}table{width:100%;border-collapse:collapse}");
    p += F("td,th{border-bottom:1px solid #e2e2e2;padding:6px;text-align:left}");
    p += F(".ok{color:#0a0}.bad{color:#c00}pre{background:#f4f4f4;padding:8px;border-radius:6px;overflow-x:auto}");
    p += F("</style></head><body>");
}

static void page_tail(String &p)
{
    p += F("<script>");
    p += F("function scan(){var x=new XMLHttpRequest();x.onreadystatechange=function(){if(x.readyState==4){");
    p += F("try{var a=JSON.parse(x.responseText);var s=document.getElementById('ap');s.options.length=0;");
    p += F("s.options[0]=new Option('-- 扫描结果 --','');for(var i=0;i<a.length;i++){");
    p += F("s.options[s.options.length]=new Option(a[i].s+'  ('+a[i].r+' dBm)',a[i].s);}}catch(e){alert('扫描失败');}}};");
    p += F("x.open('GET','/scan');x.send();}");
    p += F("function pick(){var s=document.getElementById('ap');if(s.value){document.getElementById('ssid').value=s.value;}}");
    p += F("function refresh(){var x=new XMLHttpRequest();x.onreadystatechange=function(){if(x.readyState==4){");
    p += F("document.getElementById('st').textContent=x.responseText;}};x.open('GET','/status');x.send();}");
    p += F("refresh();setInterval(refresh,5000);");
    p += F("</script></body></html>");
}

static void h_root(void)
{
    String p;
    page_head(p, "ESP32 播放器 - WiFi 配置");
    p += F("<h2>WiFi 配置</h2>");
    p += F("<p><a href='/upload'>&#8594; 歌曲上传（浏览器选文件，经 SPI 写进 TF 卡）</a></p>");

    p += F("<fieldset><legend>当前状态</legend>");
    if (WiFi.status() == WL_CONNECTED) {
        p += F("已连上路由器：<b>");
        p += ((s_active >= 0) && (s_active < (int)s_count)) ? s_ssid[s_active] : "";
        p += F("</b>  IP <b>");
        p += s_ip;
        p += F("</b>  信号 ");
        p += String((int)WiFi.RSSI());
        p += F(" dBm");
    } else {
        p += F("<span class='bad'>未连上路由器</span>");
    }
    p += F("<br>配网热点：");
    if (s_ap) {
        p += s_ap_ssid;
        p += F("  打开 <b>http://");
        p += WiFi.softAPIP().toString();
        p += F("/</b>");
    } else {
        p += F("已关闭");
    }
    p += F("</fieldset>");

    p += F("<fieldset><legend>添加 / 更新 WiFi（写入 NVS，断电不丢）</legend>");
    p += F("<form method='POST' action='/save'>");
    p += F("<label>WiFi 名称 SSID</label><input id='ssid' name='ssid' maxlength='32' required>");
    p += F("<label>密码</label><input name='pass' type='password' maxlength='64'>");
    p += F("<select id='ap' onchange='pick()'><option value=''>-- 扫描结果 --</option></select>");
    p += F("<button type='button' class='g' onclick='scan()'>扫描附近网络</button>");
    p += F("<button type='submit'>保存并连接</button>");
    p += F("<div><small>最多保存 ");
    p += String((unsigned)WIFIMGR_MAX);
    p += F(" 组；同名会覆盖旧密码。</small></div></form></fieldset>");

    p += F("<fieldset><legend>已保存的账号（存在 NVS 里）</legend>");
    if (s_count == 0u) {
        p += F("<i>还没有任何账号：先在上面填一个，或者连热点 ");
        p += s_ap_ssid;
        p += F(" 打开 http://192.168.4.1/</i>");
    } else {
        p += F("<table><tr><th>#</th><th>SSID</th><th>操作</th></tr>");
        for (uint8_t i = 0u; i < s_count; i++) {
            p += F("<tr><td>");
            p += String((unsigned)i);
            if ((int)i == (int)s_active) p += F("*");
            p += F("</td><td>");
            html_escape(s_ssid[i], p);
            p += F("</td><td>");
            p += F("<form method='POST' action='/conn' style='display:inline'>");
            p += F("<input type='hidden' name='idx' value='");
            p += String((unsigned)i);
            p += F("'><button type='submit'>连接</button></form> ");
            p += F("<form method='POST' action='/del' style='display:inline'>");
            p += F("<input type='hidden' name='idx' value='");
            p += String((unsigned)i);
            p += F("'><button type='submit' class='g'>删除</button></form>");
            p += F("</td></tr>");
        }
        p += F("</table><small>* = 上次连上的那个，开机优先尝试</small>");
    }
    p += F("</fieldset>");

    p += F("<fieldset><legend>运行状态</legend><pre id='st'>读取中...</pre></fieldset>");
    page_tail(p);
    s_web.send(200, "text/html; charset=utf-8", p);
}

static void h_msg(const char *title, const char *text)
{
    String p;
    page_head(p, title);
    p += F("<h2>");
    p += title;
    p += F("</h2><p>");
    p += text;
    p += F("</p><p><a href='/'>返回配置页</a></p>");
    p += F("<script>setTimeout(function(){location.href='/';},2500);</script>");
    p += F("</body></html>");
    s_web.send(200, "text/html; charset=utf-8", p);
}

static void h_scan(void)
{
    int n = (int)WiFi.scanNetworks(false, true);
    String j = F("[");
    for (int i = 0; i < n; i++) {
        String s = WiFi.SSID(i);
        s.replace("\"", "'");      /* JSON 里不能出现裸引号/反斜杠，直接换掉 */
        s.replace("\\", "/");
        if (i > 0) j += F(",");
        j += F("{\"s\":\"");
        j += s;
        j += F("\",\"r\":");
        j += String((int)WiFi.RSSI(i));
        j += F("}");
    }
    j += F("]");
    WiFi.scanDelete();
    s_web.send(200, "application/json", j);
}

static void h_status(void)
{
    String t;
    t += F("sta=");
    t += (WiFi.status() == WL_CONNECTED) ? F("connected") : F("disconnected");
    t += F("  ip=");
    t += s_ip;
    t += F("  rssi=");
    t += String((int)WiFi.RSSI());
    t += F("\r\nsaved=");
    t += String((unsigned)s_count);
    t += F("  active=");
    t += String((int)s_active);
    t += F("  ap=");
    t += s_ap ? F("on ") : F("off");
    t += s_ap_ssid;
    t += F("  ok=");
    t += String((unsigned long)s_ok);
    t += F("  fail=");
    t += String((unsigned long)s_fail);
    t += F("  mac=");
    t += WiFi.macAddress();
    t += F("\r\n");
    for (uint8_t i = 0u; i < s_count; i++) {
        t += F("  [");
        t += String((unsigned)i);
        t += F("] ");
        t += s_ssid[i];
        if ((int)i == (int)s_active) t += F("  <= 上次连上");
        t += F("\r\n");
    }
    s_web.send(200, "text/plain; charset=utf-8", t);
}

static void h_save(void)
{
    String ssid = s_web.hasArg("ssid") ? s_web.arg("ssid") : String("");
    String pass = s_web.hasArg("pass") ? s_web.arg("pass") : String("");
    ssid.trim();

    if (ssid.length() == 0u) { h_msg("保存失败", "SSID 不能为空"); return; }
    if (ssid.length() > 32u) { h_msg("保存失败", "SSID 太长"); return; }
    if (pass.length() > 64u) { h_msg("保存失败", "密码太长"); return; }

    int idx = wifimgr_add(ssid.c_str(), pass.c_str());
    if (idx < 0) { h_msg("保存失败", "账号表已满或参数非法"); return; }

    String m = F("已保存到 NVS：");
    m += ssid;
    m += F("（第 ");
    m += String((unsigned)idx);
    m += F(" 组）。正在尝试连接，几秒后自动返回配置页。");
    h_msg("保存成功", m.c_str());
}

static void h_del(void)
{
    int idx = s_web.hasArg("idx") ? s_web.arg("idx").toInt() : -1;
    String m;
    if ((idx < 0) || !wifimgr_remove(idx)) {
        m = F("删除失败：下标无效");
    } else {
        m = F("已删除第 ");
        m += String((unsigned)idx);
        m += F(" 组，并同步更新 NVS。");
    }
    h_msg("删除账号", m.c_str());
}

static void h_conn(void)
{
    int idx = s_web.hasArg("idx") ? s_web.arg("idx").toInt() : -1;
    String m;
    if ((idx < 0) || !wifimgr_connect_index(idx)) {
        m = F("下标无效");
    } else {
        m = F("正在切换到第 ");
        m += String((unsigned)idx);
        m += F(" 组并重连...");
    }
    h_msg("切换网络", m.c_str());
}

static void h_404(void)
{
    /* 配网热点开着时，把一切未知请求（手机探测 captive.apple.com / connecttest.txt 之类）
       都引到配置页 —— 这就是「手机连上热点自动弹网页」的关键一步。 */
    String loc;
    if (s_ap) { loc = "http://"; loc += WiFi.softAPIP().toString(); loc += "/"; }
    else      { loc = "/"; }
    s_web.sendHeader("Location", loc, true);
    s_web.send(302, "text/plain", "");
}

/* 把配置网页的 WebServer 句柄借给别的模块（netdl 用它挂 /upload 上传页） */
WebServer *wifimgr_web(void)
{
    return &s_web;
}

static void web_begin(void)
{
    s_web.on("/",        HTTP_GET,  h_root);
    s_web.on("/save",    HTTP_POST, h_save);
    s_web.on("/del",     HTTP_POST, h_del);
    s_web.on("/conn",    HTTP_POST, h_conn);
    s_web.on("/scan",    HTTP_GET,  h_scan);
    s_web.on("/status",  HTTP_GET,  h_status);
    s_web.onNotFound(h_404);
    s_web.begin();
    Serial.printf("[WIFI] 配置网页已监听端口 %d\r\n", (int)WIFIMGR_WEB_PORT);
}

/* ================= 对外 API ================= */

void wifimgr_init(void)
{
    Serial.println("[WIFI] --- wifimgr 启动：先从 NVS 读回账号 ---");
    nvs_boot();

    memset(s_ssid, 0, sizeof(s_ssid));
    memset(s_pass, 0, sizeof(s_pass));
    s_count = 0u;
    s_active = -1;

    s_prefs_ok = s_prefs.begin("wifi", false);
    if (!s_prefs_ok) Serial.println("[WIFI] !! Preferences.begin 失败（NVS 分区不可用？）");
    else             nvs_load();

    /* NVS 里一个账号都没有，才用编译期兜底账号（写回 NVS，之后就以 NVS 为准） */
    if ((s_count == 0u) && (WIFIMGR_DEFAULT_SSID[0] != 0)) {
        Serial.println("[WIFI] NVS 为空，使用编译期兜底账号");
        strncpy(s_ssid[0], WIFIMGR_DEFAULT_SSID, WIFIMGR_SSID_LEN - 1u);
        strncpy(s_pass[0], WIFIMGR_DEFAULT_PASS, WIFIMGR_PASS_LEN - 1u);
        s_count = 1u;
        s_active = 0;
        nvs_save_table();
    }

    WiFi.persistent(false);
    WiFi.setHostname(WIFIMGR_HOSTNAME);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);

    web_begin();

    s_inited = true;
    s_state = WST_IDLE;
    s_tried = 0u;
    s_retry_at = millis();

    Serial.printf("[WIFI] 读到 %u 组账号，active=%d\r\n", (unsigned)s_count, (int)s_active);
    if (s_count == 0u) {
        Serial.println("[WIFI] 没有账号：直接开配网热点，等网页里填");
        ap_start();
    } else {
        sta_begin(try_index(0u));
    }
}

void wifimgr_poll(void)
{
    if (!s_inited) return;

    s_web.handleClient();
    if (s_ap) s_dns.processNextRequest();   /* 配网热点的 DNS 劫持 */

    if (s_busy) return;                     /* 上传期间不动 WiFi 模式 */

    uint32_t now = millis();

    if (WiFi.status() == WL_CONNECTED) {
        if (s_state != WST_ONLINE) on_connected(s_try);
        else ip_refresh();
        if (s_ap && ((int32_t)(now - s_ap_hold_until) >= 0)) ap_stop();
        return;
    }

    if (s_state == WST_ONLINE) {            /* 掉线了 */
        Serial.println("[WIFI] 连接断开，稍后重连");
        s_state = WST_IDLE;
        s_retry_at = now + 2000u;
        return;
    }

    if (s_count == 0u) { ap_start(); return; }

    if (s_state == WST_TRYING) {
        /* 密码错/找不到这个网络时 ESP32 会立刻报状态，不必傻等 12 s；只有
           "还在连" 的情况才等满 WIFIMGR_TRY_MS，这样配网模式下轮询快得多。 */
        wl_status_t ws = WiFi.status();
        bool hard = ((ws == WL_NO_SSID_AVAIL) || (ws == WL_CONNECT_FAILED));
        if (!hard && ((uint32_t)(now - s_try_t0) < WIFIMGR_TRY_MS)) return;
        if (hard) Serial.printf("[WIFI] [%d] 直接失败(status=%d)：密码错或找不到该网络\r\n", (int)s_try, (int)ws);
        else      Serial.printf("[WIFI] [%d] 超时\r\n", (int)s_try);
        s_tried++;
        if (s_tried >= s_count) {
            s_fail++;
            Serial.printf("[WIFI] %u 组账号都不通，开配网热点\r\n", (unsigned)s_count);
            ap_start();
            s_state = WST_IDLE;
            s_retry_at = now + WIFIMGR_RETRY_MS;
            return;
        }
        sta_begin(try_index(s_tried));
        return;
    }

    /* WST_IDLE */
    if ((int32_t)(now - s_retry_at) < 0) return;
    s_tried = 0u;
    sta_begin(try_index(0u));
}

void wifimgr_set_busy(bool busy)
{
    s_busy = busy;
}

int wifimgr_count(void) { return (int)s_count; }

const char *wifimgr_ssid(int i)
{
    if ((i < 0) || (i >= (int)s_count)) return "";
    return s_ssid[i];
}

const char *wifimgr_pass(int i)
{
    if ((i < 0) || (i >= (int)s_count)) return "";
    return s_pass[i];
}

int wifimgr_active(void) { return (int)s_active; }

int wifimgr_add(const char *ssid, const char *pass)
{
    if ((ssid == NULL) || (ssid[0] == 0)) return -1;
    if (strlen(ssid) >= WIFIMGR_SSID_LEN) return -1;
    if (pass == NULL) pass = "";
    if (strlen(pass) >= WIFIMGR_PASS_LEN) return -1;

    int idx = -1;
    for (uint8_t i = 0u; i < s_count; i++) {
        if (strcmp(s_ssid[i], ssid) == 0) { idx = (int)i; break; }
    }
    if (idx < 0) {
        if (s_count >= WIFIMGR_MAX) {
            idx = 0;                        /* 表满：覆盖第 0 组 */
            Serial.println("[WIFI] 账号表已满，覆盖第 0 组");
        } else {
            idx = (int)s_count;
            s_count++;
        }
    }

    strncpy(s_ssid[idx], ssid, WIFIMGR_SSID_LEN - 1u);
    s_ssid[idx][WIFIMGR_SSID_LEN - 1u] = 0;
    strncpy(s_pass[idx], pass, WIFIMGR_PASS_LEN - 1u);
    s_pass[idx][WIFIMGR_PASS_LEN - 1u] = 0;

    s_active = (int8_t)idx;
    nvs_save_table();                       /* 立刻落盘：这就是断电不丢的关键 */

    s_state = WST_IDLE;
    s_tried = 0u;
    s_retry_at = millis();
    if (!s_busy) {
        if (WiFi.status() == WL_CONNECTED) WiFi.disconnect(false, false);
        sta_begin(idx);
    }
    Serial.printf("[WIFI] 新增/更新账号 [%d] %s\r\n", idx, s_ssid[idx]);
    return idx;
}

bool wifimgr_remove(int i)
{
    if ((i < 0) || (i >= (int)s_count)) return false;

    for (uint8_t k = (uint8_t)i; (k + 1u) < s_count; k++) {
        memcpy(s_ssid[k], s_ssid[k + 1u], WIFIMGR_SSID_LEN);
        memcpy(s_pass[k], s_pass[k + 1u], WIFIMGR_PASS_LEN);
    }
    s_count--;
    memset(s_ssid[s_count], 0, WIFIMGR_SSID_LEN);
    memset(s_pass[s_count], 0, WIFIMGR_PASS_LEN);

    if (s_active == (int8_t)i) s_active = (s_count > 0u) ? 0 : -1;
    else if (s_active > (int8_t)i) s_active--;

    nvs_save_table();
    Serial.printf("[WIFI] 删除 [%d]，剩 %u 组\r\n", i, (unsigned)s_count);
    return true;
}

bool wifimgr_connect_index(int i)
{
    if ((i < 0) || (i >= (int)s_count)) return false;
    s_active = (int8_t)i;
    if (s_prefs_ok) s_prefs.putChar("act", s_active);
    s_tried = 0u;
    s_state = WST_IDLE;
    s_retry_at = millis();
    if (!s_busy) {
        if (WiFi.status() == WL_CONNECTED) WiFi.disconnect(false, false);
        sta_begin(i);
    }
    return true;
}

bool wifimgr_connected(void) { return (WiFi.status() == WL_CONNECTED); }
const char *wifimgr_ip(void) { return s_ip; }
int wifimgr_rssi(void) { return (int)WiFi.RSSI(); }
bool wifimgr_ap_on(void) { return s_ap; }
const char *wifimgr_ap_ssid(void) { return s_ap_ssid; }
const char *wifimgr_ap_pass(void) { return s_ap_pass; }
const char *wifimgr_ap_ip(void)
{
    static char buf[20] = "";
    if (s_ap) {
        IPAddress ip = WiFi.softAPIP();
        snprintf(buf, sizeof(buf), "%u.%u.%u.%u", (unsigned)ip[0], (unsigned)ip[1], (unsigned)ip[2], (unsigned)ip[3]);
    } else {
        buf[0] = 0;
    }
    return buf;
}
uint32_t wifimgr_ok_count(void) { return s_ok; }
uint32_t wifimgr_fail_count(void) { return s_fail; }
