/*
 * wifi_spectrum — real-time 2.4 GHz channel activity visualizer.
 *
 * Promiscuous mode on each of the 14 channels in rotation. For every
 * frame we see, track the best (strongest) RSSI and a packet count.
 * Render the whole thing as a live bar graph with a pulse animation
 * on the currently-sampling channel.
 *
 * 2.4 GHz has 14 channels (1-14, though most devices stop at 13).
 * Dwell ~80ms per channel so a full sweep is ~1.2 s — fast enough to
 * look live, slow enough to catch intermittent beacons.
 */
#include "app.h"
#include "../theme.h"
#include "ui.h"
#include "input.h"
#include "radio.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include <math.h>

#define CH_N 14  /* channels 1..13 valid + extra slot to keep the math simple */

static volatile int8_t  s_peak[CH_N + 1];
static volatile uint32_t s_pkts[CH_N + 1];
static volatile uint8_t s_current_ch = 1;
static volatile bool    s_running = false;
static volatile bool    s_decay_alive = false;
static volatile bool    s_hop_alive   = false;

/* Visual style — cycled via V key, persisted to NVS. */
enum spec_style_t : uint8_t {
    SPEC_BARS      = 0,
    SPEC_WATERFALL = 1,
    SPEC_RADAR     = 2,
    SPEC_STYLE_COUNT
};
static spec_style_t s_style = SPEC_BARS;
static const char *spec_style_name(spec_style_t s)
{
    switch (s) {
    case SPEC_BARS:      return "BARS";
    case SPEC_WATERFALL: return "WATERFALL";
    case SPEC_RADAR:     return "RADAR";
    default:             return "?";
    }
}
static void spec_style_load(void)
{
    Preferences p;
    if (p.begin("pui", true)) {
        uint8_t v = p.getUChar("specstyle", 0);
        if (v < SPEC_STYLE_COUNT) s_style = (spec_style_t)v;
        p.end();
    }
}
static void spec_style_save(void)
{
    Preferences p;
    if (p.begin("pui", false)) {
        p.putUChar("specstyle", (uint8_t)s_style);
        p.end();
    }
}

/* Waterfall: ring buffer of 13 cols × WF_ROWS history rows of RSSI. */
#define WF_ROWS 70
static int8_t s_wf[WF_ROWS][14];   /* index [row][ch], ch 1..13 used */
static int    s_wf_head = 0;       /* newest row */
static uint32_t s_wf_last_push = 0;

/* RSSI → color ramp. -100 = transparent (bg), -85 = deep blue, -70 = cyan,
 * -55 = green, -45 = yellow, -35 = magenta/hot. */
static uint16_t rssi_color(int8_t rssi)
{
    if (rssi <= -100) return T_BG;
    if (rssi > -45)   return T_BAD;          /* magenta hot */
    if (rssi > -55)   return T_WARN;         /* yellow/orange */
    if (rssi > -70)   return T_GOOD;         /* green */
    if (rssi > -85)   return T_ACCENT;       /* cyan */
    return 0x2124;                            /* deep cool */
}

static void spec_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    (void)type;
    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    int8_t rssi = pkt->rx_ctrl.rssi;
    int ch = s_current_ch;
    if (ch < 1 || ch > CH_N) return;
    if (rssi > s_peak[ch]) s_peak[ch] = rssi;
    s_pkts[ch]++;
}

static void hop_task(void *)
{
    s_hop_alive = true;
    while (s_running) {
        s_current_ch = (s_current_ch % 13) + 1;
        esp_wifi_set_channel(s_current_ch, WIFI_SECOND_CHAN_NONE);
        delay(80);
    }
    s_hop_alive = false;
    vTaskDelete(nullptr);
}

static void decay_task(void *)
{
    /* Slowly decay peaks so the display responds to changes instead
     * of stuck at the all-time max. Subtract 1 dB every 500ms. */
    s_decay_alive = true;
    while (s_running) {
        delay(500);
        for (int c = 1; c <= 13; ++c) {
            int8_t v = s_peak[c];
            if (v > -100) s_peak[c] = v - 1;
        }
    }
    s_decay_alive = false;
    vTaskDelete(nullptr);
}

/* Per-frame cache for draw_spectrum — reset when we re-enter the feature
 * (statics would otherwise persist and skip the static-chrome paint). */
static bool   s_spec_first = true;
static int8_t s_spec_last_peak[CH_N + 1];
static int    s_spec_last_current_ch = -1;
/* Radar sweep angle (degrees, advances each frame). */
static float  s_radar_angle = 0.0f;

void wifi_spectrum_invalidate_cache(void)
{
    s_spec_first = true;
    s_spec_last_current_ch = -1;
    for (int c = 0; c <= CH_N; ++c) s_spec_last_peak[c] = -127;
    for (int r = 0; r < WF_ROWS; ++r)
        for (int c = 0; c < 14; ++c) s_wf[r][c] = -100;
    s_wf_head = 0;
    s_wf_last_push = 0;
    s_radar_angle = 0.0f;
}

static void draw_bars(void)
{
    auto &d = M5Cardputer.Display;
    bool &first = s_spec_first;
    int8_t *last_peak = s_spec_last_peak;
    int &last_current_ch = s_spec_last_current_ch;

    const int bar_w  = 16;
    const int gap    = 2;
    const int top    = BODY_Y + 18;
    const int bottom = FOOTER_Y - 10;
    const int height = bottom - top;
    const int start_x = (SCR_W - (13 * (bar_w + gap))) / 2;

    if (first) {
        ui_clear_body();
        d.setTextColor(T_ACCENT, T_BG);
        d.setCursor(4, BODY_Y + 2);
        d.printf("SPECTRUM %s", spec_style_name(s_style));
        d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT);
        /* Bar baselines + channel labels painted once. */
        for (int c = 1; c <= 13; ++c) {
            int x = start_x + (c - 1) * (bar_w + gap);
            d.drawFastVLine(x + bar_w / 2 - 1, top, height, 0x0841);
            d.setTextColor(T_DIM, T_BG);
            d.setCursor(x + (c < 10 ? bar_w / 2 - 3 : bar_w / 2 - 6), bottom + 1);
            d.printf("%d", c);
        }
        for (int c = 0; c <= CH_N; ++c) last_peak[c] = -127;
        first = false;
    }

    /* Update only bars that changed. */
    for (int c = 1; c <= 13; ++c) {
        int8_t rssi = s_peak[c];
        if (rssi == last_peak[c] && c != s_current_ch && c != last_current_ch) continue;
        last_peak[c] = rssi;

        int x  = start_x + (c - 1) * (bar_w + gap);
        int bh = (rssi + 100) * height / 70;
        if (bh < 0) bh = 0;
        if (bh > height) bh = height;

        uint16_t color;
        if      (rssi > -50) color = T_BAD;
        else if (rssi > -70) color = T_WARN;
        else if (rssi > -85) color = T_GOOD;
        else                 color = 0x2124;

        /* Clear the bar column back to bg, redraw baseline, fill new bar. */
        d.fillRect(x, top, bar_w, height, T_BG);
        d.drawFastVLine(x + bar_w / 2 - 1, top, height, 0x0841);
        d.fillRect(x, bottom - bh, bar_w, bh, color);

        /* Channel number colour tracks current-selected highlight. */
        d.setTextColor(c == s_current_ch ? T_ACCENT : T_DIM, T_BG);
        d.fillRect(x, bottom + 1, bar_w, 8, T_BG);
        d.setCursor(x + (c < 10 ? bar_w / 2 - 3 : bar_w / 2 - 6), bottom + 1);
        d.printf("%d", c);
    }

    /* Pulse ring — erase old one, draw new one. */
    if (s_current_ch != last_current_ch) {
        if (last_current_ch >= 1 && last_current_ch <= 13) {
            int ox = start_x + (last_current_ch - 1) * (bar_w + gap);
            d.drawRect(ox - 1, top - 1, bar_w + 2, height + 2, T_BG);
        }
        if (s_current_ch >= 1 && s_current_ch <= 13) {
            int nx = start_x + (s_current_ch - 1) * (bar_w + gap);
            d.drawRect(nx - 1, top - 1, bar_w + 2, height + 2, 0xFFFF);
        }
        last_current_ch = s_current_ch;
    }

    ui_draw_status(radio_name(), "spec");
}

/* ============ WATERFALL — SDR-style scrolling spectrogram ============ */

static void draw_waterfall(void)
{
    auto &d = M5Cardputer.Display;
    const int top      = BODY_Y + 14;
    const int bottom   = FOOTER_Y - 10;
    const int rows_h   = bottom - top;
    const int col_w    = (SCR_W - 8) / 13;   /* 14 cols × 13 ~= 17 px */
    const int start_x  = 4 + ((SCR_W - 8) - col_w * 13) / 2;
    const int rows     = rows_h;             /* 1 row per pixel — dense */
    if (s_spec_first) {
        d.fillRect(0, BODY_Y, SCR_W, BODY_H, T_BG);
        d.setTextColor(T_ACCENT, T_BG);
        d.setCursor(4, BODY_Y + 2);
        d.printf("SPECTRUM %s", spec_style_name(s_style));
        d.setTextColor(T_DIM, T_BG);
        d.setCursor(SCR_W - 76, BODY_Y + 2);
        d.print("V=style");
        s_spec_first = false;
    }

    /* Push a new row every ~80 ms — matches hop dwell so each row is one
     * full sample for one channel (the current). The other 12 cells in
     * that row come from the last-seen peak for those channels — so the
     * waterfall is a rolling snapshot, not a pure "current channel only"
     * trail. */
    uint32_t now = millis();
    if (now - s_wf_last_push > 80) {
        s_wf_last_push = now;
        s_wf_head = (s_wf_head + 1) % WF_ROWS;
        for (int c = 1; c <= 13; ++c) s_wf[s_wf_head][c] = s_peak[c];
    }

    /* Render: top row is newest. Walk back through history.
     * Cap rendered rows at WF_ROWS so we don't draw past our buffer. */
    int draw_rows = rows_h < WF_ROWS ? rows_h : WF_ROWS;
    for (int r = 0; r < draw_rows; ++r) {
        int hist = (s_wf_head - r + WF_ROWS) % WF_ROWS;
        int y = top + r;
        for (int c = 1; c <= 13; ++c) {
            int x = start_x + (c - 1) * col_w;
            uint16_t color = rssi_color(s_wf[hist][c]);
            d.drawFastHLine(x, y, col_w - 1, color);
        }
    }

    /* Channel labels along the bottom. Cyan for current. */
    for (int c = 1; c <= 13; ++c) {
        int x = start_x + (c - 1) * col_w;
        d.fillRect(x, bottom + 1, col_w - 1, 8, T_BG);
        d.setTextColor(c == s_current_ch ? T_ACCENT : T_DIM, T_BG);
        d.setCursor(x + (c < 10 ? col_w / 2 - 3 : col_w / 2 - 6), bottom + 1);
        d.printf("%d", c);
    }

    /* Current-channel highlight: thin magenta column overlay at top edge. */
    int cx = start_x + (s_current_ch - 1) * col_w;
    d.drawFastVLine(cx, top - 2, 2, T_ACCENT2);
    d.drawFastVLine(cx + col_w - 1, top - 2, 2, T_ACCENT2);

    ui_draw_status(radio_name(), "spec");
}

/* ============ RADAR — polar sweep with persistent blips ============ */

static struct { float age_ms; int8_t rssi; } s_radar_blip[14];

static void draw_radar(void)
{
    auto &d = M5Cardputer.Display;
    const int cx = SCR_W / 2;
    const int cy = BODY_Y + (BODY_H / 2) + 4;
    const int rmax = (BODY_H / 2) - 4;

    if (s_spec_first) {
        d.fillRect(0, BODY_Y, SCR_W, BODY_H, T_BG);
        d.setTextColor(T_ACCENT, T_BG);
        d.setCursor(4, BODY_Y + 2);
        d.printf("SPECTRUM %s", spec_style_name(s_style));
        d.setTextColor(T_DIM, T_BG);
        d.setCursor(SCR_W - 76, BODY_Y + 2);
        d.print("V=style");
        s_spec_first = false;
        for (int c = 0; c < 14; ++c) { s_radar_blip[c].age_ms = 1e9; s_radar_blip[c].rssi = -100; }
    }

    /* Erase frame buffer below header. */
    d.fillRect(0, BODY_Y + 12, SCR_W, BODY_H - 12, T_BG);

    /* Three concentric range rings — RSSI -90 / -70 / -50 markers. */
    d.drawCircle(cx, cy, rmax,         0x2124);
    d.drawCircle(cx, cy, rmax * 2 / 3, 0x2124);
    d.drawCircle(cx, cy, rmax / 3,     0x2124);

    /* Channel labels around the perimeter. ch1 at 12 o'clock, sweep CW. */
    for (int c = 1; c <= 13; ++c) {
        float ang = (c - 1) * (360.0f / 13.0f) - 90.0f;   /* deg, -90 = 12 o'clock */
        float rad = ang * (float)M_PI / 180.0f;
        int lx = cx + (int)(cosf(rad) * (rmax + 6));
        int ly = cy + (int)(sinf(rad) * (rmax + 6));
        d.setTextColor(c == s_current_ch ? T_ACCENT : T_DIM, T_BG);
        d.setCursor(lx - (c < 10 ? 2 : 5), ly - 3);
        d.printf("%d", c);
    }

    /* Decay + maybe re-arm blips. */
    static uint32_t last_tick = 0;
    uint32_t now = millis();
    uint32_t dt = (last_tick == 0) ? 50 : (now - last_tick);
    last_tick = now;
    for (int c = 1; c <= 13; ++c) {
        s_radar_blip[c].age_ms += dt;
        /* Re-arm with current peak — keep brightest seen recently. */
        if (s_peak[c] > s_radar_blip[c].rssi || s_radar_blip[c].age_ms > 5000) {
            s_radar_blip[c].rssi = s_peak[c];
            s_radar_blip[c].age_ms = 0;
        }
    }

    /* Draw blips. Position = (channel angle, distance scaled by RSSI). */
    for (int c = 1; c <= 13; ++c) {
        if (s_radar_blip[c].rssi <= -99) continue;
        float ang = (c - 1) * (360.0f / 13.0f) - 90.0f;
        float rad = ang * (float)M_PI / 180.0f;
        int pct = (s_radar_blip[c].rssi + 100) * 100 / 70;
        if (pct < 5) pct = 5; if (pct > 100) pct = 100;
        int dist = rmax * (100 - pct) / 100;   /* hot = close to center */
        int bx = cx + (int)(cosf(rad) * dist);
        int by = cy + (int)(sinf(rad) * dist);
        /* Brightness fades with age. */
        uint16_t color = rssi_color(s_radar_blip[c].rssi);
        if (s_radar_blip[c].age_ms > 2000) color = 0x2124;
        int radius = (pct > 70) ? 4 : (pct > 40) ? 3 : 2;
        d.fillCircle(bx, by, radius, color);
        if (c == s_current_ch) d.drawCircle(bx, by, radius + 2, T_FG);
    }

    /* Sweep beam — rotates CW, completes ~2 s. */
    s_radar_angle += 360.0f * (dt / 2000.0f);
    if (s_radar_angle >= 360.0f) s_radar_angle -= 360.0f;
    float bang = (s_radar_angle - 90.0f) * (float)M_PI / 180.0f;
    int bx = cx + (int)(cosf(bang) * rmax);
    int by = cy + (int)(sinf(bang) * rmax);
    d.drawLine(cx, cy, bx, by, T_ACCENT);
    /* Trailing afterglow — 2 segments behind. */
    for (int t = 1; t <= 2; ++t) {
        float tang = (s_radar_angle - 90.0f - t * 10.0f) * (float)M_PI / 180.0f;
        int tx = cx + (int)(cosf(tang) * rmax);
        int ty = cy + (int)(sinf(tang) * rmax);
        d.drawLine(cx, cy, tx, ty, t == 1 ? T_GOOD : 0x2124);
    }

    /* Center dot. */
    d.fillCircle(cx, cy, 2, T_ACCENT2);

    ui_draw_status(radio_name(), "spec");
}

/* Dispatcher. */
static void draw_spectrum(void)
{
    switch (s_style) {
    case SPEC_WATERFALL: draw_waterfall(); break;
    case SPEC_RADAR:     draw_radar();     break;
    case SPEC_BARS:
    default:             draw_bars();      break;
    }
}

void feat_wifi_spectrum(void)
{
    radio_switch(RADIO_WIFI);
    wifi_lean_sta_init();   /* raw-safe STA init; Arduino WiFi.mode() asserts on a raw-inited driver. */

    for (int c = 0; c <= CH_N; ++c) { s_peak[c] = -100; s_pkts[c] = 0; }
    s_current_ch = 1;
    s_running = true;

    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(spec_cb);
    esp_wifi_set_channel(s_current_ch, WIFI_SECOND_CHAN_NONE);

    xTaskCreate(hop_task,   "spec_hop",   3072, nullptr, 4, nullptr);
    xTaskCreate(decay_task, "spec_decay", 2048, nullptr, 3, nullptr);

    /* Force draw_spectrum's per-frame cache to rebuild the static chrome
     * on re-entry (it's file-scope static — would otherwise think it's
     * already painted from a previous session). */
    extern void wifi_spectrum_invalidate_cache(void);
    wifi_spectrum_invalidate_cache();

    spec_style_load();
    ui_draw_footer("R=reset  V=style  `=back");
    uint32_t last = 0;
    while (true) {
        /* Radar wants smoother animation than bars — render at ~30 fps
         * when in radar/waterfall, ~12 fps for static bars (the original
         * cadence). */
        uint32_t period = (s_style == SPEC_BARS) ? 80 : 33;
        if (millis() - last > period) {
            last = millis();
            draw_spectrum();
        }
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(10); continue; }
        if (k == PK_ESC) break;
        if (k == PK_ACTIONS) {
            static const char *const acts[] = { "Reset peaks", "Toggle view" };
            int pick = ui_action_menu("SPECTRUM", acts, 2);
            if (pick == 0) k = 'r';
            else if (pick == 1) k = 'v';
        }
        if (k == 'r' || k == 'R') {
            for (int c = 0; c <= CH_N; ++c) { s_peak[c] = -100; s_pkts[c] = 0; }
        }
        if (k == 'v' || k == 'V') {
            s_style = (spec_style_t)(((int)s_style + 1) % SPEC_STYLE_COUNT);
            spec_style_save();
            wifi_spectrum_invalidate_cache();
        }
    }

    s_running = false;
    /* Join the decay task before returning — its loop period (500ms) is
     * longer than the old delay(200), so it could still be running when
     * the next feature inits the radio. Bounded wait. */
    uint32_t deadline = millis() + 800;
    while ((s_decay_alive || s_hop_alive) && millis() < deadline) delay(5);
    esp_wifi_set_promiscuous(false);
    delay(200);
}
