/*
 * subghz_spectrum.cpp — professional spectrum analyzer + waterfall.
 *
 * Three visualization modes with polished rendering:
 *   1. Bar spectrum: gradient bars + peak hold + grid + dBm scale
 *   2. Waterfall: scrolling heatmap spectrogram
 *   3. Waveform: live oscilloscope with grid + trigger level
 */
#include "../app.h"
#include "../theme.h"
#include "../ui.h"
#include "../input.h"
#include "../radio.h"
#include "../cc1101_hw.h"
#include <ELECHOUSE_CC1101_SRC_DRV.h>

struct freq_range_t { float start; float end; const char *name; };
static const freq_range_t RANGES[] = {
    { 300.0f, 348.0f, "300-348" },
    { 387.0f, 464.0f, "387-464" },
    { 779.0f, 928.0f, "779-928" },
};
#define RANGE_COUNT 3

/* Smooth gradient: deep blue → cyan → green → yellow → red. */
static uint16_t rssi_color(int rssi)
{
    int n = rssi + 110;
    if (n < 0) n = 0;
    if (n > 80) n = 80;
    int p = (n * 255) / 80;
    auto &d = M5Cardputer.Display;
    if (p < 50)  return d.color565(0, 0, 40 + p * 2);
    if (p < 100) return d.color565(0, (p - 50) * 5, 140 - (p - 50) * 2);
    if (p < 170) return d.color565((p - 100) * 3, 255, 0);
    return d.color565(255, 255 - (p - 170) * 3, 0);
}

/* ---- Bar Spectrum ---- */

static void run_bar_spectrum(const freq_range_t &range)
{
    auto &d = M5Cardputer.Display;
    const int GX = 24, GY = BODY_Y + 14, GW = SCR_W - 30, GH = BODY_H - 30;
    int bins = GW;
    float step = (range.end - range.start) / bins;
    int8_t rssi[232] = {0};
    int8_t peak[232] = {0};
    memset(peak, -110, sizeof(peak));

    ui_clear_body();

    while (true) {
        for (int i = 0; i < bins; ++i) {
            cc1101_set_freq(range.start + i * step);
            delayMicroseconds(800);
            rssi[i] = (int8_t)cc1101_get_rssi();
            if (rssi[i] > peak[i]) peak[i] = rssi[i];
        }
        for (int i = 0; i < bins; ++i)
            if (peak[i] > rssi[i] + 1) peak[i]--;

        /* Graph background. */
        d.fillRect(GX, GY, GW, GH, 0x0000);

        /* Grid lines + dBm labels. */
        for (int db = -100; db <= -40; db += 20) {
            int y = GY + GH - ((db + 110) * GH) / 80;
            if (y >= GY && y <= GY + GH) {
                for (int x = GX; x < GX + GW; x += 4)
                    d.drawPixel(x, y, 0x2104);
                d.setTextColor(0x4208, T_BG);
                d.setCursor(1, y - 3);
                d.printf("%d", db);
            }
        }

        /* Bars with gradient fill. */
        for (int i = 0; i < bins; ++i) {
            int norm = rssi[i] + 110;
            if (norm < 0) norm = 0;
            int h = (norm * GH) / 80;
            if (h > 0) {
                for (int dy = 0; dy < h && dy < GH; ++dy) {
                    int fake_rssi = -110 + ((h - dy) * 80) / GH;
                    d.drawPixel(GX + i, GY + GH - 1 - dy, rssi_color(fake_rssi));
                }
            }
            /* Peak marker — white dot with fade. */
            int pn = peak[i] + 110;
            if (pn > 0) {
                int py = GY + GH - (pn * GH) / 80;
                if (py >= GY) d.drawPixel(GX + i, py, 0xFFFF);
            }
        }

        /* Axis frame. */
        d.drawRect(GX - 1, GY - 1, GW + 2, GH + 2, 0x4208);

        /* Title + freq labels. */
        ui_text(4, BODY_Y + 2, T_ACCENT, "SPECTRUM %s MHz", range.name);
        d.setTextColor(0x4208, T_BG);
        d.setCursor(GX, GY + GH + 3);
        d.printf("%.0f", range.start);
        d.setCursor(GX + GW - 24, GY + GH + 3);
        d.printf("%.0f", range.end);

        /* Peak freq indicator. */
        int peak_i = 0, peak_v = -120;
        for (int i = 0; i < bins; ++i) {
            if (rssi[i] > peak_v) { peak_v = rssi[i]; peak_i = i; }
        }
        float peak_f = range.start + peak_i * step;
        d.setTextColor(T_FG, T_BG);
        d.setCursor(GX + GW / 2 - 36, GY + GH + 3);
        d.printf("pk:%.1f %ddB", peak_f, peak_v);

        ui_draw_footer("ESC=back  R=reset peaks");

        uint16_t k = input_poll();
        if (k == PK_ESC) return;
        if (k == 'r' || k == 'R') memset(peak, -110, sizeof(peak));
    }
}

/* ---- Waterfall / Spectrogram ---- */

#define WF_MAX_ROWS 40
#define WF_MAX_BINS 210

static void run_waterfall(const freq_range_t &range)
{
    auto &d = M5Cardputer.Display;
    const int GX = 24, GY = BODY_Y + 14, GW = WF_MAX_BINS, GH = WF_MAX_ROWS;
    float step = (range.end - range.start) / GW;

    uint16_t *ring = (uint16_t *)malloc(GH * GW * sizeof(uint16_t));
    if (!ring) { ui_toast("OOM", T_BAD, 1000); return; }
    int head = 0, count = 0;

    ui_clear_body();
    d.drawRect(GX - 1, GY - 1, GW + 2, GH + 2, 0x4208);

    /* Color legend. */
    for (int i = 0; i < 40; ++i) {
        int r = -110 + (i * 80) / 40;
        d.drawPixel(GX + GW + 3, GY + GH - 1 - i, rssi_color(r));
    }
    d.setTextColor(0x4208, T_BG);
    d.setCursor(GX + GW + 2, GY - 2); d.print("H");
    d.setCursor(GX + GW + 2, GY + GH - 6); d.print("L");

    while (true) {
        uint16_t *row = &ring[head * GW];
        for (int i = 0; i < GW; ++i) {
            cc1101_set_freq(range.start + i * step);
            delayMicroseconds(500);
            row[i] = rssi_color(cc1101_get_rssi());
        }
        head = (head + 1) % GH;
        if (count < GH) count++;

        /* Render from ring — newest at bottom. */
        for (int r = 0; r < count; ++r) {
            int ri = (head - count + r + GH) % GH;
            d.pushImage(GX, GY + r, GW, 1, &ring[ri * GW]);
        }

        ui_text(4, BODY_Y + 2, T_ACCENT2, "WATERFALL %s MHz", range.name);
        d.setTextColor(0x4208, T_BG);
        d.setCursor(GX, GY + GH + 3); d.printf("%.0f", range.start);
        d.setCursor(GX + GW - 24, GY + GH + 3); d.printf("%.0f", range.end);
        ui_draw_footer("ESC=back");

        uint16_t k = input_poll();
        if (k == PK_ESC) { free(ring); return; }
    }
}

/* ---- Oscilloscope Waveform ---- */

static void run_waveform(float freq)
{
    auto &d = M5Cardputer.Display;
    cc1101_set_freq(freq);
    cc1101_set_rx();

    const int GX = 24, GY = BODY_Y + 18, GW = SCR_W - 30, GH = BODY_H - 34;
    int mid = GY + GH / 2;
    int8_t history[232];
    memset(history, 0, sizeof(history));

    ui_clear_body();

    while (true) {
        /* Sample RSSI across the display width. */
        for (int i = 0; i < GW; ++i) {
            history[i] = (int8_t)cc1101_get_rssi();
            delayMicroseconds(80);
        }

        /* Graph background + grid. */
        d.fillRect(GX, GY, GW, GH, 0x0000);
        d.drawRect(GX - 1, GY - 1, GW + 2, GH + 2, 0x4208);

        /* Horizontal grid at -80, -60, -40 dBm. */
        for (int db = -80; db <= -40; db += 20) {
            int y = mid - ((db + 80) * (GH / 2)) / 40;
            if (y >= GY && y <= GY + GH) {
                for (int x = GX; x < GX + GW; x += 6)
                    d.drawPixel(x, y, 0x2104);
                d.setTextColor(0x4208, T_BG);
                d.setCursor(1, y - 3); d.printf("%d", db);
            }
        }
        /* Center line. */
        for (int x = GX; x < GX + GW; x += 3)
            d.drawPixel(x, mid, 0x3186);

        /* Filled waveform with gradient. */
        for (int i = 1; i < GW; ++i) {
            int n1 = history[i - 1] + 110, n2 = history[i] + 110;
            if (n1 < 0) n1 = 0; if (n2 < 0) n2 = 0;
            int y1 = GY + GH - (n1 * GH) / 80;
            int y2 = GY + GH - (n2 * GH) / 80;
            /* Line connecting samples. */
            uint16_t c = rssi_color(history[i]);
            d.drawLine(GX + i - 1, y1, GX + i, y2, c);
            /* Filled area to bottom with dim version. */
            int fill_y = (y2 < GY + GH) ? y2 : GY + GH;
            if (fill_y < GY + GH)
                d.drawFastVLine(GX + i, fill_y, GY + GH - fill_y, rssi_color(history[i] - 20));
        }

        /* Title + current readings. */
        ui_text(4, BODY_Y + 2, T_ACCENT, "SCOPE %.3f MHz", freq);
        int cur = history[GW / 2];
        d.setTextColor(cur > -60 ? T_GOOD : T_DIM, T_BG);
        d.setCursor(SCR_W - 54, BODY_Y + 2);
        d.printf("%d dBm", cur);

        ui_draw_footer("+-=freq  ESC=back");

        uint32_t t = millis();
        while (millis() - t < 60) {
            uint16_t k = input_poll();
            if (k == PK_ESC) return;
            if (k == '+' || k == '=') { freq += 0.5f; cc1101_set_freq(freq); }
            if (k == '-') { freq -= 0.5f; cc1101_set_freq(freq); }
        }
    }
}

/* ---- Mode/range picker ---- */

void feat_subghz_spectrum(void)
{
    radio_switch(RADIO_SUBGHZ);
    if (!cc1101_begin(433.0f)) {
        ui_toast("CC1101 not found", T_BAD, 1500);
        radio_switch(RADIO_NONE);
        return;
    }

    auto &d = M5Cardputer.Display;
    int mode = 0, range = 1;
    const char *modes[] = { "Bar Spectrum", "Waterfall", "Oscilloscope" };
    const char *descs[] = {
        "Gradient bars + peak hold + dBm grid",
        "Scrolling color heatmap spectrogram",
        "Live RSSI waveform with filled area"
    };

    while (true) {
        ui_clear_body();
        d.setTextColor(T_ACCENT2, T_BG);
        d.setCursor(4, BODY_Y + 2); d.print("SIGNAL ANALYZER");
        d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT2);

        for (int i = 0; i < 3; ++i) {
            int y = BODY_Y + 20 + i * 22;
            bool s = (i == mode);
            if (s) {
                d.fillRoundRect(4, y - 2, SCR_W - 8, 20, 3, 0x18C3);
                d.drawRoundRect(4, y - 2, SCR_W - 8, 20, 3, T_ACCENT);
            }
            d.setTextColor(s ? T_ACCENT : T_FG, s ? 0x18C3 : T_BG);
            d.setCursor(10, y); d.printf("%s", modes[i]);
            d.setTextColor(s ? 0x07FF : T_DIM, s ? 0x18C3 : T_BG);
            d.setCursor(10, y + 10); d.printf("%s", descs[i]);
        }

        d.setTextColor(T_DIM, T_BG);
        d.setCursor(4, BODY_Y + 90);
        d.printf("band: %s MHz  (TAB to change)", RANGES[range].name);
        ui_draw_footer(";/.=mode  TAB=band  ENTER=go  ESC=quit");

        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) break;
        if (k == ';' || k == PK_UP)   mode = (mode + 2) % 3;
        if (k == '.' || k == PK_DOWN) mode = (mode + 1) % 3;
        if (k == '\t') range = (range + 1) % RANGE_COUNT;
        if (k == PK_ENTER) {
            if (mode == 0) run_bar_spectrum(RANGES[range]);
            else if (mode == 1) run_waterfall(RANGES[range]);
            else run_waveform(RANGES[range].start + (RANGES[range].end - RANGES[range].start) / 2);
            ui_clear_body();
        }
    }

    cc1101_end();
    radio_switch(RADIO_NONE);
}
