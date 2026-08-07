/*
 * ui_subghz.cpp — shared SubGHz visual widgets.
 */
#include "ui_subghz.h"
#include "app.h"
#include "theme.h"
#include "ui.h"
#include <M5Cardputer.h>
#include <Arduino.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---- color helpers ---- */
static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

/* Simple 6-stop spectral palette — rotates on pulse duration bucket. */
static uint16_t bucket_color(int bucket, bool is_high)
{
    static const uint16_t hi_palette[4] = {
        0xF81F,   /* magenta — short */
        0xFC00,   /* orange  — mid-short */
        0xFFE0,   /* yellow  — mid-long */
        0x07FF    /* cyan    — long */
    };
    static const uint16_t lo_palette[4] = {
        0x780F,   /* dark magenta */
        0x7800,   /* dark orange */
        0x7BE0,   /* dark yellow-green */
        0x03EF    /* dark cyan */
    };
    int b = bucket; if (b < 0) b = 0; if (b > 3) b = 3;
    return is_high ? hi_palette[b] : lo_palette[b];
}

static int quartile(int val, int min_v, int max_v)
{
    if (max_v <= min_v) return 0;
    int q = ((val - min_v) * 4) / (max_v - min_v + 1);
    if (q < 0) q = 0;
    if (q > 3) q = 3;
    return q;
}

/* ---- pulse wave ---- */

void ui_draw_pulse_wave(int x, int y, int w, int h,
                        const int16_t *pulses, int n_pulses,
                        int playhead_idx)
{
    auto &d = M5Cardputer.Display;
    /* Body + midline. */
    d.fillRect(x, y, w, h, T_BG);
    int mid = y + h / 2;
    /* Glow halo: very dim top/bottom. */
    d.drawFastHLine(x, y, w, 0x1082);
    d.drawFastHLine(x, y + h - 1, w, 0x1082);
    /* Midline — dim scanline. */
    d.drawFastHLine(x, mid, w, 0x2944);
    /* Frame. */
    d.drawRect(x - 1, y - 1, w + 2, h + 2, T_DIM);

    if (n_pulses <= 0 || !pulses) return;

    uint32_t total = 0;
    int max_abs = 1, min_abs = 0x7FFF;
    for (int i = 0; i < n_pulses; ++i) {
        int a = abs(pulses[i]);
        if (a == 0) continue;
        total += a;
        if (a > max_abs) max_abs = a;
        if (a < min_abs) min_abs = a;
    }
    if (total == 0) return;

    int half = h / 2 - 2;
    int cur_x = x;
    uint32_t acc = 0;

    for (int i = 0; i < n_pulses; ++i) {
        int a = abs(pulses[i]);
        acc += a;
        int next_x = x + (int)((uint64_t)acc * w / total);
        int bw = next_x - cur_x;
        if (bw < 1) bw = 1;
        int bh = (int)((uint64_t)a * half / max_abs);
        if (bh < 1) bh = 1;

        bool high = pulses[i] > 0;
        int q = quartile(a, min_abs, max_abs);
        uint16_t body = bucket_color(q, high);
        uint16_t edge = bucket_color(q, !high);  /* darker counterpart */

        int by = high ? (mid - bh)     : (mid + 1);
        /* Gradient: top/outer edge lighter, core darker — faked by
         * drawing the outer 1px border in 'body', interior in 'edge'. */
        d.fillRect(cur_x, by, bw, bh, edge);
        if (bh >= 2) {
            /* Top/edge highlight pixel row for glow. */
            if (high) d.drawFastHLine(cur_x, by, bw, body);
            else      d.drawFastHLine(cur_x, by + bh - 1, bw, body);
        }
        cur_x = next_x;
        if (cur_x >= x + w) break;
    }

    if (playhead_idx >= 0 && playhead_idx < n_pulses) {
        uint32_t pacc = 0;
        for (int i = 0; i <= playhead_idx; ++i) pacc += abs(pulses[i]);
        int px = x + (int)((uint64_t)pacc * w / total);
        /* Bright yellow playhead with fading trail. */
        for (int k = 0; k < 4; ++k) {
            uint8_t intensity = 255 - k * 50;
            uint16_t col = rgb565(intensity, intensity, 0);
            int cx = px - k;
            if (cx >= x && cx < x + w) d.drawFastVLine(cx, y, h, col);
        }
        d.drawFastVLine(px + 1, y, h, T_WARN);
    }
}

/* ---- freq band ---- */

void ui_draw_freq_band(int x, int y, int w, int h, float freq_mhz)
{
    auto &d = M5Cardputer.Display;
    d.fillRect(x, y, w, h + 10, T_BG);

    const float min_f = 280.0f;
    const float max_f = 930.0f;

    /* Baseline with subtle tick marks every 100 MHz. */
    d.drawFastHLine(x, y + h - 2, w, T_DIM);
    for (int f = 300; f <= 900; f += 100) {
        int tx = x + (int)((f - min_f) * w / (max_f - min_f));
        d.drawFastVLine(tx, y + h - 4, 2, 0x3186);
    }

    struct band_t { float lo, hi; uint16_t hi_color, lo_color; };
    const band_t bands[] = {
        {300.0f, 348.0f, 0xF81F, 0x780F},  /* magenta */
        {387.0f, 464.0f, 0xFFE0, 0x7BE0},  /* yellow  */
        {779.0f, 928.0f, 0x07FF, 0x03EF},  /* cyan    */
    };
    for (const band_t &b : bands) {
        int bx1 = x + (int)((b.lo - min_f) * w / (max_f - min_f));
        int bx2 = x + (int)((b.hi - min_f) * w / (max_f - min_f));
        if (bx2 <= bx1) continue;
        /* 2-row gradient: bottom = dark, top = bright — volumetric band */
        d.fillRect(bx1, y + h - 5, bx2 - bx1, 1, b.hi_color);
        d.fillRect(bx1, y + h - 4, bx2 - bx1, 2, b.lo_color);
    }

    /* Cursor: triangle pointer + glowing vertical line. */
    int cx = x + (int)((freq_mhz - min_f) * w / (max_f - min_f));
    if (cx < x) cx = x;
    if (cx > x + w - 1) cx = x + w - 1;
    for (int k = 0; k < 3; ++k) {
        int tri_w = 3 - k;
        if (tri_w < 1) continue;
        d.drawFastVLine(cx - tri_w, y + k + 2, h - k - 4, T_FG);
        d.drawFastVLine(cx + tri_w, y + k + 2, h - k - 4, T_FG);
    }
    d.drawFastVLine(cx, y, h - 2, T_WARN);
    d.fillTriangle(cx - 3, y, cx + 3, y, cx, y + 5, T_WARN);

    /* Freq readout — big'ish numbers, colored to active band. */
    uint16_t freq_col = T_FG;
    for (const band_t &b : bands) {
        if (freq_mhz >= b.lo && freq_mhz <= b.hi) { freq_col = b.hi_color; break; }
    }
    char buf[24];
    snprintf(buf, sizeof(buf), "%.3f MHz", freq_mhz);
    d.setTextColor(freq_col, T_BG);
    d.setCursor(x, y + h + 2);
    d.print(buf);
}

/* ---- scrolling RSSI scope ---- */

#define SCOPE_MAX_W SCR_W
static int8_t  s_scope_hist[SCOPE_MAX_W];
static int     s_scope_head = 0;
static int     s_scope_count = 0;

void ui_rssi_scope_reset(void)
{
    memset(s_scope_hist, -100, sizeof(s_scope_hist));
    s_scope_head = 0;
    s_scope_count = 0;
}

void ui_draw_rssi_scope(int x, int y, int w, int h, int rssi_dbm)
{
    if (w > SCOPE_MAX_W) w = SCOPE_MAX_W;
    auto &d = M5Cardputer.Display;

    /* Push newest sample. */
    if (rssi_dbm < -100) rssi_dbm = -100;
    if (rssi_dbm > -20)  rssi_dbm = -20;
    s_scope_hist[s_scope_head] = (int8_t)rssi_dbm;
    s_scope_head = (s_scope_head + 1) % w;
    if (s_scope_count < w) s_scope_count++;

    /* Background. */
    d.fillRect(x, y, w, h, T_BG);
    d.drawRect(x - 1, y - 1, w + 2, h + 2, 0x2945);
    /* Horizontal tier lines: -80, -60, -40 dBm. */
    for (int dbm = -80; dbm <= -40; dbm += 20) {
        int ty = y + h - ((dbm + 100) * h / 80);
        d.drawFastHLine(x, ty, w, 0x1082);
    }

    /* Draw time-series bars, oldest-left → newest-right. */
    int start = (s_scope_head - s_scope_count + w) % w;
    for (int i = 0; i < s_scope_count; ++i) {
        int idx = (start + i) % w;
        int v = s_scope_hist[idx];
        int bar_h = ((v + 100) * h) / 80;
        if (bar_h < 1) bar_h = 1;
        int bx = x + i;
        int by = y + h - bar_h;
        uint16_t col;
        int pct = v + 100;
        if (pct < 24)      col = T_BAD;
        else if (pct < 56) col = T_WARN;
        else               col = T_GOOD;
        d.drawFastVLine(bx, by, bar_h, col);
        /* Accent pixel at the tip for glow. */
        d.drawPixel(bx, by, 0xFFFF);
    }

    /* Current RSSI readout lower-right. */
    d.setTextColor(T_FG, T_BG);
    d.setCursor(x + w - 42, y + h - 10);
    char buf[12];
    snprintf(buf, sizeof(buf), "%4ddBm", rssi_dbm);
    d.print(buf);
}

/* ---- LIVE TX splash ---- */

void ui_subghz_live_tx_splash(float freq_mhz,
                              const char *protocol,
                              uint32_t payload_hex,
                              uint32_t duration_ms)
{
    auto &d = M5Cardputer.Display;
    int cx = SCR_W / 2;
    int cy = SCR_H / 2;
    uint32_t start = millis();

    char head[48];
    snprintf(head, sizeof(head), ">> %.3f MHz <<", freq_mhz);

    while (millis() - start < duration_ms) {
        uint32_t t = millis() - start;
        float phase = t / 60.0f;

        /* Clear body only (leave status + footer alone). */
        d.fillRect(0, BODY_Y, SCR_W, SCR_H - BODY_Y, 0);

        /* Concentric expanding rings — 3 phases offset. */
        for (int r = 0; r < 3; ++r) {
            int radius = (int)(phase * 4 + r * 18) % 90;
            uint8_t alpha = 255 - (radius * 3);
            uint16_t col = rgb565(alpha, 0, alpha);
            if (radius > 3) d.drawCircle(cx, cy, radius, col);
            if (radius > 5) d.drawCircle(cx, cy, radius - 1, col);
        }

        /* Radial spokes — 8 beams rotating. */
        for (int s = 0; s < 8; ++s) {
            float ang = phase * 0.05f + (s * 3.14159f / 4);
            int ex = cx + (int)(cos(ang) * 60);
            int ey = cy + (int)(sin(ang) * 30);
            d.drawLine(cx, cy, ex, ey, 0x780F);
        }

        /* Center: title + protocol + payload hex ticker. */
        d.setTextColor(0xFFE0, 0);  /* yellow */
        int hw = (int)strlen(head) * 6;
        d.setCursor(cx - hw / 2, cy - 18);
        d.print(head);

        d.setTextColor(0x07FF, 0);  /* cyan */
        if (protocol && *protocol) {
            int pw = (int)strlen(protocol) * 6;
            d.setCursor(cx - pw / 2, cy - 4);
            d.print(protocol);
        }

        d.setTextColor(0xFFFF, 0);
        char hex[16];
        snprintf(hex, sizeof(hex), "0x%08lX", (unsigned long)payload_hex);
        int xw = (int)strlen(hex) * 6;
        d.setCursor(cx - xw / 2, cy + 8);
        d.print(hex);

        /* Bottom-center "LIVE" blink. */
        if ((t / 150) & 1) {
            d.setTextColor(0xF800, 0);  /* red */
            d.setCursor(cx - 12, SCR_H - 20);
            d.print("LIVE");
        }

        delay(30);
    }

    /* Clear splash aftermath so the next redraw starts fresh. */
    d.fillRect(0, BODY_Y, SCR_W, SCR_H - BODY_Y, T_BG);
}
