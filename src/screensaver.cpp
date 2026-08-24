/*
 * screensaver.cpp — pool of theme-tinting full-screen idle takeovers.
 *
 * Each painter owns the entire screen for the duration of its run. We
 * don't try to preserve the prior status / footer / menu state — when
 * the screensaver returns, the caller (menu loop) is expected to repaint
 * its UI from scratch.
 *
 * Pool architecture: all painters draw with theme palette macros
 * (T_BG / T_FG / T_ACCENT / T_ACCENT2 / T_DIM / T_GOOD / T_WARN / T_BAD)
 * so they auto-tint to whichever theme is active. screensaver_check_idle
 * picks from the pool — random-exclude-last when SCREENSAVER_PICK_SHUFFLE,
 * otherwise the user's locked index.
 *
 * State invariants:
 *   - Per-painter state is function-static so each entry feels fresh
 *     even if a previous run was interrupted partway through.
 *   - No PSRAM, no malloc in the render loop, no IRAM_ATTR.
 *   - Each painter polls input_poll() every frame and returns the moment
 *     a real key arrives.
 */
#include "screensaver.h"
#include "theme.h"
#include "app.h"
#include "ui.h"
#include "input.h"
#include <M5Cardputer.h>
#include <Preferences.h>
#include <esp_random.h>
#include <math.h>
#include <string.h>
#include <stdio.h>

extern void ui_matrix_rain(int x, int y, int w, int h, uint16_t color);

/* ---- 5-6-5 alpha blend (used for sonar fade, glitch chromatic split). */
static uint16_t blend565(uint16_t a, uint16_t b, uint8_t t)
{
    uint16_t ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    uint16_t br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
    uint16_t rr = (ar * (255 - t) + br * t) / 255;
    uint16_t rg = (ag * (255 - t) + bg * t) / 255;
    uint16_t rb = (ab * (255 - t) + bb * t) / 255;
    return (rr << 11) | (rg << 5) | rb;
}

/* ---- NVS-backed config ---- */
static bool     s_enabled        = true;
static uint32_t s_timeout_ms     = 120000;   /* 2 minutes default */
static int8_t   s_pick           = SCREENSAVER_PICK_SHUFFLE;
static int8_t   s_last           = -1;       /* last-actually-run index */
static bool     s_loaded         = false;

static void load_settings(void)
{
    if (s_loaded) return;
    s_loaded = true;
    Preferences p;
    if (p.begin("pscr", true)) {
        s_enabled    = p.getBool ("enabled", true);
        s_timeout_ms = p.getUInt ("timeoutms", 120000);
        s_pick       = (int8_t)p.getChar("pick", SCREENSAVER_PICK_SHUFFLE);
        s_last       = (int8_t)p.getChar("last", -1);
        p.end();
    }
}

bool screensaver_enabled(void) { load_settings(); return s_enabled; }

void screensaver_enabled_set(bool on)
{
    load_settings();
    s_enabled = on;
    Preferences p;
    if (p.begin("pscr", false)) { p.putBool("enabled", on); p.end(); }
}

uint32_t screensaver_timeout_ms(void) { load_settings(); return s_timeout_ms; }

void screensaver_timeout_set(uint32_t ms)
{
    load_settings();
    if (ms < 10000)    ms = 10000;
    if (ms > 3600000)  ms = 3600000;
    s_timeout_ms = ms;
    Preferences p;
    if (p.begin("pscr", false)) { p.putUInt("timeoutms", ms); p.end(); }
}

static void save_last(int idx)
{
    s_last = (int8_t)idx;
    Preferences p;
    if (p.begin("pscr", false)) { p.putChar("last", (char)idx); p.end(); }
}

/* ============ POSEIDON: WARDRIVE.cinema ============ */

#define WD_AP_SLOTS      9
#define WD_SSID_MAX      19
#define WD_SSID_POOL     36

static const char *const WD_SSID_POOL_TBL[WD_SSID_POOL] = {
    "Linksys_2G_Net",       "ATT_Home_5G",          "Verizon_FiOS",
    "TPLink_Guest",         "Xfinity_WiFi",         "Spectrum_AP_88",
    "FBI_Surveillance",     "Pretty_Fly_4_WiFi",    "skynet",
    "Network_Not_Found",    "DEFCON-Open",          "PineappleAP",
    "FreeWiFi_DontUse",     "Apartment_4B",         "Comcast_2.4",
    "GalaxyS22_Hotspot",    "Pixel_Hotspot",        "iPhone_of_dave",
    "myFi",                 "GUEST",                "Starbucks-WPA",
    "McDonalds_Free",       "Marriott_Lobby",       "AirportFree",
    "PrintServer_42",       "WD_NetworkDrive",      "RING_Cam",
    "Ecobee_42BC",          "Lutron_HUB",           "NOT_A_HONEYPOT",
    "vanlife_192_168",      "wifi_was_here",        "tell_my_wifi",
    "mom_use_this",         "FBI_VAN_99",           "iotsec_fail",
};

struct wd_ap_t {
    char     ssid[WD_SSID_MAX + 1];
    uint8_t  bssid[6];
    int8_t   rssi;
    uint8_t  channel;
    uint8_t  auth;
    bool     captured;
    uint32_t born_ms;
};

static wd_ap_t   wd_aps[WD_AP_SLOTS];
static int       wd_count        = 0;
static uint32_t  wd_next_spawn   = 0;
static uint32_t  wd_next_chanhop = 0;
static uint32_t  wd_next_capture = 0;
static uint32_t  wd_next_streak  = 0;
static uint8_t   wd_chan         = 1;
static uint16_t  wd_total_seen   = 0;
static uint32_t  wd_streak_start = 0;
static int       wd_streak_y     = 0;
static int       wd_streak_dir   = 0;

static const char *wd_auth_label(uint8_t a)
{
    switch (a) {
    case 0: return "WPA3";
    case 1: return "WPA2";
    case 2: return "W23";
    case 3: return "OPEN";
    case 4: return "WEP ";
    default: return "M1*";
    }
}
static uint16_t wd_auth_color(uint8_t a)
{
    switch (a) {
    case 0: return T_ACCENT2;
    case 1: return T_ACCENT;
    case 2: return T_GOOD;
    case 3: return T_WARN;
    case 4: return T_BAD;
    default: return T_ACCENT2;
    }
}
static void wd_spawn_ap(void)
{
    for (int i = WD_AP_SLOTS - 1; i > 0; --i) wd_aps[i] = wd_aps[i - 1];
    wd_ap_t &a = wd_aps[0];
    const char *src = WD_SSID_POOL_TBL[esp_random() % WD_SSID_POOL];
    strncpy(a.ssid, src, WD_SSID_MAX);
    a.ssid[WD_SSID_MAX] = 0;
    for (int i = 0; i < 6; ++i) a.bssid[i] = (uint8_t)(esp_random() & 0xFF);
    a.rssi = -30 - (int8_t)(esp_random() % 60);
    if ((esp_random() & 0xFF) < 0xCC) {
        a.channel = 1 + (uint8_t)(esp_random() % 13);
    } else {
        static const uint8_t fiveG[] = { 36, 40, 44, 48, 149, 153, 157, 161 };
        a.channel = fiveG[esp_random() % (sizeof(fiveG))];
    }
    a.auth = (uint8_t)(esp_random() % 5);
    a.captured = false;
    a.born_ms = millis();
    if (wd_count < WD_AP_SLOTS) wd_count++;
    wd_total_seen++;
}
static void wd_render(void)
{
    auto &d = M5Cardputer.Display;
    d.fillScreen(T_BG);
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(2, 2); d.print("WARDRIVE.cinema");
    char hud[24];
    snprintf(hud, sizeof(hud), "ch:%-3u APs:%u", wd_chan, wd_total_seen);
    d.setTextColor(T_ACCENT2, T_BG);
    int hw = d.textWidth(hud);
    d.setCursor(SCR_W - hw - 2, 2); d.print(hud);
    d.drawFastHLine(0, 11, SCR_W, T_ACCENT2);
    d.drawFastHLine(0, 12, SCR_W, T_ACCENT2);

    int row_h = 11;
    for (int i = 0; i < wd_count; ++i) {
        const wd_ap_t &a = wd_aps[i];
        int y = 16 + i * row_h;
        if (y + row_h > SCR_H - 11) break;
        char ssidbuf[15];
        strncpy(ssidbuf, a.ssid, 14); ssidbuf[14] = 0;
        d.setTextColor(a.captured ? T_BAD : T_FG, T_BG);
        d.setCursor(2, y); d.print(ssidbuf);
        int bx = 100, by = y + 1;
        int strength = a.rssi >= -40 ? 5 : a.rssi >= -55 ? 4 : a.rssi >= -70 ? 3
                     : a.rssi >= -80 ? 2 : a.rssi >= -90 ? 1 : 0;
        for (int s = 0; s < 5; ++s) {
            int sx = bx + s * 6;
            uint16_t fc = s < strength ? (a.captured ? T_BAD : T_ACCENT) : T_DIM;
            d.fillRect(sx, by, 4, 7, fc);
        }
        d.setTextColor(T_DIM, T_BG);
        char rs[6]; snprintf(rs, sizeof(rs), "%-3d", a.rssi);
        d.setCursor(135, y); d.print(rs);
        d.setTextColor(T_ACCENT, T_BG);
        char chs[6]; snprintf(chs, sizeof(chs), "%-3u", a.channel);
        d.setCursor(160, y); d.print(chs);
        const char *al = a.captured ? "PMKID" : wd_auth_label(a.auth);
        uint16_t   ac = a.captured ? T_BAD : wd_auth_color(a.auth);
        d.fillRect(SCR_W - 30, y, 28, 9, T_SEL_BG);
        d.drawRect(SCR_W - 30, y, 28, 9, ac);
        d.setTextColor(ac, T_SEL_BG);
        d.setCursor(SCR_W - 28, y + 1); d.print(al);
    }
    if (wd_streak_start) {
        uint32_t elapsed = millis() - wd_streak_start;
        if (elapsed < 800) {
            int sx = (int)((int64_t)elapsed * (SCR_W + 40) / 800) - 20;
            if (wd_streak_dir < 0) sx = SCR_W - sx;
            d.fillRect(sx - 8, wd_streak_y, 16, 2, T_ACCENT2);
            d.fillRect(sx - 4, wd_streak_y - 1, 8, 4, T_ACCENT2);
        } else wd_streak_start = 0;
    }
    d.fillRect(0, SCR_H - 11, SCR_W, 11, T_BG);
    d.drawFastHLine(0, SCR_H - 11, SCR_W, T_ACCENT2);
    d.setTextColor(T_DIM, T_BG);
    d.setCursor(2, SCR_H - 9); d.print("// any key to wake");
    d.setTextColor(T_ACCENT, T_BG);
    char up[16];
    uint32_t s = millis() / 1000;
    snprintf(up, sizeof(up), "uptime %lu:%02lu",
             (unsigned long)(s / 60), (unsigned long)(s % 60));
    int upw = d.textWidth(up);
    d.setCursor(SCR_W - upw - 2, SCR_H - 9); d.print(up);
}
static void wd_tick(uint32_t now)
{
    if (now >= wd_next_spawn) {
        wd_spawn_ap();
        wd_next_spawn = now + 500 + (esp_random() % 400);
    }
    if (now >= wd_next_chanhop) {
        if ((esp_random() & 0xFF) < 0xCC) {
            wd_chan = 1 + (uint8_t)(esp_random() % 13);
        } else {
            static const uint8_t fg[] = { 36, 40, 44, 48, 149, 153, 157, 161 };
            wd_chan = fg[esp_random() % sizeof(fg)];
        }
        wd_next_chanhop = now + 180 + (esp_random() % 220);
    }
    if (now >= wd_next_capture && wd_count > 0) {
        int idx = esp_random() % wd_count;
        wd_aps[idx].captured = true;
        wd_next_capture = now + 3500 + (esp_random() % 4000);
    }
    if (now >= wd_next_streak && wd_streak_start == 0) {
        wd_streak_start = now;
        wd_streak_y     = 18 + (int)(esp_random() % (SCR_H - 30));
        wd_streak_dir   = (esp_random() & 1) ? +1 : -1;
        wd_next_streak  = now + 1500 + (esp_random() % 2500);
    }
}
static void run_wardrive_cinema(void)
{
    wd_count = 0; wd_total_seen = 0; wd_chan = 1; wd_streak_start = 0;
    wd_next_spawn   = millis();
    wd_next_chanhop = millis();
    wd_next_capture = millis() + 4000;
    wd_next_streak  = millis() + 2000;
    for (int i = 0; i < 4; ++i) wd_spawn_ap();
    while (input_poll() == PK_NONE) {
        wd_tick(millis());
        wd_render();
        delay(45);
    }
}

/* ============ MATRIX RAIN ============ */

static void run_matrix_rain(void)
{
    auto &d = M5Cardputer.Display;
    d.fillScreen(T_BG);
    while (input_poll() == PK_NONE) {
        ui_matrix_rain(0, 0, SCR_W, SCR_H, T_FG);
        delay(33);
    }
}

/* ============ BREATHING wordmark ============ */

static int eink_drift_x = 0;
static int eink_drift_y = 0;

static void run_eink_breathing(void)
{
    auto &d = M5Cardputer.Display;
    const uint32_t T_FADE_IN  = 8000;
    const uint32_t T_HOLD     = 3000;
    const uint32_t T_FADE_OUT = 8000;
    const uint32_t T_BLANK    = 3000;
    const uint32_t T_CYCLE    = T_FADE_IN + T_HOLD + T_FADE_OUT + T_BLANK;

    uint32_t cycle_start = millis();
    eink_drift_x = -25 + (int)(esp_random() % 50);
    eink_drift_y = -10 + (int)(esp_random() % 20);

    while (input_poll() == PK_NONE) {
        uint32_t now = millis();
        uint32_t phase = (now - cycle_start) % T_CYCLE;
        uint8_t brightness;
        if      (phase < T_FADE_IN)               brightness = (uint8_t)((phase * 255u) / T_FADE_IN);
        else if (phase < T_FADE_IN + T_HOLD)      brightness = 255;
        else if (phase < T_FADE_IN + T_HOLD + T_FADE_OUT) {
            uint32_t fp = phase - T_FADE_IN - T_HOLD;
            brightness = (uint8_t)(255u - ((fp * 255u) / T_FADE_OUT));
        } else                                    brightness = 0;

        if (phase < 100 && now > cycle_start + T_CYCLE - 100) {
            cycle_start = now;
            eink_drift_x = -25 + (int)(esp_random() % 50);
            eink_drift_y = -10 + (int)(esp_random() % 20);
        }
        uint16_t fade = blend565(T_BG, T_FG, brightness);
        d.fillScreen(T_BG);
        d.setTextSize(3);
        d.setTextColor(fade, T_BG);
        int tx = (SCR_W / 2) - 72 + eink_drift_x;
        int ty = (SCR_H / 2) - 12 + eink_drift_y;
        d.setCursor(tx, ty); d.print("POSEIDON");
        d.setTextSize(1);
        int tide_w = (int)((brightness * (SCR_W - 40)) / 255);
        d.drawFastHLine((SCR_W - tide_w) / 2, SCR_H - 8, tide_w, fade);
        d.setTextColor(T_DIM, T_BG);
        d.setCursor(2, SCR_H - 9); d.print("any key");
        delay(60);
    }
}

/* ============ DEEP SCAN — sonar pulse + named contacts ============ */

struct ds_contact_t {
    int      angle;   /* 0..359 */
    int      dist;    /* 15..70 */
    uint32_t born_ms;
    char     name[12];
};
static const char *const DS_NAMES[] = {
    "CARGO-43",   "USS-PYTHIA", "UNKNOWN-01", "FERRY-N",
    "FISHING-7B", "GHOST-ECHO", "VESSEL-117", "RADAR-XK",
    "POSEIDON-A", "MIMIR-DROP", "TRIDENT-1",  "DEFCON-7",
};

static void run_deep_scan(void)
{
    auto &d = M5Cardputer.Display;
    int cx = SCR_W / 2, cy = SCR_H / 2 + 4;
    int max_r = (SCR_W < SCR_H ? SCR_W : SCR_H) / 2 + 10;

    static ds_contact_t contacts[6];
    int contact_count = 0;
    uint32_t next_contact = millis() + 600;
    uint32_t pulse0 = millis();
    const uint32_t PULSE_MS = 2200;

    while (input_poll() == PK_NONE) {
        uint32_t now = millis();
        d.fillScreen(T_BG);

        /* Header */
        d.setTextColor(T_ACCENT, T_BG);
        d.setCursor(2, 2); d.print("DEEP SCAN // sonar");
        char hud[16];
        snprintf(hud, sizeof(hud), "ctc:%d", contact_count);
        int hw = d.textWidth(hud);
        d.setTextColor(T_ACCENT2, T_BG);
        d.setCursor(SCR_W - hw - 2, 2); d.print(hud);
        d.drawFastHLine(0, 11, SCR_W, T_DIM);

        /* Three concentric pulse rings, staggered. */
        for (int i = 0; i < 3; ++i) {
            uint32_t age = (now - pulse0 + i * (PULSE_MS / 3)) % PULSE_MS;
            int r = (int)((age * max_r) / PULSE_MS);
            uint8_t a = (uint8_t)(255 - (age * 255 / PULSE_MS));
            uint16_t c = blend565(T_BG, T_ACCENT, a);
            d.drawCircle(cx, cy, r, c);
        }

        /* Crosshair + center beacon */
        d.drawFastHLine(cx - 4, cy, 9, T_DIM);
        d.drawFastVLine(cx, cy - 4, 9, T_DIM);
        d.fillCircle(cx, cy, 3, T_ACCENT2);
        d.drawCircle(cx, cy, 5, T_ACCENT);

        /* Spawn contacts */
        if (now >= next_contact && contact_count < 6) {
            ds_contact_t &c = contacts[contact_count++];
            c.angle   = esp_random() % 360;
            c.dist    = 18 + (int)(esp_random() % 45);
            c.born_ms = now;
            const char *src = DS_NAMES[esp_random() % (sizeof(DS_NAMES) / sizeof(DS_NAMES[0]))];
            strncpy(c.name, src, 11); c.name[11] = 0;
            next_contact = now + 1200 + (esp_random() % 2000);
        }

        /* Render + age contacts (5 s lifespan) */
        for (int i = 0; i < contact_count; ) {
            uint32_t age = now - contacts[i].born_ms;
            if (age > 5000) {
                for (int j = i; j < contact_count - 1; ++j) contacts[j] = contacts[j + 1];
                contact_count--;
                continue;
            }
            float rad = contacts[i].angle * (float)M_PI / 180.0f;
            int x = cx + (int)(cos(rad) * contacts[i].dist);
            int y = cy + (int)(sin(rad) * contacts[i].dist);
            uint8_t a = (age < 4000) ? 255 : (uint8_t)(255 - ((age - 4000) * 255 / 1000));
            uint16_t bc = blend565(T_BG, T_ACCENT2, a);
            d.fillCircle(x, y, 2, bc);
            d.drawCircle(x, y, 4, bc);
            d.setTextColor(blend565(T_BG, T_FG, a), T_BG);
            int lx = x + 6;
            if (lx > SCR_W - 50) lx = x - d.textWidth(contacts[i].name) - 6;
            int ly = y - 3;
            if (ly < 14) ly = y + 6;
            d.setCursor(lx, ly); d.print(contacts[i].name);
            ++i;
        }

        /* Footer */
        d.setTextColor(T_DIM, T_BG);
        d.setCursor(2, SCR_H - 9); d.print("// any key to wake");
        delay(50);
    }
}

/* ============ PORT SCAN — vertical port grid + nmap-like output ============ */

struct ps_hit_t {
    uint16_t port;
    uint8_t  state;   /* 0 closed/dim, 1 open, 2 filtered */
    uint32_t born_ms;
};
static const char *const PS_SVCS[] = {
    "ssh", "http", "https", "telnet", "ftp", "smb", "rdp", "vnc",
    "mysql", "redis", "smtp", "dns", "ntp", "snmp", "ldap", "rtsp",
};
static const uint16_t PS_OPEN_PORTS[] = {
    21, 22, 23, 25, 53, 80, 110, 143, 389, 443, 445, 502, 554,
    993, 1433, 1521, 3306, 3389, 5432, 5900, 6379, 8080, 8443,
};

static void run_port_scan(void)
{
    auto &d = M5Cardputer.Display;
    /* Build a fake target IP that looks plausible. */
    static char target[18];
    snprintf(target, sizeof(target), "10.%u.%u.%u",
             (unsigned)(esp_random() % 255),
             (unsigned)(esp_random() % 255),
             1 + (unsigned)(esp_random() % 254));

    static ps_hit_t hits[8];
    int hit_count = 0;
    uint32_t cursor_port = 0;
    const uint32_t SCAN_RATE = 35;  /* ports/frame */

    while (input_poll() == PK_NONE) {
        uint32_t now = millis();
        d.fillScreen(T_BG);

        /* Header */
        d.setTextColor(T_ACCENT, T_BG);
        d.setCursor(2, 2); d.print("PORT SCAN //");
        d.setTextColor(T_ACCENT2, T_BG);
        d.setCursor(70, 2); d.print(target);
        d.setTextColor(T_DIM, T_BG);
        char prog[20];
        snprintf(prog, sizeof(prog), "%5u/65535", (unsigned)cursor_port);
        int pw = d.textWidth(prog);
        d.setCursor(SCR_W - pw - 2, 2); d.print(prog);
        d.drawFastHLine(0, 11, SCR_W, T_DIM);

        /* Vertical port column visualizer — split screen into 64 cells, each cell = 1024 ports.
         * Draw the cells as a stack of ticks; the cursor cell is highlighted. */
        const int col_x   = 2;
        const int col_y   = 14;
        const int col_h   = SCR_H - 30;
        const int n_cells = 64;
        int cell_h = col_h / n_cells; if (cell_h < 1) cell_h = 1;
        for (int c = 0; c < n_cells; ++c) {
            int y = col_y + c * cell_h;
            uint16_t cc = T_DIM;
            int cell_port_lo = c * 1024;
            int cell_port_hi = cell_port_lo + 1024;
            if ((int)cursor_port >= cell_port_lo && (int)cursor_port < cell_port_hi) cc = T_ACCENT;
            d.drawFastHLine(col_x, y, 12, cc);
        }
        /* Beam cursor */
        int cur_y = col_y + (int)((cursor_port * (uint32_t)col_h) / 65536u);
        d.fillRect(col_x - 1, cur_y - 1, 16, 3, T_ACCENT2);

        /* Hit log on the right */
        d.setTextColor(T_ACCENT, T_BG);
        d.setCursor(28, 14); d.print("RESULTS");
        d.drawFastHLine(28, 23, SCR_W - 30, T_DIM);
        for (int i = 0; i < hit_count; ++i) {
            int y = 26 + i * 11;
            if (y > SCR_H - 14) break;
            uint16_t state_c = (hits[i].state == 1) ? T_GOOD :
                               (hits[i].state == 2) ? T_WARN : T_DIM;
            const char *state_s = (hits[i].state == 1) ? "OPEN " :
                                  (hits[i].state == 2) ? "FILT " : "CLOSE";
            d.setTextColor(state_c, T_BG);
            d.setCursor(28, y); d.print(state_s);
            d.setTextColor(T_FG, T_BG);
            char pbuf[12];
            snprintf(pbuf, sizeof(pbuf), "%5u", hits[i].port);
            d.setCursor(60, y); d.print(pbuf);
            d.setTextColor(T_ACCENT2, T_BG);
            const char *svc;
            if (hits[i].port == 22)   svc = "ssh";
            else if (hits[i].port == 80)   svc = "http";
            else if (hits[i].port == 443)  svc = "https";
            else if (hits[i].port == 23)   svc = "telnet";
            else if (hits[i].port == 21)   svc = "ftp";
            else if (hits[i].port == 445)  svc = "smb";
            else if (hits[i].port == 3389) svc = "rdp";
            else if (hits[i].port == 3306) svc = "mysql";
            else if (hits[i].port == 5900) svc = "vnc";
            else                            svc = PS_SVCS[hits[i].port % (sizeof(PS_SVCS)/sizeof(PS_SVCS[0]))];
            d.setCursor(98, y); d.print(svc);
        }

        /* Footer */
        d.setTextColor(T_DIM, T_BG);
        d.setCursor(2, SCR_H - 9); d.print("// nmap -sS -p- -- any key wakes");

        /* Advance scan cursor */
        cursor_port += SCAN_RATE;
        if (cursor_port >= 65535) cursor_port = 0;

        /* Probabilistically drop hits — bias toward known open ports. */
        if ((esp_random() & 0xFF) < 0x10) {
            ps_hit_t h;
            if ((esp_random() & 0x3) == 0) {
                h.port  = PS_OPEN_PORTS[esp_random() % (sizeof(PS_OPEN_PORTS)/sizeof(PS_OPEN_PORTS[0]))];
                h.state = 1;
            } else {
                h.port  = (uint16_t)cursor_port;
                h.state = (esp_random() & 1) ? 2 : 0;
            }
            h.born_ms = now;
            for (int i = 7; i > 0; --i) hits[i] = hits[i-1];
            hits[0] = h;
            if (hit_count < 8) hit_count++;
        }
        delay(45);
    }
}

/* ============ HEX CASCADE — falling hex bytes with decoded reveals ============ */

#define HC_COLS  30
#define HC_ROWS  17
struct hc_col_t {
    int      y;          /* head row */
    uint8_t  speed;      /* rows per tick (1..3) */
    bool     decoding;
    uint32_t decode_until;
    char     decode_text[12];
    uint8_t  decode_pos;
};
static const char *const HC_REVEALS[] = {
    "POSEIDON", "DEADBEEF", "PWNED",  "ROOT",      "EXPLOIT",
    "0xCAFE",   "PHANTOM",  "MIMIR",  "TRIDENT",   "BREACH",
    "PMKID",    "SHELL",    "0xBADC0DE", "ACCESS", "OWNED",
};

static void run_hex_cascade(void)
{
    auto &d = M5Cardputer.Display;
    d.fillScreen(T_BG);
    static hc_col_t cols[HC_COLS];
    for (int c = 0; c < HC_COLS; ++c) {
        cols[c].y = -(int)(esp_random() % HC_ROWS);
        cols[c].speed = 1 + (esp_random() & 1);
        cols[c].decoding = false;
        cols[c].decode_pos = 0;
    }
    const int CELL_W = 8;
    const int CELL_H = 8;

    while (input_poll() == PK_NONE) {
        uint32_t now = millis();
        /* Mostly black bg with slight fade-trail effect — fillScreen each tick
         * is cheap; trail is implicit via past glyph erasure. */
        d.fillScreen(T_BG);

        for (int c = 0; c < HC_COLS; ++c) {
            hc_col_t &col = cols[c];
            int x = c * CELL_W;

            /* Tail glyphs above head */
            for (int t = 0; t < HC_ROWS; ++t) {
                int row = col.y - t;
                if (row < 0 || row >= HC_ROWS) continue;
                int y = row * CELL_H + 2;
                /* Random hex char */
                uint8_t b = (uint8_t)(esp_random() & 0xFF);
                char hb[3]; snprintf(hb, sizeof(hb), "%02X", b);
                uint16_t cc = (t == 0) ? T_FG :
                              (t < 3) ? T_ACCENT :
                              (t < 6) ? T_DIM : T_BG;
                if (cc == T_BG) continue;
                if (col.decoding && t < (int)strlen(col.decode_text)) {
                    /* Replace with reveal text char in T_ACCENT2 */
                    char ch[2] = { col.decode_text[t], 0 };
                    d.setTextColor(T_ACCENT2, T_BG);
                    d.setCursor(x, y); d.print(ch);
                } else {
                    d.setTextColor(cc, T_BG);
                    d.setCursor(x, y); d.print(hb);
                }
            }
            /* Advance head */
            col.y += col.speed;
            if (col.y > HC_ROWS + 2) {
                col.y = -(int)(esp_random() % 4);
                col.speed = 1 + (esp_random() & 1);
                col.decoding = false;
            }
            /* Trigger decode on a column at random */
            if (!col.decoding && (esp_random() & 0x7FF) == 0) {
                const char *src = HC_REVEALS[esp_random() % (sizeof(HC_REVEALS)/sizeof(HC_REVEALS[0]))];
                strncpy(col.decode_text, src, 11);
                col.decode_text[11] = 0;
                col.decoding = true;
                col.decode_until = now + 1500;
            }
            if (col.decoding && now > col.decode_until) col.decoding = false;
        }

        /* Header overlay (drawn after grid so it stays on top). */
        d.fillRect(0, 0, SCR_W, 11, T_BG);
        d.setTextColor(T_ACCENT, T_BG);
        d.setCursor(2, 2); d.print("HEX CASCADE");
        d.setTextColor(T_ACCENT2, T_BG);
        d.setCursor(SCR_W - 60, 2); d.print("// decoded");
        d.drawFastHLine(0, 11, SCR_W, T_DIM);
        delay(60);
    }
}

/* ============ TERMINAL CRACK — fake hashcat output ============ */

static const char *const TC_PWDS[] = {
    "password",     "P@ssw0rd!",   "12345678",     "qwerty123",   "letmein",
    "admin",        "welcome1",    "iloveyou",     "monkey99",    "dragon",
    "000000",       "abc123",      "trustno1",     "hunter2",     "shadow",
    "master",       "P0seidon",    "RootMe!",      "h4xx0r",      "deepblue",
    "summer2024",   "Spring2025",  "ChangeMe1!",   "NotMyPass",   "starwars",
    "supercalifrag","correcthorse","letmein123!",  "PwnThisOne",  "tesla88",
};
static const char *const TC_LINES[] = {
    "Session..........: poseidon",
    "Status...........: Running",
    "Hash.Mode........: 22000 (WPA-PBKDF2-PMKID+EAPOL)",
    "Hash.Target......: $WPA-PMKID-PBKDF2*",
    "Time.Started.....: now",
    "Time.Estimated...: 5 mins, 42 secs",
    "Kernel.Feature...: Pure Kernel",
    "Guess.Base.......: File (rockyou.txt)",
    "Recovered........: 0/1 (0.00%) Digests",
    "Progress.........: 1456384/14344392 (10.15%)",
    "Rejected.........: 0/1456384 (0.00%)",
    "Restore.Point....: 1310720/14344392 (9.13%)",
    "Restore.Sub.#1...: Salt:0 Amplifier:0-1 Iteration:0-1",
    "Candidate.Engine.: Device Generator",
    "Candidates.#1....: rockyou-30000-100000.txt",
    "Hardware.Mon.#1..: Util: 99% Core:1755MHz Mem: 5500MHz",
};

struct tc_line_t { char buf[40]; uint16_t color; };

static void run_terminal_crack(void)
{
    auto &d = M5Cardputer.Display;
    static tc_line_t lines[14];
    int line_count = 0;
    uint32_t next_line = millis();
    uint32_t next_crack = millis() + 8000 + (esp_random() % 6000);
    bool show_crack = false;
    uint32_t crack_until = 0;
    char crack_pwd[24] = { 0 };
    char crack_hash[16] = { 0 };

    while (input_poll() == PK_NONE) {
        uint32_t now = millis();
        d.fillScreen(T_BG);

        /* Header */
        d.setTextColor(T_ACCENT, T_BG);
        d.setCursor(2, 2); d.print("hashcat -m 22000 -a 0");
        d.setTextColor(T_DIM, T_BG);
        d.setCursor(SCR_W - 70, 2); d.print("// poseidon");
        d.drawFastHLine(0, 11, SCR_W, T_DIM);

        /* Push new line */
        if (now >= next_line) {
            tc_line_t l;
            uint8_t roll = esp_random() & 0xFF;
            if (roll < 0x60) {
                /* attempt line */
                snprintf(l.buf, sizeof(l.buf), "* attempt: %s",
                         TC_PWDS[esp_random() % (sizeof(TC_PWDS)/sizeof(TC_PWDS[0]))]);
                l.color = T_FG;
            } else if (roll < 0xC0) {
                strncpy(l.buf, TC_LINES[esp_random() % (sizeof(TC_LINES)/sizeof(TC_LINES[0]))], 39);
                l.buf[39] = 0;
                l.color = T_DIM;
            } else {
                snprintf(l.buf, sizeof(l.buf), "Speed.#1.........: %u H/s",
                         (unsigned)(8000 + (esp_random() % 9000)));
                l.color = T_ACCENT;
            }
            for (int i = 13; i > 0; --i) lines[i] = lines[i-1];
            lines[0] = l;
            if (line_count < 14) line_count++;
            next_line = now + 220 + (esp_random() % 350);
        }

        /* Draw lines (newest at top, scroll down) */
        for (int i = 0; i < line_count; ++i) {
            int y = 14 + i * 9;
            if (y > SCR_H - 14) break;
            d.setTextColor(lines[i].color, T_BG);
            d.setCursor(2, y); d.print(lines[i].buf);
        }

        /* CRACKED flash — overlays middle of screen */
        if (now >= next_crack && !show_crack) {
            show_crack = true;
            crack_until = now + 1800;
            const char *src = TC_PWDS[esp_random() % (sizeof(TC_PWDS)/sizeof(TC_PWDS[0]))];
            strncpy(crack_pwd, src, 23); crack_pwd[23] = 0;
            for (int i = 0; i < 8; ++i) {
                static const char hex[] = "0123456789abcdef";
                crack_hash[i] = hex[esp_random() % 16];
            }
            crack_hash[8] = 0;
            next_crack = now + 12000 + (esp_random() % 10000);
        }
        if (show_crack && now < crack_until) {
            int bx = 8, by = SCR_H/2 - 14, bw = SCR_W - 16, bh = 28;
            d.fillRect(bx, by, bw, bh, T_BG);
            d.drawRect(bx, by, bw, bh, T_GOOD);
            d.drawRect(bx+1, by+1, bw-2, bh-2, T_GOOD);
            d.setTextColor(T_GOOD, T_BG);
            d.setCursor(bx + 6, by + 4); d.print("CRACKED:");
            d.setTextColor(T_ACCENT2, T_BG);
            d.setCursor(bx + 60, by + 4); d.print(crack_pwd);
            d.setTextColor(T_DIM, T_BG);
            d.setCursor(bx + 6, by + 16); d.print("hash:");
            d.setTextColor(T_FG, T_BG);
            d.setCursor(bx + 38, by + 16); d.print(crack_hash);
        } else if (show_crack && now >= crack_until) {
            show_crack = false;
        }

        d.setTextColor(T_DIM, T_BG);
        d.setCursor(2, SCR_H - 9); d.print("// any key");
        delay(60);
    }
}

/* ============ NEURAL ARC — pulsing dot mesh + electric arcs ============ */

#define NA_NODES 28
struct na_node_t { int x, y; };

static void run_neural_arc(void)
{
    auto &d = M5Cardputer.Display;
    static na_node_t nodes[NA_NODES];
    /* Place nodes in a slightly-jittered grid so the mesh looks intentional. */
    int cols = 7, rows = 4;
    int dx = SCR_W / (cols + 1);
    int dy = (SCR_H - 30) / (rows + 1);
    int idx = 0;
    for (int r = 0; r < rows && idx < NA_NODES; ++r) {
        for (int c = 0; c < cols && idx < NA_NODES; ++c) {
            nodes[idx].x = dx * (c + 1) + (int)(esp_random() % 9) - 4;
            nodes[idx].y = 16 + dy * (r + 1) + (int)(esp_random() % 7) - 3;
            idx++;
        }
    }
    int node_count = idx;

    uint32_t next_arc = millis() + 600;
    int arc_a = 0, arc_b = 0;
    uint32_t arc_until = 0;

    while (input_poll() == PK_NONE) {
        uint32_t now = millis();
        d.fillScreen(T_BG);

        /* Header */
        d.setTextColor(T_ACCENT, T_BG);
        d.setCursor(2, 2); d.print("NEURAL ARC // mesh");
        d.setTextColor(T_DIM, T_BG);
        char hud[20];
        snprintf(hud, sizeof(hud), "nodes:%d", node_count);
        d.setCursor(SCR_W - 60, 2); d.print(hud);
        d.drawFastHLine(0, 11, SCR_W, T_DIM);

        /* Connect each node to its 2 nearest neighbors with thin dim lines. */
        for (int i = 0; i < node_count; ++i) {
            int best1 = -1, best2 = -1;
            int d1 = 1<<30, d2 = 1<<30;
            for (int j = 0; j < node_count; ++j) {
                if (j == i) continue;
                int dxn = nodes[i].x - nodes[j].x;
                int dyn = nodes[i].y - nodes[j].y;
                int dd = dxn*dxn + dyn*dyn;
                if (dd < d1) { d2 = d1; best2 = best1; d1 = dd; best1 = j; }
                else if (dd < d2) { d2 = dd; best2 = j; }
            }
            if (best1 >= 0) d.drawLine(nodes[i].x, nodes[i].y, nodes[best1].x, nodes[best1].y, T_DIM);
            if (best2 >= 0) d.drawLine(nodes[i].x, nodes[i].y, nodes[best2].x, nodes[best2].y, T_DIM);
        }

        /* Pulse-brightness on nodes — sinusoidal per-node phase. */
        for (int i = 0; i < node_count; ++i) {
            float ph = (now / 250.0f) + i * 0.7f;
            float v  = (sinf(ph) + 1.0f) * 0.5f;        /* 0..1 */
            uint8_t a = (uint8_t)(80 + v * 175);
            uint16_t cc = blend565(T_BG, T_ACCENT, a);
            d.fillCircle(nodes[i].x, nodes[i].y, 2, cc);
        }

        /* Random electric arc between 2 nodes — bright zigzag with decay. */
        if (now >= next_arc) {
            arc_a = (int)(esp_random() % node_count);
            do { arc_b = (int)(esp_random() % node_count); } while (arc_b == arc_a);
            arc_until = now + 350;
            next_arc = now + 500 + (esp_random() % 1200);
        }
        if (now < arc_until) {
            uint8_t a = (uint8_t)(255 - ((now - (arc_until - 350)) * 255 / 350));
            uint16_t cc = blend565(T_BG, T_ACCENT2, a);
            int x0 = nodes[arc_a].x, y0 = nodes[arc_a].y;
            int x1 = nodes[arc_b].x, y1 = nodes[arc_b].y;
            /* Zigzag: 5 segments with random perpendicular offset. */
            int segs = 5;
            int px = x0, py = y0;
            for (int s = 1; s <= segs; ++s) {
                int tx = x0 + (x1 - x0) * s / segs;
                int ty = y0 + (y1 - y0) * s / segs;
                if (s < segs) {
                    int jitter = (int)(esp_random() % 9) - 4;
                    tx += jitter;
                    ty += (int)(esp_random() % 9) - 4;
                }
                d.drawLine(px, py, tx, ty, cc);
                d.drawLine(px, py + 1, tx, ty + 1, cc);
                px = tx; py = ty;
            }
            d.fillCircle(x0, y0, 4, T_FG);
            d.fillCircle(x1, y1, 4, T_FG);
        }

        d.setTextColor(T_DIM, T_BG);
        d.setCursor(2, SCR_H - 9); d.print("// any key");
        delay(45);
    }
}

/* ============ GLITCH BSOD — periodic full-screen text glitches ============ */

static const char *const GB_MSGS[] = {
    "ACCESS DENIED",   "ROOT GAINED",      "INTRUSION DETECTED",
    "AUTH BYPASS",     "PRIV ESC",         "PORT 22 OPEN",
    "HASH CRACKED",    "BREACH IN PROGRESS","SYSTEM COMPROMISED",
    "FIREWALL DOWN",   "PMKID CAPTURED",   "DEAUTH BURST",
    "SHELL ACQUIRED",  "PWNED",            "0xDEADBEEF",
};

static void run_glitch_bsod(void)
{
    auto &d = M5Cardputer.Display;
    uint32_t next_burst = millis() + 800;
    uint32_t burst_until = 0;
    char msg[24] = "";
    int msg_dy = 0;

    while (input_poll() == PK_NONE) {
        uint32_t now = millis();
        d.fillScreen(T_BG);

        /* Background scatter — sparse noise dots so the screen never feels totally dead. */
        for (int i = 0; i < 22; ++i) {
            int x = esp_random() % SCR_W;
            int y = esp_random() % SCR_H;
            d.drawPixel(x, y, ((esp_random() & 1) ? T_DIM : T_ACCENT));
        }

        /* Spawn a glitch burst */
        if (now >= next_burst) {
            const char *src = GB_MSGS[esp_random() % (sizeof(GB_MSGS)/sizeof(GB_MSGS[0]))];
            strncpy(msg, src, 23); msg[23] = 0;
            msg_dy = -3 + (int)(esp_random() % 7);
            burst_until = now + 700;
            next_burst = now + 1400 + (esp_random() % 1800);
        }
        if (now < burst_until) {
            uint32_t age = 700 - (burst_until - now);
            uint8_t a;
            if (age < 100)        a = (uint8_t)((age * 255) / 100);
            else if (age < 500)   a = 255;
            else                  a = (uint8_t)(255 - ((age - 500) * 255 / 200));
            d.setTextSize(2);
            int tw = d.textWidth(msg);
            int tx = (SCR_W - tw) / 2;
            int ty = SCR_H/2 - 8 + msg_dy;
            /* Chromatic split: T_ACCENT slightly left, T_ACCENT2 slightly right, T_FG center hot. */
            int shake = (int)(esp_random() % 3) - 1;
            d.setTextColor(blend565(T_BG, T_ACCENT,  a), T_BG);
            d.setCursor(tx - 2 + shake, ty); d.print(msg);
            d.setTextColor(blend565(T_BG, T_ACCENT2, a), T_BG);
            d.setCursor(tx + 2 + shake, ty); d.print(msg);
            d.setTextColor(blend565(T_BG, T_FG,      a), T_BG);
            d.setCursor(tx     + shake, ty); d.print(msg);
            d.setTextSize(1);
            /* Scanline glitches */
            for (int s = 0; s < 4; ++s) {
                int y = (esp_random() % SCR_H);
                d.drawFastHLine(0, y, SCR_W, T_ACCENT2);
            }
        }

        /* Header overlay last so glitch text doesn't cover it. */
        d.fillRect(0, 0, SCR_W, 11, T_BG);
        d.setTextColor(T_ACCENT, T_BG);
        d.setCursor(2, 2); d.print("// SYSTEM");
        d.setTextColor(T_BAD, T_BG);
        d.setCursor(SCR_W - 60, 2); d.print("STATUS: HOT");
        d.drawFastHLine(0, 11, SCR_W, T_BAD);

        d.setTextColor(T_DIM, T_BG);
        d.setCursor(2, SCR_H - 9); d.print("// any key");
        delay(45);
    }
}

/* ============ TIDE WAVES — overlapping sine waves ============ */

static void run_tide_waves(void)
{
    auto &d = M5Cardputer.Display;
    /* 4 waves, each a sin curve sampled across screen width. */
    struct wave_t { float freq; float amp; float phase; float speed; uint16_t color; };
    wave_t waves[4] = {
        { 0.040f, 18.0f, 0.0f, 0.012f, T_ACCENT  },
        { 0.060f, 14.0f, 1.2f, 0.018f, T_ACCENT2 },
        { 0.025f, 22.0f, 2.4f, 0.008f, T_FG      },
        { 0.080f, 10.0f, 0.6f, 0.024f, T_DIM     },
    };

    while (input_poll() == PK_NONE) {
        uint32_t now = millis();
        d.fillScreen(T_BG);

        /* Header */
        d.setTextColor(T_ACCENT, T_BG);
        d.setCursor(2, 2); d.print("TIDE // waves");
        d.setTextColor(T_DIM, T_BG);
        d.setCursor(SCR_W - 70, 2); d.print("// poseidon");
        d.drawFastHLine(0, 11, SCR_W, T_DIM);

        int cy = SCR_H / 2 + 4;
        for (int w = 0; w < 4; ++w) {
            wave_t &wv = waves[w];
            wv.phase += wv.speed;
            int prev_y = cy;
            for (int x = 0; x < SCR_W; ++x) {
                int y = cy + (int)(sinf(x * wv.freq + wv.phase) * wv.amp);
                if (x == 0) prev_y = y;
                d.drawLine(x - 1, prev_y, x, y, wv.color);
                prev_y = y;
            }
        }

        /* Tide line / horizon */
        d.drawFastHLine(0, cy, SCR_W, T_DIM);

        d.setTextColor(T_DIM, T_BG);
        d.setCursor(2, SCR_H - 9); d.print("// any key");
        delay(40);
    }
}

/* ============ POOL TABLE + DISPATCHER ============ */

typedef void (*screensaver_fn)(void);
struct screensaver_def_t { const char *name; screensaver_fn run; };

static const screensaver_def_t s_pool[] = {
    { "WARDRIVE",       run_wardrive_cinema },
    { "MATRIX RAIN",    run_matrix_rain     },
    { "BREATHING",      run_eink_breathing  },
    { "DEEP SCAN",      run_deep_scan       },
    { "PORT SCAN",      run_port_scan       },
    { "HEX CASCADE",    run_hex_cascade     },
    { "TERMINAL CRACK", run_terminal_crack  },
    { "NEURAL ARC",     run_neural_arc      },
    { "GLITCH BSOD",    run_glitch_bsod     },
    { "TIDE WAVES",     run_tide_waves      },
};
static const int POOL_N = (int)(sizeof(s_pool) / sizeof(s_pool[0]));

int          screensaver_pool_count(void)    { return POOL_N; }
const char  *screensaver_pool_name(int idx)
{
    if (idx < 0 || idx >= POOL_N) return "?";
    return s_pool[idx].name;
}

int screensaver_pick_get(void) { load_settings(); return s_pick; }

void screensaver_pick_set(int idx)
{
    load_settings();
    if (idx < SCREENSAVER_PICK_SHUFFLE) idx = SCREENSAVER_PICK_SHUFFLE;
    if (idx >= POOL_N) idx = POOL_N - 1;
    s_pick = (int8_t)idx;
    Preferences p;
    if (p.begin("pscr", false)) { p.putChar("pick", (char)idx); p.end(); }
}

void screensaver_run_index(int idx)
{
    load_settings();
    if (idx < 0 || idx >= POOL_N) return;
    s_pool[idx].run();
    save_last(idx);
}

/* SHUFFLE picker — uniform random across pool, never pick s_last twice in a row. */
static int pick_shuffle(void)
{
    if (POOL_N <= 1) return 0;
    int n;
    int safety = 8;
    do {
        n = (int)(esp_random() % POOL_N);
        if (--safety <= 0) break;
    } while (n == s_last);
    return n;
}

bool screensaver_check_idle(void)
{
    load_settings();
    if (!s_enabled) return false;
    uint32_t last = input_last_input_ms();
    if (last == 0) return false;
    if (millis() - last < s_timeout_ms) return false;

    int idx;
    if (s_pick == SCREENSAVER_PICK_SHUFFLE) idx = pick_shuffle();
    else                                    idx = s_pick;
    if (idx < 0 || idx >= POOL_N) idx = 0;

    s_pool[idx].run();
    save_last(idx);
    return true;
}
