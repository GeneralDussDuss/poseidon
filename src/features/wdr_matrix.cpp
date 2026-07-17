/*
 * wdr_matrix — see wdr_matrix.h. Ported from the validated 240x135 render
 * sim: rain density/trail, 55ms/char decode, 3 concurrent lanes, roster 5.
 */
#include "app.h"
#include "../theme.h"
#include "wdr_matrix.h"
#include <esp_wifi.h>
#include <esp_random.h>
#include <string.h>

#define MXW      240
#define MXH      135
#define MXCW     6
#define MXCH     8
#define MX_COLS  (MXW / MXCW)     /* 40 */
#define MX_ROWS  (MXH / MXCH)     /* 16 */
#define MX_TRAIL 4
#define MX_MAXDEC 3
#define MX_ROSTER 5
#define MX_PEND   12
#define MX_DEC_MS  55
#define MX_HOLD_MS 240

static const int MX_LANE_Y[MX_MAXDEC] = { 80, 96, 112 };

/* ---- rain ---- */
static int      mxr_head[MX_COLS];
static uint8_t  mxr_spd[MX_COLS];
static bool     mxr_on[MX_COLS];
static bool     mxr_init = false;
static uint32_t mxr_last_rain = 0;

/* ---- decode lanes ---- */
struct mx_decode_t {
    char     name[40];
    uint8_t  auth;
    int8_t   rssi;
    int      x;
    int      resolved;
    uint32_t t_res;
    uint8_t  phase;     /* 0 = resolve, 1 = hold */
    uint32_t hold_t;
    bool     used;
};
static mx_decode_t mxr_dec[MX_MAXDEC];
static bool         mxr_lane_busy[MX_MAXDEC];

/* ---- pending queue (ring) ---- */
struct mx_pend_t { char name[40]; uint8_t auth; int8_t rssi; };
static mx_pend_t mxr_pend[MX_PEND];
static int mxr_pend_head = 0, mxr_pend_tail = 0, mxr_pend_n = 0;

/* ---- roster ---- */
struct mx_rost_t { char name[40]; uint8_t auth; int8_t rssi; };
static mx_rost_t mxr_roster[MX_ROSTER];
static int mxr_rost_n = 0;

/* ---- catch banner ---- */
static uint32_t mxr_note_until = 0;
static char     mxr_note_tag[10];
static char     mxr_note_ssid[40];
static uint16_t mxr_note_col;

static inline char mx_glyph(void) { return (char)(0x21 + (esp_random() % 0x5D)); }

static const char *mx_auth_label(uint8_t a)
{
    switch (a) {
    case WIFI_AUTH_OPEN:         return "OPEN";
    case WIFI_AUTH_WEP:          return "WEP";
    case WIFI_AUTH_WPA3_PSK:     return "WPA3";
    case WIFI_AUTH_WPA_PSK:      return "WPA";
    default:                     return "WPA2";
    }
}
static uint16_t mx_auth_color(uint8_t a)
{
    switch (a) {
    case WIFI_AUTH_OPEN:         return T_WARN;
    case WIFI_AUTH_WEP:          return T_BAD;
    case WIFI_AUTH_WPA3_PSK:     return T_ACCENT2;
    default:                     return T_ACCENT;
    }
}

static void mx_init_rain(void)
{
    for (int c = 0; c < MX_COLS; ++c) {
        mxr_head[c] = -(int)(esp_random() % MX_ROWS);
        mxr_spd[c]  = 1 + (esp_random() & 1);
        mxr_on[c]   = (esp_random() % 100) < 70;
    }
    mxr_init = true;
    mxr_last_rain = millis();
}

static void mx_push_roster(const char *name, uint8_t auth, int8_t rssi)
{
    for (int i = MX_ROSTER - 1; i > 0; --i) mxr_roster[i] = mxr_roster[i - 1];
    strncpy(mxr_roster[0].name, name, sizeof(mxr_roster[0].name) - 1);
    mxr_roster[0].name[sizeof(mxr_roster[0].name) - 1] = 0;
    mxr_roster[0].auth = auth;
    mxr_roster[0].rssi = rssi;
    if (mxr_rost_n < MX_ROSTER) mxr_rost_n++;
}

void wdr_matrix_seed(const char *ssid, uint8_t auth, int8_t rssi)
{
    mx_push_roster(ssid, auth, rssi);
}

void wdr_matrix_begin(void)
{
    mxr_rost_n = 0;
    mxr_pend_head = mxr_pend_tail = mxr_pend_n = 0;
    for (int l = 0; l < MX_MAXDEC; ++l) { mxr_dec[l].used = false; mxr_lane_busy[l] = false; }
    mxr_note_until = 0;
    mx_init_rain();
}

void wdr_matrix_feed(const char *ssid, uint8_t auth, int8_t rssi)
{
    if (mxr_pend_n < MX_PEND) {
        mx_pend_t &p = mxr_pend[mxr_pend_tail];
        strncpy(p.name, ssid, sizeof(p.name) - 1); p.name[sizeof(p.name) - 1] = 0;
        p.auth = auth; p.rssi = rssi;
        mxr_pend_tail = (mxr_pend_tail + 1) % MX_PEND;
        mxr_pend_n++;
    }
    if (auth == WIFI_AUTH_OPEN || auth == WIFI_AUTH_WPA3_PSK) {
        bool open = (auth == WIFI_AUTH_OPEN);
        strncpy(mxr_note_tag, open ? "OPEN NET" : "WPA3 SAE", sizeof(mxr_note_tag) - 1);
        mxr_note_tag[sizeof(mxr_note_tag) - 1] = 0;
        strncpy(mxr_note_ssid, ssid, sizeof(mxr_note_ssid) - 1);
        mxr_note_ssid[sizeof(mxr_note_ssid) - 1] = 0;
        mxr_note_col   = open ? T_WARN : T_ACCENT2;
        mxr_note_until = millis() + 1600;
    }
}

static void mx_dispatch(uint32_t now)
{
    for (int l = 0; l < MX_MAXDEC; ++l) {
        if (mxr_lane_busy[l] || mxr_pend_n <= 0) continue;
        mx_pend_t &p = mxr_pend[mxr_pend_head];
        mxr_pend_head = (mxr_pend_head + 1) % MX_PEND;
        mxr_pend_n--;
        mx_decode_t &dc = mxr_dec[l];
        strncpy(dc.name, p.name, sizeof(dc.name) - 1); dc.name[sizeof(dc.name) - 1] = 0;
        dc.auth = p.auth; dc.rssi = p.rssi;
        int w = (int)strlen(dc.name) * MXCW;
        int maxx = MXW - w - 2; if (maxx < 2) maxx = 2;
        dc.x = 2 + (int)(esp_random() % (uint32_t)(maxx));
        dc.resolved = 0; dc.t_res = now; dc.phase = 0; dc.hold_t = 0; dc.used = true;
        mxr_lane_busy[l] = true;
    }
}

static void mx_draw_banner(uint32_t now)
{
    auto &d = M5Cardputer.Display;
    const int by = 46, bh = 36;
    d.fillRect(0, by, MXW, bh, T_BG);
    bool blink = (now / 120) & 1;
    d.drawRect(0, by, MXW, bh, blink ? 0xFFFF : mxr_note_col);
    d.drawRect(2, by + 2, MXW - 4, bh - 4, mxr_note_col);
    d.setTextColor(mxr_note_col, T_BG);
    d.setTextSize(2);
    int tw = (int)strlen(mxr_note_tag) * 12;
    d.setCursor((MXW - tw) / 2, by + 4); d.print(mxr_note_tag);
    d.setTextSize(1);
    int sw = (int)strlen(mxr_note_ssid) * MXCW;
    if (sw > MXW) sw = MXW;
    d.setCursor((MXW - sw) / 2, by + 24); d.print(mxr_note_ssid);
}

void wdr_matrix_render(uint8_t chan, int ap_count, bool gps_valid, uint8_t sats)
{
    auto &d = M5Cardputer.Display;
    uint32_t now = millis();
    if (!mxr_init) mx_init_rain();

    d.fillScreen(T_BG);
    d.setTextSize(1);

    /* ---- rain ---- */
    bool adv = (now - mxr_last_rain > 45);
    if (adv) mxr_last_rain = now;
    for (int c = 0; c < MX_COLS; ++c) {
        if (!mxr_on[c]) { if (adv && (esp_random() % 100) < 3) mxr_on[c] = true; else continue; }
        int head = mxr_head[c];
        for (int t = 0; t < MX_TRAIL; ++t) {
            int ty = head - t;
            if (ty < 0 || ty >= MX_ROWS) continue;
            uint16_t col = (t == 0) ? 0xFFFF : (t == 1) ? T_ACCENT : T_DIM;
            d.setTextColor(col, T_BG);
            d.setCursor(c * MXCW, ty * MXCH);
            d.printf("%c", mx_glyph());
        }
        if (adv) {
            mxr_head[c] += mxr_spd[c];
            if (mxr_head[c] >= MX_ROWS + MX_TRAIL) {
                mxr_head[c] = -(int)(esp_random() % 6);
                mxr_spd[c]  = 1 + (esp_random() & 1);
                mxr_on[c]   = (esp_random() % 100) < 70;
            }
        }
    }

    /* ---- decode fly-ins ---- */
    mx_dispatch(now);
    for (int i = 0; i < MX_MAXDEC; ++i) {
        mx_decode_t &dc = mxr_dec[i];
        if (!dc.used) continue;
        int len = (int)strlen(dc.name);
        if (dc.phase == 0) {
            if (now - dc.t_res >= MX_DEC_MS) {
                dc.t_res = now;
                if (++dc.resolved >= len) { dc.phase = 1; dc.hold_t = now; }
            }
        } else if (now - dc.hold_t >= MX_HOLD_MS) {
            mx_push_roster(dc.name, dc.auth, dc.rssi);
            dc.used = false; mxr_lane_busy[i] = false;
            continue;
        }
        for (int k = 0; k < len; ++k) {
            bool done = k < dc.resolved;
            uint16_t col = done ? (dc.phase == 1 ? 0xFFFF : T_FG) : T_ACCENT;
            char ch = done ? dc.name[k] : mx_glyph();
            d.setTextColor(col, T_BG);
            d.setCursor(dc.x + k * MXCW, MX_LANE_Y[i]);
            d.printf("%c", ch);
        }
    }

    /* ---- roster ---- */
    for (int i = 0; i < mxr_rost_n; ++i) {
        int y = 15 + i * 11;
        d.fillRect(0, y - 1, MXW, 10, T_BG);        /* wipe rain behind the row */
        d.setTextColor(i == 0 ? 0xFFFF : T_FG, T_BG);
        char nm[23]; strncpy(nm, mxr_roster[i].name, 22); nm[22] = 0;
        d.setCursor(2, y); d.print(nm);
        int rs = mxr_roster[i].rssi;
        int bars = rs >= -45 ? 5 : rs >= -58 ? 4 : rs >= -70 ? 3 : rs >= -82 ? 2 : 1;
        for (int b = 0; b < 5; ++b)
            d.fillRect(150 + b * 5, y, 3, 7, b < bars ? T_ACCENT : T_SEL_BG);
        d.setTextColor(mx_auth_color(mxr_roster[i].auth), T_BG);
        d.setCursor(184, y); d.print(mx_auth_label(mxr_roster[i].auth));
    }

    /* ---- HUD ---- */
    d.fillRect(0, 0, MXW, 13, T_BG);
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(2, 3); d.print("POSEIDON//WARDRIVE");
    char hud[24]; snprintf(hud, sizeof(hud), "ch:%u APs:%d", chan, ap_count);
    d.setTextColor(T_DIM, T_BG);
    d.setCursor(MXW - (int)strlen(hud) * MXCW - 12, 3); d.print(hud);
    d.setTextColor(gps_valid ? T_GOOD : T_DIM, T_BG);
    d.setCursor(MXW - 11, 3); d.print(gps_valid ? "*" : ".");
    d.drawFastHLine(0, 12, MXW, T_ACCENT2);

    /* ---- footer ---- */
    d.fillRect(0, MXH - 10, MXW, 10, T_BG);
    d.setTextColor(T_DIM, T_BG);
    d.setCursor(2, MXH - 9); d.print("// A=view F=flush ESC=stop");

    /* ---- catch banner (on top) ---- */
    if (now < mxr_note_until) mx_draw_banner(now);
    (void)sats;
}
