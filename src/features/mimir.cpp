/*
 * mimir.cpp — MIMIR pentest drop-box control client.
 *
 * Talks to a Banana Pi BPI-M4 Zero running the MIMIR daemon over
 * USB-C to USB-C. Protocol is newline-delimited JSON over USB-CDC.
 * All parsing is hand-rolled — no ArduinoJson, no heap alloc.
 *
 * Screens:
 *   MS_MAIN     — landing page + connection status + stats
 *   MS_TARGETS  — scrollable AP list from MIMIR scan results
 *   MS_ATTACK   — selected target + attack hotkeys
 *   MS_LIVE     — running attack status + animated TX bars
 *   MS_STATUS   — diagnostic view with auto-refresh
 */
#include "app.h"
#include "../theme.h"
#include "ui.h"
#include "input.h"
#include "mimir.h"

/* ---- data structures (all fixed-size, no heap) ---- */

#define MIMIR_MAX_APS 64

struct APRow {
    char    bssid[18];
    char    ssid[33];
    uint8_t channel;
    int8_t  rssi;
    char    enc[8];
    bool    wps;
    uint8_t clients;
};

static APRow s_targets[MIMIR_MAX_APS];
static int   s_target_count = 0;

struct MimirStatus {
    char     state[16];
    uint8_t  battery;
    uint8_t  gps_sats;
    uint16_t handshakes;
    uint16_t cracked;
    bool     pocket_mode;
};

static MimirStatus s_status     = {};
static bool        s_connected  = false;
static char        s_live_msg[128] = "";
static uint16_t    s_live_color = T_FG;
static char        s_filter[33] = "";
static char        s_rx_buf[1024];
static int         s_rx_len     = 0;
static int         s_target_sel = 0;
static char        s_attack_mode[16] = "";

enum MimirScreen { MS_MAIN, MS_TARGETS, MS_ATTACK, MS_LIVE, MS_STATUS };

/* ---- hand-rolled JSON helpers ---- */

/* Extract a string value for "key":"value". Returns true if found. */
static bool json_str(const char *buf, const char *key, char *out, int out_sz)
{
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char *p = strstr(buf, pat);
    if (!p) return false;
    p += strlen(pat);
    int i = 0;
    while (*p && *p != '"' && i < out_sz - 1) out[i++] = *p++;
    out[i] = '\0';
    return true;
}

/* Extract an integer value for "key":123. Returns def if not found. */
static int json_int(const char *buf, const char *key, int def)
{
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(buf, pat);
    if (!p) return def;
    p += strlen(pat);
    /* skip whitespace */
    while (*p == ' ') p++;
    if (*p == '"') return def;  /* it's a string, not int */
    return atoi(p);
}

/* Extract a boolean value for "key":true/false. Returns def if not found. */
static bool json_bool(const char *buf, const char *key, bool def)
{
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(buf, pat);
    if (!p) return def;
    p += strlen(pat);
    while (*p == ' ') p++;
    if (*p == 't') return true;
    if (*p == 'f') return false;
    return def;
}

/* ---- command senders ---- */

static void send_hello(void)
{
    Serial.println("{\"cmd\":\"hello\",\"ver\":1}");
}

static void send_scan_dual(int duration)
{
    Serial.printf("{\"cmd\":\"scan\",\"bands\":[\"2g\",\"5g\"],\"duration\":%d}\n",
                  duration);
}

static void send_select(const char *bssid)
{
    Serial.printf("{\"cmd\":\"select\",\"bssid\":\"%s\"}\n", bssid);
}

static void send_attack_deauth(const char *bssid, int bursts)
{
    Serial.printf("{\"cmd\":\"attack\",\"mode\":\"deauth\",\"bssid\":\"%s\",\"bursts\":%d}\n",
                  bssid, bursts);
}

static void send_attack_handshake(const char *bssid, int timeout)
{
    Serial.printf("{\"cmd\":\"attack\",\"mode\":\"handshake\",\"bssid\":\"%s\",\"timeout\":%d}\n",
                  bssid, timeout);
}

static void send_attack_pmkid(const char *bssid, int timeout)
{
    Serial.printf("{\"cmd\":\"attack\",\"mode\":\"pmkid\",\"bssid\":\"%s\",\"timeout\":%d}\n",
                  bssid, timeout);
}

static void send_attack_evil_twin(const char *ssid, int channel)
{
    Serial.printf("{\"cmd\":\"attack\",\"mode\":\"evil_twin\",\"ssid\":\"%s\",\"channel\":%d}\n",
                  ssid, channel);
}

static void send_attack_beacon_spam(const char *name, int channel)
{
    Serial.printf("{\"cmd\":\"attack\",\"mode\":\"beacon_spam\",\"names\":[\"%s\"],\"channel\":%d}\n",
                  name, channel);
}

static void send_stop(void)
{
    Serial.println("{\"cmd\":\"stop\"}");
}

static void send_status(void)
{
    Serial.println("{\"cmd\":\"status\"}");
}

static void send_loot(void)
{
    Serial.println("{\"cmd\":\"loot\"}");
}

static void send_mode(bool pocket)
{
    Serial.printf("{\"cmd\":\"mode\",\"pocket\":%s}\n", pocket ? "true" : "false");
}

/* ---- event handler ---- */

static void handle_line(const char *buf)
{
    char evt[24];
    if (!json_str(buf, "evt", evt, sizeof(evt))) return;

    if (strcmp(evt, "hello") == 0) {
        s_connected = true;
        snprintf(s_live_msg, sizeof(s_live_msg), "MIMIR online");
        s_live_color = T_GOOD;
    }
    else if (strcmp(evt, "ap") == 0) {
        if (s_target_count < MIMIR_MAX_APS) {
            APRow &a = s_targets[s_target_count];
            json_str(buf, "bssid", a.bssid, sizeof(a.bssid));
            json_str(buf, "ssid",  a.ssid,  sizeof(a.ssid));
            a.channel = (uint8_t)json_int(buf, "ch", 0);
            a.rssi    = (int8_t)json_int(buf, "rssi", -99);
            json_str(buf, "enc", a.enc, sizeof(a.enc));
            a.wps     = json_bool(buf, "wps", false);
            a.clients = (uint8_t)json_int(buf, "clients", 0);
            s_target_count++;
        }
    }
    else if (strcmp(evt, "scan_started") == 0) {
        s_target_count = 0;
        s_target_sel   = 0;
        snprintf(s_live_msg, sizeof(s_live_msg), "Scanning...");
        s_live_color = T_ACCENT;
    }
    else if (strcmp(evt, "scan_done") == 0) {
        int cnt = json_int(buf, "count", s_target_count);
        snprintf(s_live_msg, sizeof(s_live_msg), "Scan done: %d APs", cnt);
        s_live_color = T_GOOD;
    }
    else if (strcmp(evt, "status") == 0) {
        json_str(buf, "state", s_status.state, sizeof(s_status.state));
        s_status.battery    = (uint8_t)json_int(buf, "bat", 0);
        s_status.gps_sats   = (uint8_t)json_int(buf, "gps", 0);
        s_status.handshakes = (uint16_t)json_int(buf, "handshakes", 0);
        s_status.cracked    = (uint16_t)json_int(buf, "cracked", 0);
    }
    else if (strcmp(evt, "attack_started") == 0) {
        char mode[16];
        json_str(buf, "mode", mode, sizeof(mode));
        snprintf(s_live_msg, sizeof(s_live_msg), "%s ACTIVE", mode);
        s_live_color = T_BAD;
    }
    else if (strcmp(evt, "attack_stopped") == 0) {
        char mode[16], reason[16];
        json_str(buf, "mode",   mode,   sizeof(mode));
        json_str(buf, "reason", reason, sizeof(reason));
        snprintf(s_live_msg, sizeof(s_live_msg), "%s stopped: %s", mode, reason);
        s_live_color = T_WARN;
    }
    else if (strcmp(evt, "handshake") == 0) {
        char ssid[33];
        json_str(buf, "ssid", ssid, sizeof(ssid));
        snprintf(s_live_msg, sizeof(s_live_msg), "HANDSHAKE! %s", ssid);
        s_live_color = T_GOOD;
    }
    else if (strcmp(evt, "pmkid") == 0) {
        char bssid[18];
        json_str(buf, "bssid", bssid, sizeof(bssid));
        snprintf(s_live_msg, sizeof(s_live_msg), "PMKID captured %s", bssid);
        s_live_color = T_GOOD;
    }
    else if (strcmp(evt, "cracked") == 0) {
        char ssid[33];
        json_str(buf, "ssid", ssid, sizeof(ssid));
        snprintf(s_live_msg, sizeof(s_live_msg), "CRACKED: %s", ssid);
        s_live_color = T_ACCENT2;
        s_status.cracked++;
    }
    else if (strcmp(evt, "state") == 0) {
        json_str(buf, "state", s_status.state, sizeof(s_status.state));
    }
    else if (strcmp(evt, "mode") == 0) {
        s_status.pocket_mode = json_bool(buf, "pocket", s_status.pocket_mode);
    }
    else if (strcmp(evt, "selected") == 0) {
        char bssid[18];
        json_str(buf, "bssid", bssid, sizeof(bssid));
        snprintf(s_live_msg, sizeof(s_live_msg), "Selected %s", bssid);
        s_live_color = T_ACCENT;
    }
    else if (strcmp(evt, "loot") == 0) {
        int hs = json_int(buf, "handshakes", 0);
        s_status.handshakes = (uint16_t)hs;
        snprintf(s_live_msg, sizeof(s_live_msg), "Loot: %d hs", hs);
        s_live_color = T_ACCENT2;
    }
}

/* ---- serial rx pump ---- */

static void pump_rx(void)
{
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\r') continue;  /* strip \r — MIMIR may send \r\n */
        if (c == '\n') {
            s_rx_buf[s_rx_len] = '\0';
            if (s_rx_len > 0) handle_line(s_rx_buf);
            s_rx_len = 0;
        } else if (s_rx_len < (int)sizeof(s_rx_buf) - 1) {
            s_rx_buf[s_rx_len++] = c;
        } else {
            /* Overflow: skip to next \n to resync. */
            while (Serial.available()) {
                if ((char)Serial.read() == '\n') break;
            }
            s_rx_len = 0;
        }
    }
}

/* ---- filter helper ---- */

static bool ap_matches_filter(const APRow &a)
{
    if (s_filter[0] == '\0') return true;
    const char *s = a.ssid;
    size_t fl = strlen(s_filter);
    for (; *s; ++s) {
        if (strncasecmp(s, s_filter, fl) == 0) return true;
    }
    return false;
}

/* ---- screen drawers ---- */

static void draw_main(void)
{
    ui_clear_body();
    auto &d = M5Cardputer.Display;

    d.setTextColor(T_ACCENT2, T_BG);
    d.setCursor(4, BODY_Y + 2);
    d.print("MIMIR");
    d.drawFastHLine(4, BODY_Y + 12, 50, T_ACCENT2);

    /* Connection status. */
    d.setTextColor(s_connected ? T_GOOD : T_BAD, T_BG);
    d.setCursor(60, BODY_Y + 2);
    d.print(s_connected ? "CONNECTED" : "DISCONNECTED");

    /* Status fields. */
    d.setTextColor(T_FG, T_BG);
    d.setCursor(4, BODY_Y + 20);
    d.printf("State: %s", s_status.state[0] ? s_status.state : "---");
    d.setCursor(4, BODY_Y + 32);
    d.printf("Bat: %d%%  GPS: %d sats", s_status.battery, s_status.gps_sats);
    d.setCursor(4, BODY_Y + 44);
    d.printf("HS: %d  Cracked: %d", s_status.handshakes, s_status.cracked);

    /* Pocket mode. */
    d.setTextColor(s_status.pocket_mode ? T_WARN : T_DIM, T_BG);
    d.setCursor(4, BODY_Y + 56);
    d.printf("Pocket: %s", s_status.pocket_mode ? "ON" : "off");

    /* Live message. */
    if (s_live_msg[0]) {
        d.setTextColor(s_live_color, T_BG);
        d.setCursor(4, BODY_Y + 72);
        d.printf("%.38s", s_live_msg);
    }

    ui_draw_footer("S=scan T=tgt A=atk L=loot Q=pkt I=stat `=exit");
}

static void draw_targets(void)
{
    ui_clear_body();
    auto &d = M5Cardputer.Display;

    /* Build filtered index. */
    int idx[MIMIR_MAX_APS];
    int n = 0;
    for (int i = 0; i < s_target_count; ++i) {
        if (ap_matches_filter(s_targets[i])) idx[n++] = i;
    }

    /* Title. */
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2);
    d.printf("TARGETS %d", n);
    if (s_filter[0]) {
        d.setTextColor(T_WARN, T_BG);
        d.printf("  /%s", s_filter);
    }

    if (n == 0) {
        d.setTextColor(T_DIM, T_BG);
        d.setCursor(4, BODY_Y + 18);
        d.print("no targets — press S to scan");
        ui_draw_footer("S=scan F=filter `=back");
        return;
    }

    /* Clamp cursor. */
    if (s_target_sel >= n) s_target_sel = n - 1;
    if (s_target_sel < 0) s_target_sel = 0;

    /* Scrolling 7-row window. */
    const int rows   = 7;
    const int row_h  = 13;
    const int first_y = BODY_Y + 14;
    int first = s_target_sel - rows / 2;
    if (first < 0) first = 0;
    if (first + rows > n) first = max(0, n - rows);

    for (int r = 0; r < rows && first + r < n; ++r) {
        int ai = idx[first + r];
        const APRow &a = s_targets[ai];
        int y = first_y + r * row_h;
        bool sel = (first + r == s_target_sel);
        uint16_t sel_bg = 0x3007;
        if (sel) {
            d.fillRoundRect(2, y - 1, SCR_W - 4, 12, 2, sel_bg);
            d.drawRoundRect(2, y - 1, SCR_W - 4, 12, 2, T_ACCENT2);
            d.drawRoundRect(3, y,     SCR_W - 6, 10, 2, T_ACCENT);
        }
        uint16_t bg = sel ? sel_bg : T_BG;

        /* SSID (18 chars max) */
        d.setTextColor(sel ? T_FG : T_FG, bg);
        d.setCursor(4, y + 1);
        d.printf("%-18.18s", a.ssid[0] ? a.ssid : "<hidden>");

        /* channel */
        d.setTextColor(T_DIM, bg);
        d.setCursor(118, y + 1);
        d.printf("%2u", a.channel);

        /* RSSI */
        uint16_t rssi_col = a.rssi > -60 ? T_GOOD : (a.rssi > -75 ? T_WARN : T_BAD);
        d.setTextColor(rssi_col, bg);
        d.setCursor(138, y + 1);
        d.printf("%4d", a.rssi);

        /* enc */
        d.setTextColor(T_ACCENT, bg);
        d.setCursor(170, y + 1);
        d.printf("%-4.4s", a.enc);

        /* WPS indicator */
        if (a.wps) {
            d.setTextColor(T_WARN, bg);
            d.setCursor(200, y + 1);
            d.print("W");
        }
    }

    /* Scroll arrows. */
    if (first > 0) {
        d.fillTriangle(SCR_W - 7, first_y - 3,
                       SCR_W - 3, first_y - 3,
                       SCR_W - 5, first_y - 6, T_ACCENT2);
    }
    if (first + rows < n) {
        int ay = first_y + rows * row_h - 2;
        d.fillTriangle(SCR_W - 7, ay,
                       SCR_W - 3, ay,
                       SCR_W - 5, ay + 3, T_ACCENT2);
    }

    ui_draw_footer(";/.=move ENTER=sel F=filter S=scan `=back");
}

static void draw_attack(void)
{
    ui_clear_body();
    auto &d = M5Cardputer.Display;

    /* Build filtered index to find selected AP. */
    int idx[MIMIR_MAX_APS];
    int n = 0;
    for (int i = 0; i < s_target_count; ++i) {
        if (ap_matches_filter(s_targets[i])) idx[n++] = i;
    }
    if (n == 0 || s_target_sel >= n) {
        d.setTextColor(T_DIM, T_BG);
        d.setCursor(4, BODY_Y + 18);
        d.print("no target selected");
        ui_draw_footer("`=back");
        return;
    }

    const APRow &a = s_targets[idx[s_target_sel]];

    d.setTextColor(T_ACCENT2, T_BG);
    d.setCursor(4, BODY_Y + 2);
    d.print("ATTACK");
    d.drawFastHLine(4, BODY_Y + 12, 60, T_ACCENT2);

    d.setTextColor(T_FG, T_BG);
    d.setCursor(4, BODY_Y + 18);
    d.printf("SSID : %.24s", a.ssid[0] ? a.ssid : "<hidden>");
    d.setCursor(4, BODY_Y + 30);
    d.printf("BSSID: %s", a.bssid);
    d.setCursor(4, BODY_Y + 42);
    d.printf("CH: %u  ENC: %s", a.channel, a.enc);

    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 58);
    d.print("[D] Deauth");
    d.setCursor(4, BODY_Y + 70);
    d.print("[H] Handshake capture");
    d.setCursor(4, BODY_Y + 82);
    d.print("[P] PMKID capture");
    d.setCursor(120, BODY_Y + 58);
    d.print("[E] Evil twin");
    d.setCursor(120, BODY_Y + 70);
    d.print("[B] Beacon spam");

    ui_draw_footer("D=dth H=hs P=pmkid E=evil B=beacon `=back");
}

static void draw_live(void)
{
    ui_clear_body();
    auto &d = M5Cardputer.Display;

    d.setTextColor(T_BAD, T_BG);
    d.setCursor(4, BODY_Y + 2);
    d.printf("LIVE: %s", s_attack_mode);
    d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_BAD);

    /* Live message. */
    d.setTextColor(s_live_color, T_BG);
    d.setCursor(4, BODY_Y + 22);
    d.printf("%.38s", s_live_msg);

    /* Animated TX indicator bars (like jammer). */
    for (int i = 0; i < 8; ++i) {
        int h = (esp_random() % 24) + 4;
        int x = 10 + i * 28;
        int y = BODY_Y + 90 - h;
        d.fillRect(x, y, 20, h, T_BAD);
    }

    /* Hex stream backdrop for that attack vibe. */
    ui_hexstream(4, BODY_Y + 40, SCR_W - 8, 20, T_DIM);

    ui_draw_footer("X=stop `=minimize");
}

static void draw_status_screen(void)
{
    ui_clear_body();
    auto &d = M5Cardputer.Display;

    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2);
    d.print("MIMIR STATUS");
    d.drawFastHLine(4, BODY_Y + 12, 110, T_ACCENT);

    d.setTextColor(T_FG, T_BG);
    d.setCursor(4, BODY_Y + 20);
    d.printf("Connected: %s", s_connected ? "YES" : "NO");
    d.setCursor(4, BODY_Y + 32);
    d.printf("State    : %s", s_status.state[0] ? s_status.state : "---");
    d.setCursor(4, BODY_Y + 44);
    d.printf("Battery  : %d%%", s_status.battery);
    d.setCursor(4, BODY_Y + 56);
    d.printf("GPS sats : %d", s_status.gps_sats);
    d.setCursor(4, BODY_Y + 68);
    d.printf("Handshake: %d", s_status.handshakes);
    d.setCursor(4, BODY_Y + 80);
    d.printf("Cracked  : %d", s_status.cracked);
    d.setCursor(4, BODY_Y + 92);
    d.printf("Pocket   : %s", s_status.pocket_mode ? "ON" : "off");

    ui_draw_footer("auto-refresh 2s  `=back");
}

/* ---- main feature entry point ---- */

bool g_mimir_cdc_active = false;

void feat_mimir(void)
{
    g_mimir_cdc_active = true;

    /* Reset state. */
    s_connected    = false;
    s_target_count = 0;
    s_target_sel   = 0;
    s_rx_len       = 0;
    s_live_msg[0]  = '\0';
    s_live_color   = T_FG;
    s_filter[0]    = '\0';
    s_attack_mode[0] = '\0';
    memset(&s_status, 0, sizeof(s_status));

    MimirScreen screen = MS_MAIN;
    MimirScreen prev   = MS_MAIN;
    send_hello();
    uint32_t last_draw   = 0;
    uint32_t last_status = 0;

    ui_draw_status("mimir", "init");

    while (true) {
        pump_rx();

        /* Cable disconnect detection. */
        if (!Serial) {
            s_connected = false;
            ui_toast("MIMIR disconnected", T_BAD, 1500);
            break;
        }

        uint16_t k = input_poll();

        /* ESC goes back one screen, exits from main. */
        if (k == PK_ESC) {
            if (screen == MS_MAIN) {
                send_stop();  /* don't orphan running attacks on MIMIR */
                break;
            }
            screen = prev;
            prev   = MS_MAIN;
            last_draw = 0;  /* force redraw */
            continue;
        }

        /* Per-screen key handling. */
        switch (screen) {
        case MS_MAIN:
            if (k == 's' || k == 'S') {
                send_scan_dual(15);
                snprintf(s_live_msg, sizeof(s_live_msg), "Scan requested");
                s_live_color = T_ACCENT;
                prev = MS_MAIN; screen = MS_TARGETS; last_draw = 0;
            }
            else if (k == 't' || k == 'T') {
                prev = MS_MAIN; screen = MS_TARGETS; last_draw = 0;
            }
            else if (k == 'a' || k == 'A') {
                prev = MS_MAIN; screen = MS_ATTACK; last_draw = 0;
            }
            else if (k == 'l' || k == 'L') {
                send_loot();
            }
            else if (k == 'q' || k == 'Q') {
                /* Don't flip local state until MIMIR confirms via evt:mode */
                send_mode(!s_status.pocket_mode);
            }
            else if (k == 'i' || k == 'I') {
                prev = MS_MAIN; screen = MS_STATUS; last_draw = 0;
            }
            break;

        case MS_TARGETS: {
            /* Filtered count for clamping. */
            int filt_n = 0;
            for (int i = 0; i < s_target_count; ++i)
                if (ap_matches_filter(s_targets[i])) filt_n++;

            if (k == ';' || k == PK_UP) {
                s_target_sel--;
                if (s_target_sel < 0) s_target_sel = 0;
                last_draw = 0;
            }
            else if (k == '.' || k == PK_DOWN) {
                s_target_sel++;
                if (s_target_sel >= filt_n) s_target_sel = filt_n - 1;
                if (s_target_sel < 0) s_target_sel = 0;
                last_draw = 0;
            }
            else if (k == PK_ENTER) {
                /* Select AP and send to MIMIR, move to attack screen. */
                int idx[MIMIR_MAX_APS];
                int n = 0;
                for (int i = 0; i < s_target_count; ++i)
                    if (ap_matches_filter(s_targets[i])) idx[n++] = i;
                if (n > 0 && s_target_sel < n) {
                    send_select(s_targets[idx[s_target_sel]].bssid);
                    prev = MS_TARGETS; screen = MS_ATTACK; last_draw = 0;
                }
            }
            else if (k == 'f' || k == 'F') {
                if (input_line("Filter SSID:", s_filter, sizeof(s_filter))) {
                    s_target_sel = 0;
                } else {
                    s_filter[0] = '\0';
                }
                last_draw = 0;
            }
            else if (k == 's' || k == 'S') {
                send_scan_dual(15);
            }
            break;
        }

        case MS_ATTACK: {
            /* Resolve selected target. */
            int idx[MIMIR_MAX_APS];
            int n = 0;
            for (int i = 0; i < s_target_count; ++i)
                if (ap_matches_filter(s_targets[i])) idx[n++] = i;

            if (n > 0 && s_target_sel < n) {
                const APRow &a = s_targets[idx[s_target_sel]];
                if (k == 'd' || k == 'D') {
                    send_attack_deauth(a.bssid, 64);
                    strncpy(s_attack_mode, "deauth", sizeof(s_attack_mode));
                    prev = MS_ATTACK; screen = MS_LIVE; last_draw = 0;
                }
                else if (k == 'h' || k == 'H') {
                    send_attack_handshake(a.bssid, 60);
                    strncpy(s_attack_mode, "handshake", sizeof(s_attack_mode));
                    prev = MS_ATTACK; screen = MS_LIVE; last_draw = 0;
                }
                else if (k == 'p' || k == 'P') {
                    send_attack_pmkid(a.bssid, 30);
                    strncpy(s_attack_mode, "pmkid", sizeof(s_attack_mode));
                    prev = MS_ATTACK; screen = MS_LIVE; last_draw = 0;
                }
                else if (k == 'e' || k == 'E') {
                    send_attack_evil_twin(a.ssid, a.channel);
                    strncpy(s_attack_mode, "evil_twin", sizeof(s_attack_mode));
                    prev = MS_ATTACK; screen = MS_LIVE; last_draw = 0;
                }
                else if (k == 'b' || k == 'B') {
                    send_attack_beacon_spam(a.ssid, a.channel);
                    strncpy(s_attack_mode, "beacon", sizeof(s_attack_mode));
                    prev = MS_ATTACK; screen = MS_LIVE; last_draw = 0;
                }
            }
            break;
        }

        case MS_LIVE:
            if (k == 'x' || k == 'X') {
                send_stop();
                snprintf(s_live_msg, sizeof(s_live_msg), "Stop sent");
                s_live_color = T_WARN;
            }
            break;

        case MS_STATUS:
            /* No extra keys — just auto-refresh. */
            break;
        }

        /* Throttled redraw. */
        uint32_t now = millis();
        if (now - last_draw > 100) {
            switch (screen) {
            case MS_MAIN:    draw_main();          break;
            case MS_TARGETS: draw_targets();       break;
            case MS_ATTACK:  draw_attack();        break;
            case MS_LIVE:    draw_live();           break;
            case MS_STATUS:  draw_status_screen(); break;
            }
            ui_draw_status("mimir", s_connected ? "ok" : "---");
            last_draw = now;
        }

        /* Auto-request status every 2s (except on live screen where events
         * already stream in). */
        if (screen != MS_LIVE && now - last_status > 2000) {
            send_status();
            last_status = now;
        }

        yield();  /* feed watchdog without blocking — delay(5) drops CDC frames */
    }

    g_mimir_cdc_active = false;
}
