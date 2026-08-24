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
#include <esp_heap_caps.h>
#include <math.h>
#include "../board/leds_tembed.h"

struct freq_range_t { float start; float end; const char *name; };
static const freq_range_t RANGES[] = {
    { 426.0f, 440.0f, "433 MHz" },   /* default — centers on 433 (ISM band) */
    { 300.0f, 348.0f, "300-348" },
    { 387.0f, 464.0f, "387-464" },
    { 779.0f, 928.0f, "779-928" },
};
#define RANGE_COUNT 4

/* OLED Neon Rose heat ramp: black -> indigo -> magenta -> neon rose ->
 * hot pink/white. Replaces the generic blue->red so every analyzer mode
 * reads as POSEIDON's own gear. rssi is dBm ~[-110,-30]. */
static uint16_t rssi_color(int rssi)
{
    int n = rssi + 110;
    if (n < 0) n = 0;
    if (n > 80) n = 80;
    int p = (n * 255) / 80;                 /* 0..255 */
    auto &d = M5Cardputer.Display;
    if (p < 60)  return d.color565(p / 2, 0, 30 + p);                          /* black -> indigo */
    if (p < 130) return d.color565(30 + (p - 60) * 3, 0, 120);                 /* indigo -> magenta */
    if (p < 200) return d.color565(238, (p - 130) / 2, 120 + (p - 130) / 3);   /* magenta -> neon rose */
    return d.color565(255, 60 + (p - 200) * 3, 150 + (p - 200));               /* rose -> hot pink/white */
}

/* ---- Bar Spectrum ---- */

static void run_bar_spectrum(const freq_range_t &range)
{
    /* Double-buffered render: build the frame in a sprite, blit once.
     * Eliminates the tearing/flicker of the prior fillRect+redraw loop.
     * Plus EMA smoothing on RSSI values so bars don't jitter between
     * reads of the same signal (RSSI is noisy at ~1 dB precision). */
    auto &d = M5Cardputer.Display;
    const int GX = 24, GY = BODY_Y + 14, GW = SCR_W - 30, GH = BODY_H - 30;
    int bins = GW;
    float step = (range.end - range.start) / bins;

    // Sized to SCR_W so bins (= GW = SCR_W-30) always fits, incl. the 320px T-Embed build.
    float   smooth[SCR_W];
    int8_t  peak[SCR_W];
    for (int i = 0; i < bins; ++i) { smooth[i] = -110.0f; peak[i] = -110; }

    M5Canvas canvas(&d);
    canvas.setColorDepth(16);
    bool have_canvas = canvas.createSprite(GW, GH);

    ui_clear_body();
    /* Static chrome — drawn ONCE, not every frame. */
    d.drawRect(GX - 1, GY - 1, GW + 2, GH + 2, 0x4208);
    ui_text(4, BODY_Y + 2, T_ACCENT, "SPECTRUM %s MHz", range.name);
    d.setTextColor(0x4208, T_BG);
    d.setCursor(GX, GY + GH + 3);             d.printf("%.0f", range.start);
    d.setCursor(GX + GW - 24, GY + GH + 3);   d.printf("%.0f", range.end);
    ui_draw_footer("ESC=back  R=reset peaks");

    while (true) {
        for (int i = 0; i < bins; ++i) {
            cc1101_set_freq(range.start + i * step); cc1101_set_rx();
            delayMicroseconds(800);
            int raw = cc1101_get_rssi();
            /* Lighter EMA (50/50) so the live bar tracks bursts, but PEAK-HOLD ON
             * THE RAW SAMPLE — the sweep sits on any given bin only briefly, so a
             * short fob burst shows up in a single raw read; peak-holding the
             * smoothed value (old code) averaged it away to near-invisible. */
            smooth[i] = smooth[i] * 0.5f + raw * 0.5f;
            if (raw > peak[i]) peak[i] = (int8_t)raw;
        }
        /* Peak decay toward the live floor — slow (2 dB/sweep) so a caught burst
         * lingers a beat instead of vanishing before the eye catches it. */
        for (int i = 0; i < bins; ++i)
            if (peak[i] > (int)smooth[i]) peak[i] -= 2;

        if (have_canvas) {
            /* All rendering goes into the off-screen canvas. */
            canvas.fillSprite(0x0000);

            /* Grid lines (relative to canvas origin 0,0). */
            for (int db = -100; db <= -40; db += 20) {
                int y = GH - ((db + 110) * GH) / 80;
                if (y >= 0 && y < GH)
                    for (int x = 0; x < GW; x += 4)
                        canvas.drawPixel(x, y, 0x2104);
            }
            /* Bars. */
            for (int i = 0; i < bins; ++i) {
                int norm = (int)smooth[i] + 110;
                if (norm < 0) norm = 0;
                int h = (norm * GH) / 80;
                if (h > 0) {
                    for (int dy = 0; dy < h && dy < GH; ++dy) {
                        int fake_rssi = -110 + ((h - dy) * 80) / GH;
                        canvas.drawPixel(i, GH - 1 - dy, rssi_color(fake_rssi));
                    }
                }
                int pn = peak[i] + 110;
                if (pn > 0) {
                    int py = GH - (pn * GH) / 80;
                    /* Bright 2px peak marker so a held-peak spike is unmissable. */
                    if (py >= 0)        canvas.drawPixel(i, py, 0xFFFF);
                    if (py + 1 < GH)    canvas.drawPixel(i, py + 1, 0xFFFF);
                }
            }
            canvas.pushSprite(GX, GY);
        } else {
            /* Fallback path if sprite allocation failed — flickers but
             * still works. Matches the old behaviour. */
            d.fillRect(GX, GY, GW, GH, 0x0000);
            for (int db = -100; db <= -40; db += 20) {
                int y = GY + GH - ((db + 110) * GH) / 80;
                for (int x = GX; x < GX + GW; x += 4)
                    d.drawPixel(x, y, 0x2104);
            }
            for (int i = 0; i < bins; ++i) {
                int norm = (int)smooth[i] + 110; if (norm < 0) norm = 0;
                int h = (norm * GH) / 80;
                for (int dy = 0; dy < h; ++dy) {
                    int fake_rssi = -110 + ((h - dy) * 80) / GH;
                    d.drawPixel(GX + i, GY + GH - 1 - dy, rssi_color(fake_rssi));
                }
            }
        }

        /* dB labels drawn outside the sprite (left gutter). */
        for (int db = -100; db <= -40; db += 20) {
            int y = GY + GH - ((db + 110) * GH) / 80;
            d.setTextColor(0x4208, T_BG);
            d.setCursor(1, y - 3); d.printf("%d", db);
        }

        /* Peak freq + RSSI tag — update in place over fixed-width area
         * to avoid flicker from changing text length. */
        int peak_i = 0; float peak_v = -120.0f;
        for (int i = 0; i < bins; ++i)
            if (smooth[i] > peak_v) { peak_v = smooth[i]; peak_i = i; }
        float peak_f = range.start + peak_i * step;
        d.fillRect(GX + GW / 2 - 40, GY + GH + 3, 82, 8, T_BG);
        d.setTextColor(T_FG, T_BG);
        d.setCursor(GX + GW / 2 - 36, GY + GH + 3);
        d.printf("pk:%.1f %ddB", peak_f, (int)peak_v);

        uint16_t k = input_poll();
        if (k == PK_ESC) {
            if (have_canvas) canvas.deleteSprite();
            return;
        }
        if (k == 'r' || k == 'R')
            for (int i = 0; i < bins; ++i) peak[i] = -110;
    }
}

/* ---- Waterfall / Spectrogram ---- */

/* POS-AUDIT-246 / rf-017: ring buffer is now lazy-allocated on entry
 * and freed on exit so the 28.8 KB doesn't sit in BSS for the lifetime
 * of every session — only the user who opens the waterfall pays for
 * it. 60 rows × 240 cols × 2 B = 28800 B; heap_caps_malloc with
 * MALLOC_CAP_INTERNAL forces internal SRAM (we don't have PSRAM and
 * the renderer's pushImage path expects internal-RAM pixels). */
/* Full-screen waterfall. The ring MUST live in internal RAM (the pushImage
 * path needs internal-RAM pixels), and a full-height 1px-per-row buffer
 * (~120 rows x 240 x 2 B = ~57 KB) won't allocate when internal SRAM is
 * tight. So pick the best vertical resolution the heap allows and render
 * each ring row N px tall — it ALWAYS fills the screen, just chunkier if
 * memory is short. {rows, vscale} → rows*vscale ≈ 120 px plot height. */
#define WF_MAX_BINS SCR_W

static void run_waterfall(const freq_range_t &range)
{
    auto &d = M5Cardputer.Display;

    const int GX = 0, GY = 13, GW = WF_MAX_BINS;

    /* Plot height must come from the PANEL, not a constant. The old table
     * {120,60,40} was sized for the Cardputer's 135 px screen; on the T-Embed
     * (170 px, no footer) it left a dead band at the bottom of the waterfall
     * and pushed the axis labels away from the plot edge. Derive the row count
     * from the space actually available below the header, trying successively
     * coarser vertical scales until an allocation fits. */
    const int avail = SCR_H - GY;
    int rows = 0, vscale = 1;
    uint16_t *ring = nullptr;
    for (int vs = 1; vs <= 3 && !ring; ++vs) {
        const int r = avail / vs;
        if (r <= 0) continue;
        ring = (uint16_t *)heap_caps_malloc(
            (size_t)r * WF_MAX_BINS * sizeof(uint16_t), MALLOC_CAP_INTERNAL);
        if (ring) { rows = r; vscale = vs; }
    }
    if (!ring) { ui_toast("OOM", T_BAD, 1000); return; }

    const int plot_h = rows * vscale;
    float step = (range.end - range.start) / GW;
    memset(ring, 0, (size_t)rows * WF_MAX_BINS * sizeof(uint16_t));
    int head = 0, count = 0;
    /* Per-column peak-hold with slow decay: the sweep sits on any bin only
     * briefly, so a short fob burst caught in one sweep would be a single stray
     * pixel. Holding the max per column (decaying 1/sweep) turns it into a
     * visible vertical streak. */
    static int8_t peakcol[WF_MAX_BINS];
    for (int i = 0; i < GW; ++i) peakcol[i] = -110;

    d.fillScreen(T_BG);

    /* Title carries the band so the plot owns the whole area below it. */
    d.setTextColor(T_ACCENT2, T_BG);
    d.setCursor(4, 3); d.printf("WATERFALL %s  %.0f-%.0f", range.name, range.start, range.end);
    d.setTextColor(T_BAD, T_BG);
    d.setCursor(SCR_W - 22, 3); d.print("ESC");

    while (true) {
        /* Sweep one full row of freq bins. */
        uint16_t *row = &ring[head * GW];
        for (int i = 0; i < GW; ++i) {
            cc1101_set_freq(range.start + i * step); cc1101_set_rx();
            delayMicroseconds(500);
            int raw = cc1101_get_rssi();
            if (raw > peakcol[i]) peakcol[i] = (int8_t)raw;      /* hold the burst */
            else if (peakcol[i] > -110) peakcol[i]--;            /* slow decay */
            row[i] = rssi_color(peakcol[i]);
        }
        head = (head + 1) % rows;
        if (count < rows) count++;

        /* Newest row at the bottom; each ring row drawn vscale px tall so
         * the spectrogram fills the full screen height. */
        for (int r = 0; r < count; ++r) {
            int ri = (head - count + r + rows) % rows;
            int y = GY + r * vscale;
            for (int s = 0; s < vscale; ++s)
                d.pushImage(GX, y + s, GW, 1, &ring[ri * GW]);
        }
        (void)plot_h;

        uint16_t k = input_poll();
        if (k == PK_ESC) { free(ring); return; }
    }
}

/* ---- Oscilloscope Waveform ---- */

static void run_waveform(float freq)
{
    /* Digital-scope style: top half is RSSI trace (thin line, scrolling
     * right-to-left like a real scope — no filled bars, to differentiate
     * from the spectrum analyzer). Bottom half is a GDO0 pulse train —
     * the actual demodulated data line from the CC1101. That's what
     * shows activity when a car key transmits: a burst of square-wave
     * pulses even though RSSI only nudges. */
    auto &d = M5Cardputer.Display;
    cc1101_set_freq(freq); cc1101_set_rx();
    cc1101_set_rx();

    const int GX = 24;
    const int GW = SCR_W - 30;
    const int RSSI_Y = BODY_Y + 14;
    const int RSSI_H = 40;
    const int GDO_Y  = RSSI_Y + RSSI_H + 8;
    const int GDO_H  = 20;

    int8_t  rssi_ring[SCR_W]; // sized to SCR_W so head % GW (GW = SCR_W-30) never overruns
    uint8_t gdo_ring[SCR_W];
    memset(rssi_ring, -110, sizeof(rssi_ring));
    memset(gdo_ring, 0, sizeof(gdo_ring));
    int head = 0;

    ui_clear_body();
    /* Static chrome drawn once — borders + labels outside the canvas. */
    d.drawRect(GX - 1, RSSI_Y - 1, GW + 2, RSSI_H + 2, 0x2945);
    d.drawRect(GX - 1, GDO_Y - 1,  GW + 2, GDO_H + 2,  0x2945);
    d.setTextColor(0x4208, T_BG);
    d.setCursor(1, RSSI_Y - 4);             d.print("-30");
    d.setCursor(1, RSSI_Y + RSSI_H - 6);    d.print("-110");
    d.setCursor(1, GDO_Y + GDO_H / 2 - 3);  d.print("GDO0");
    ui_draw_footer("+-=freq  ESC=back");

    /* Double-buffer both panels into one sprite. Panels are stacked
     * vertically with a 6 px gap — sprite covers rssi_panel + gap + gdo_panel. */
    const int CANVAS_H = RSSI_H + 8 + GDO_H;
    M5Canvas canvas(&d);
    canvas.setColorDepth(16);
    bool have_canvas = canvas.createSprite(GW, CANVAS_H);

    while (true) {
        /* One scroll tick: sample N fresh points, advance head. */
        const int BATCH = 8;
        for (int k = 0; k < BATCH; ++k) {
            /* Read GDO0 a few times inside one pixel-column to catch
             * fast pulses — store 1 if we saw any high in this window. */
            uint8_t seen_high = 0;
            for (int s = 0; s < 8; ++s) {
                if (digitalRead(CC1101_GDO0)) seen_high = 1;
                delayMicroseconds(12);
            }
            rssi_ring[head] = (int8_t)cc1101_get_rssi();
            gdo_ring[head]  = seen_high;
            head = (head + 1) % GW;
        }

        if (have_canvas) {
            canvas.fillSprite(0x0000);

            /* RSSI panel occupies sprite rows 0..RSSI_H-1. */
            int prev_y = RSSI_H - 1;
            for (int i = 0; i < GW; ++i) {
                int idx = (head + i) % GW;
                int n = rssi_ring[idx] + 110; if (n < 0) n = 0; if (n > 80) n = 80;
                int y = RSSI_H - 1 - (n * (RSSI_H - 1)) / 80;
                if (i > 0) canvas.drawLine(i - 1, prev_y, i, y, 0x07E0);
                prev_y = y;
            }
            /* GDO0 panel at sprite rows RSSI_H+8 .. RSSI_H+8+GDO_H-1. */
            const int GDO_Y0 = RSSI_H + 8;
            for (int i = 0; i < GW; ++i) {
                int idx = (head + i) % GW;
                if (gdo_ring[idx])
                    canvas.drawFastVLine(i, GDO_Y0 + 2, GDO_H - 4, 0x07FF);
            }
            canvas.pushSprite(GX, RSSI_Y);
        } else {
            /* Fallback without canvas. */
            d.fillRect(GX, RSSI_Y, GW, RSSI_H, 0x0000);
            d.fillRect(GX, GDO_Y,  GW, GDO_H,  0x0000);
            int prev_y = RSSI_Y + RSSI_H - 1;
            for (int i = 0; i < GW; ++i) {
                int idx = (head + i) % GW;
                int n = rssi_ring[idx] + 110; if (n < 0) n = 0; if (n > 80) n = 80;
                int y = RSSI_Y + RSSI_H - (n * RSSI_H) / 80;
                if (i > 0) d.drawLine(GX + i - 1, prev_y, GX + i, y, 0x07E0);
                prev_y = y;
            }
            for (int i = 0; i < GW; ++i) {
                int idx = (head + i) % GW;
                if (gdo_ring[idx])
                    d.drawFastVLine(GX + i, GDO_Y + 2, GDO_H - 4, 0x07FF);
            }
        }

        /* Header text — draw in fixed-width regions on main display to
         * avoid re-rendering in the sprite. */
        int cur_rssi = rssi_ring[(head + GW - 1) % GW];
        int pulse_count = 0;
        for (int i = 0; i < GW; ++i) pulse_count += gdo_ring[i];

        d.fillRect(4, BODY_Y + 2, 120, 8, T_BG);
        d.setTextColor(T_ACCENT, T_BG);
        d.setCursor(4, BODY_Y + 2); d.printf("SCOPE %.3f MHz", freq);
        d.fillRect(SCR_W - 60, BODY_Y + 2, 58, 8, T_BG);
        d.setTextColor(cur_rssi > -60 ? T_GOOD : T_DIM, T_BG);
        d.setCursor(SCR_W - 54, BODY_Y + 2);
        d.printf("%d dBm", cur_rssi);

        /* Pulse counter — fade from dim to cyan based on activity. */
        d.fillRect(4, SCR_H - 22, 120, 8, T_BG);
        d.setTextColor(pulse_count > 4 ? 0x07FF : T_DIM, T_BG);
        d.setCursor(4, SCR_H - 22);
        d.printf("GDO0 pulses: %3d", pulse_count);

        /* Poll input + cadence. */
        uint32_t t = millis();
        while (millis() - t < 20) {
            uint16_t k = input_poll();
            if (k == PK_ESC) {
                if (have_canvas) canvas.deleteSprite();
                return;
            }
            if (k == '+' || k == '=') { freq += 0.5f; cc1101_set_freq(freq); cc1101_set_rx(); }
            if (k == '-')             { freq -= 0.5f; cc1101_set_freq(freq); cc1101_set_rx(); }
        }
    }
}

/* ---- Peak-Hold Spectrum Analyzer ---- */

static void run_peak_hold(const freq_range_t &range)
{
    auto &d = M5Cardputer.Display;
    cc1101_set_rx();

    const uint16_t ROSE = d.color565(0xEE, 0x22, 0x90);
    const uint16_t CYAN = d.color565(0x22, 0xD3, 0xEE);
    const uint16_t GRID = d.color565(0x30, 0x0A, 0x20);

    const int GX = 22;
    const int GY = 28;
    const int GW = SCR_W - GX - 2;
    const int GH = SCR_H - GY - 10;
    const int BINS = GW;

    const int DB_LO = -110, DB_HI = -30, DB_SPAN = DB_HI - DB_LO;

    float  smooth[GW > 0 ? GW : 1];
    float  peak[GW > 0 ? GW : 1];
    for (int i = 0; i < BINS; ++i) { smooth[i] = DB_LO; peak[i] = DB_LO; }

    float step = (range.end - range.start) / BINS;

    M5Canvas canvas(&d);
    canvas.setColorDepth(16);
    bool have_canvas = canvas.createSprite(GW, GH);

    d.fillScreen(T_BG);
    d.setTextColor(ROSE, T_BG);
    d.setCursor(4, 3); d.printf("PEAK-HOLD %s MHz", range.name);
    d.setTextColor(T_BAD, T_BG);
    d.setCursor(SCR_W - 22, 3); d.print("ESC");

    d.drawRect(GX - 2, GY - 2, GW + 4, GH + 4, GRID);
    d.drawRect(GX - 1, GY - 1, GW + 2, GH + 2, ROSE);

    d.setTextColor(d.color565(0x60, 0x18, 0x40), T_BG);
    for (int db = -90; db <= -50; db += 20) {
        int y = GY + GH - 1 - ((db - DB_LO) * (GH - 1)) / DB_SPAN;
        d.setCursor(0, y - 3); d.printf("%d", db);
    }

    d.setTextColor(CYAN, T_BG);
    d.setCursor(GX, GY + GH + 2);            d.printf("%.0f", range.start);
    { char buf[12]; int n = snprintf(buf, sizeof(buf), "%.0f", range.end);
      d.setCursor(GX + GW - n * 6, GY + GH + 2); d.print(buf); }

    while (true) {
        for (int i = 0; i < BINS; ++i) {
            cc1101_set_freq(range.start + i * step); cc1101_set_rx();
            delayMicroseconds(500);
            int raw = cc1101_get_rssi();
            smooth[i] = smooth[i] * 0.65f + raw * 0.35f;
            if (smooth[i] > peak[i]) peak[i] = smooth[i];
        }
        const float DECAY = (float)DB_SPAN / 40.0f;
        for (int i = 0; i < BINS; ++i) {
            if (peak[i] > smooth[i]) {
                peak[i] -= DECAY;
                if (peak[i] < smooth[i]) peak[i] = smooth[i];
            }
        }

        int   pk_i = 0;
        float pk_v = -200.0f;
        for (int i = 0; i < BINS; ++i)
            if (smooth[i] > pk_v) { pk_v = smooth[i]; pk_i = i; }
        float pk_f = range.start + pk_i * step;

        if (have_canvas) {
            canvas.fillSprite(0x0000);
            for (int db = -90; db <= -50; db += 20) {
                int y = GH - 1 - ((db - DB_LO) * (GH - 1)) / DB_SPAN;
                if (y >= 0 && y < GH)
                    for (int x = 0; x < GW; x += 4) canvas.drawPixel(x, y, GRID);
            }
            if (pk_i >= 0 && pk_i < GW)
                canvas.drawFastVLine(pk_i, 0, GH, d.color565(0x06, 0x33, 0x3A));
            for (int i = 0; i < BINS; ++i) {
                int sn = (int)smooth[i] - DB_LO;
                if (sn < 0) sn = 0; if (sn > DB_SPAN) sn = DB_SPAN;
                int h = (sn * (GH - 1)) / DB_SPAN;
                for (int dy = 0; dy < h; ++dy) {
                    int fake = DB_LO + ((h - dy) * DB_SPAN) / (GH - 1);
                    canvas.drawPixel(i, GH - 1 - dy, rssi_color(fake));
                }
                int pn = (int)peak[i] - DB_LO;
                if (pn < 0) pn = 0; if (pn > DB_SPAN) pn = DB_SPAN;
                int py = GH - 1 - (pn * (GH - 1)) / DB_SPAN;
                if (py >= 0 && py < GH) {
                    canvas.drawPixel(i, py, 0xFFFF);
                    if (py > 0) canvas.drawPixel(i, py - 1, ROSE);
                }
            }
            canvas.pushSprite(GX, GY);
        } else {
            d.fillRect(GX, GY, GW, GH, 0x0000);
            for (int i = 0; i < BINS; ++i) {
                int sn = (int)smooth[i] - DB_LO;
                if (sn < 0) sn = 0; if (sn > DB_SPAN) sn = DB_SPAN;
                int h = (sn * (GH - 1)) / DB_SPAN;
                for (int dy = 0; dy < h; ++dy) {
                    int fake = DB_LO + ((h - dy) * DB_SPAN) / (GH - 1);
                    d.drawPixel(GX + i, GY + GH - 1 - dy, rssi_color(fake));
                }
                int pn = (int)peak[i] - DB_LO;
                if (pn < 0) pn = 0; if (pn > DB_SPAN) pn = DB_SPAN;
                int py = GY + GH - 1 - (pn * (GH - 1)) / DB_SPAN;
                d.drawPixel(GX + i, py, 0xFFFF);
            }
        }

        d.fillRect(GX, 13, GW, 13, T_BG);
        d.setTextColor(ROSE, T_BG);
        d.setTextSize(2);
        d.setCursor(GX, 12);
        d.printf("%d", (int)pk_v);
        d.setTextSize(1);
        d.setTextColor(d.color565(0x80, 0x20, 0x50), T_BG);
        d.print("dBm");
        d.setTextColor(CYAN, T_BG);
        d.setCursor(GX + GW - 78, 16);
        d.printf("%8.3f MHz", pk_f);

        uint16_t k = input_poll();
        if (k == PK_ESC) {
            if (have_canvas) canvas.deleteSprite();
            d.setTextSize(1);
            return;
        }
        if (k == 'r' || k == 'R')
            for (int i = 0; i < BINS; ++i) peak[i] = DB_LO;
    }
}

/* ---- Radar / Threat-Scope (Polar Spectrum) ---- */

static void run_radar(const freq_range_t &range)
{
    auto &d = M5Cardputer.Display;
    cc1101_set_rx();

    const int CX = SCR_H / 2 + 2;
    const int CY = SCR_H / 2 + 6;
    const int R  = SCR_H / 2 - 8;

    const uint16_t ROSE  = d.color565(238, 34, 144);
    const uint16_t ROSE_D = d.color565(70, 10, 44);
    const uint16_t ROSE_DD = d.color565(34, 6, 22);
    const uint16_t CYAN  = d.color565(34, 211, 238);

    const int SECTORS = 180;
    const float TWO_PI_ = 6.2831853f;
    const float ASTEP = TWO_PI_ / SECTORS;

    static int8_t  blip_r[180];
    static int8_t  blip_rssi[180];
    static uint8_t blip_age[180];
    for (int i = 0; i < SECTORS; ++i) { blip_r[i] = 0; blip_rssi[i] = -110; blip_age[i] = 255; }

    float step = (range.end - range.start) / SECTORS;

    d.fillScreen(T_BG);
    d.setTextColor(ROSE, T_BG);
    d.setCursor(4, 3); d.printf("THREAT SCOPE");
    d.setTextColor(CYAN, T_BG);
    d.setCursor(118, 3); d.printf("%s MHz", range.name);
    d.setTextColor(T_BAD, T_BG);
    d.setCursor(SCR_W - 22, 3); d.print("ESC");

    for (int ring = 1; ring <= 4; ++ring) {
        int rr = (R * ring) / 4;
        d.drawCircle(CX, CY, rr, ring == 4 ? ROSE_D : ROSE_DD);
    }
    d.drawLine(CX, CY - R, CX, CY + R, ROSE_DD);
    d.drawLine(CX - R, CY, CX + R, CY, ROSE_DD);
    {
        int dxy = (int)(R * 0.7071f);
        d.drawLine(CX - dxy, CY - dxy, CX + dxy, CY + dxy, ROSE_DD);
        d.drawLine(CX - dxy, CY + dxy, CX + dxy, CY - dxy, ROSE_DD);
    }
    d.fillTriangle(CX - 3, CY - R - 1, CX + 3, CY - R - 1, CX, CY - R + 5, CYAN);

    const int HUDX = CX + R + 6;
    d.drawFastVLine(CX + R + 2, 14, SCR_H - 16, ROSE_DD);

    float sweep = 0.0f;
    int   prev_sector = -1;

    while (true) {
        const int ADV = 3;
        for (int a = 0; a < ADV; ++a) {
            int sec = (int)(sweep / ASTEP) % SECTORS;
            float ang = sec * ASTEP;
            float sa = sinf(ang), ca = cosf(ang);

            for (int rr = 1; rr <= R; ++rr) {
                int x = CX + (int)(rr * sa);
                int y = CY - (int)(rr * ca);
                d.drawPixel(x, y, T_BG);
            }
            for (int ring = 1; ring <= 4; ++ring) {
                int rr = (R * ring) / 4;
                int x = CX + (int)(rr * sa);
                int y = CY - (int)(rr * ca);
                d.drawPixel(x, y, ring == 4 ? ROSE_D : ROSE_DD);
            }

            float mhz = range.start + sec * step;
            cc1101_set_freq(mhz); cc1101_set_rx();
            delayMicroseconds(500);
            int rssi = cc1101_get_rssi();

            int n = rssi + 110; if (n < 0) n = 0; if (n > 80) n = 80;
            int br = (n * R) / 80;
            if (n > 12) {
                blip_r[sec]    = (int8_t)(br > R ? R : br);
                blip_rssi[sec] = (int8_t)rssi;
                blip_age[sec]  = 0;
            }

            sweep += ASTEP;
            if (sweep >= TWO_PI_) sweep -= TWO_PI_;
            prev_sector = sec;
        }

        for (int s = 0; s < SECTORS; ++s) {
            if (blip_r[s] <= 0) continue;
            if (blip_age[s] < 255) blip_age[s]++;
            if (blip_age[s] > 90) {
                float ang = s * ASTEP;
                int x = CX + (int)(blip_r[s] * sinf(ang));
                int y = CY - (int)(blip_r[s] * cosf(ang));
                d.fillCircle(x, y, 2, T_BG);
                blip_r[s] = 0;
                continue;
            }
            float ang = s * ASTEP;
            float sa = sinf(ang), ca = cosf(ang);
            int x = CX + (int)(blip_r[s] * sa);
            int y = CY - (int)(blip_r[s] * ca);

            uint16_t hc = rssi_color(blip_rssi[s]);
            int sz; uint16_t col;
            if (blip_age[s] < 18)       { sz = (blip_r[s] > R * 3 / 4) ? 3 : 2; col = hc; }
            else if (blip_age[s] < 50)  { sz = 1; col = hc; }
            else                        { sz = 1; col = ROSE_D; }

            if (sz >= 2) {
                d.fillCircle(x, y, sz, col);
                if (blip_age[s] < 6 && blip_r[s] > R / 2)
                    d.drawCircle(x, y, sz + 2, CYAN);
            } else {
                d.drawPixel(x, y, col);
            }
        }

        {
            int sec = prev_sector < 0 ? 0 : prev_sector;
            float ang = sec * ASTEP;
            float sa = sinf(ang), ca = cosf(ang);
            int ex = CX + (int)(R * sa);
            int ey = CY - (int)(R * ca);
            d.drawLine(CX, CY, ex, ey, ROSE);
            int hx = CX + (int)((R / 2) * sa);
            int hy = CY - (int)((R / 2) * ca);
            d.drawLine(CX, CY, hx, hy, CYAN);
            d.fillCircle(ex, ey, 2, ROSE);
        }
        d.fillCircle(CX, CY, 3, ROSE);
        d.drawCircle(CX, CY, 3, CYAN);

        {
            int sec = prev_sector < 0 ? 0 : prev_sector;
            float mhz = range.start + sec * step;
            int rssi = blip_rssi[sec];
            int bearing = (sec * 360) / SECTORS;

            d.fillRect(HUDX, 18, SCR_W - HUDX, 8, T_BG);
            d.setTextColor(CYAN, T_BG);
            d.setCursor(HUDX, 18); d.printf("%.2f", mhz);
            d.setTextColor(T_DIM, T_BG);
            d.setCursor(HUDX + 1, 27); d.print("MHz");

            d.fillRect(HUDX, 42, SCR_W - HUDX, 8, T_BG);
            d.setTextColor(rssi > -60 ? T_GOOD : (rssi > -85 ? T_WARN : T_DIM), T_BG);
            d.setCursor(HUDX, 42); d.printf("%d", rssi);
            d.setTextColor(T_DIM, T_BG);
            d.setCursor(HUDX + 1, 51); d.print("dBm");

            d.fillRect(HUDX, 66, SCR_W - HUDX, 8, T_BG);
            d.setTextColor(ROSE, T_BG);
            d.setCursor(HUDX, 66); d.printf("%03d", bearing);
            d.setTextColor(T_DIM, T_BG);
            d.setCursor(HUDX + 1, 75); d.print("brg");

            int contacts = 0;
            for (int s = 0; s < SECTORS; ++s)
                if (blip_r[s] > 0 && blip_age[s] < 60) contacts++;
            d.fillRect(HUDX, 92, SCR_W - HUDX, 8, T_BG);
            d.setTextColor(contacts ? T_WARN : T_DIM, T_BG);
            d.setCursor(HUDX, 92); d.printf("trk:%d", contacts);
        }

        uint16_t k = input_poll();
        if (k == PK_ESC) return;
    }
}

/* ---- Persistence / Digital Phosphor ---- */

static void run_persistence(const freq_range_t &range)
{
    auto &d = M5Cardputer.Display;

    const int GX = 22, GY = 13;
    const int GW = SCR_W - GX;
    const int PLOT_H = SCR_H - GY - 8;

    static const struct { int rows, cell_h; } OPTS[] = {
        {57, 2}, {38, 3}, {28, 4},
    };
    int rows = 0, cell_h = 1;
    uint8_t *grid = nullptr;
    for (unsigned o = 0; o < sizeof(OPTS) / sizeof(OPTS[0]); ++o) {
        grid = (uint8_t *)heap_caps_malloc(
            (size_t)OPTS[o].rows * GW * sizeof(uint8_t), MALLOC_CAP_INTERNAL);
        if (grid) { rows = OPTS[o].rows; cell_h = OPTS[o].cell_h; break; }
    }
    if (!grid) { ui_toast("OOM", T_BAD, 1000); return; }
    memset(grid, 0, (size_t)rows * GW * sizeof(uint8_t));

    cc1101_set_rx();
    float step = (range.end - range.start) / GW;

    d.fillScreen(T_BG);
    d.setTextColor(T_ACCENT2, T_BG);
    d.setCursor(4, 3); d.printf("PERSIST %s  %.0f-%.0f", range.name, range.start, range.end);
    d.setTextColor(T_BAD, T_BG);
    d.setCursor(SCR_W - 22, 3); d.print("ESC");

    d.setTextColor(T_DIM, T_BG);
    d.setCursor(0, GY);                   d.print("-30");
    d.setCursor(0, GY + PLOT_H / 2 - 3);  d.print("dBm");
    d.setCursor(0, GY + PLOT_H - 6);      d.print("-110");

    d.setTextColor(T_DIM, T_BG);
    d.setCursor(GX, GY + PLOT_H + 1);                 d.printf("%.0f", range.start);
    d.setCursor(SCR_W - 24, GY + PLOT_H + 1);         d.printf("%.0f", range.end);

    const int PMAX = 48;
    uint16_t pal[PMAX + 1];
    for (int v = 0; v <= PMAX; ++v) {
        int p = (v * 255) / PMAX;
        uint16_t c;
        if (p == 0)        c = T_BG;
        else if (p < 70)   c = d.color565(34, 60 + p, (90 + p * 2 > 211) ? 211 : 90 + p * 2);
        else if (p < 150)  c = d.color565(34 + (p - 70) * 2, (211 - (p - 70) * 2 < 0) ? 0 : 211 - (p - 70) * 2, 200 - (p - 70));
        else               c = d.color565(238, 34 + (p - 150) * 2, 144 + (p - 150));
        pal[v] = c;
    }

    const int DECAY_EVERY = 2;
    uint32_t frame = 0;

    while (true) {
        for (int x = 0; x < GW; ++x) {
            cc1101_set_freq(range.start + x * step); cc1101_set_rx();
            delayMicroseconds(500);
            int rssi = cc1101_get_rssi();

            int n = rssi + 110; if (n < 0) n = 0; if (n > 80) n = 80;
            int ry = (rows - 1) - (n * (rows - 1)) / 80;

            uint8_t *col = &grid[x];
            uint8_t *c0 = &col[ry * GW];
            if (*c0 < PMAX - 2) *c0 += 3; else *c0 = PMAX;
            if (ry > 0)        { uint8_t *u = &col[(ry - 1) * GW]; if (*u < PMAX - 1) (*u)++; }
            if (ry < rows - 1) { uint8_t *l = &col[(ry + 1) * GW]; if (*l < PMAX - 1) (*l)++; }
        }

        if ((frame % DECAY_EVERY) == 0) {
            size_t total = (size_t)rows * GW;
            for (size_t i = 0; i < total; ++i)
                if (grid[i]) grid[i]--;
        }

        for (int gy = 0; gy < rows; ++gy) {
            uint8_t *grow = &grid[gy * GW];
            int py = GY + gy * cell_h;
            static uint16_t line[SCR_W];
            for (int x = 0; x < GW; ++x) {
                int v = grow[x]; if (v > PMAX) v = PMAX;
                line[x] = pal[v];
            }
            for (int s = 0; s < cell_h; ++s)
                d.pushImage(GX, py + s, GW, 1, line);
        }

        frame++;

        uint16_t k = input_poll();
        if (k == PK_ESC) { free(grid); return; }
    }
}

/* ---- Blip Sonar (active-contact detector) ---- */

static void run_blip_sonar(const freq_range_t &range)
{
    auto &d = M5Cardputer.Display;
    cc1101_set_rx();

    const int PLOT_Y = 14;
    const int PLOT_H = SCR_H - PLOT_Y;
    const int BINS   = SCR_W;
    const float step = (range.end - range.start) / BINS;

    struct contact_t {
        int16_t  x;
        int16_t  y;
        uint8_t  active;
        uint8_t  age;
        uint8_t  life;
        uint8_t  rose;
        int8_t   rssi;
        int8_t   prev_r;
    };
    const int MAX_CONTACTS = 40;
    static contact_t cts[MAX_CONTACTS];
    for (int i = 0; i < MAX_CONTACTS; ++i) cts[i].active = 0;

    static float floor_ema[SCR_W];
    for (int i = 0; i < BINS; ++i) floor_ema[i] = -110.0f;
    bool warm = false;
    int  warm_sweeps = 0;

    const uint16_t ROSE = d.color565(238, 34, 144);
    const uint16_t CYAN = d.color565(34, 211, 238);
    int spawn_parity = 0;

    d.fillScreen(T_BG);
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, 3); d.printf("BLIP SONAR %s", range.name);
    d.setTextColor(T_BAD, T_BG);
    d.setCursor(SCR_W - 22, 3); d.print("ESC");

    uint32_t rng = 0x1234abcdu ^ (uint32_t)millis();
    auto nextr = [&rng]() -> uint32_t {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng;
    };

    while (true) {
        for (int wy = PLOT_Y; wy < SCR_H; wy += 2) {
            d.fillRect(0, wy, SCR_W, 1, T_BG);
        }

        int   strongest_i = 0;
        int   strongest_v = -127;
        for (int i = 0; i < BINS; ++i) {
            cc1101_set_freq(range.start + i * step); cc1101_set_rx();
            delayMicroseconds(500);
            int rssi = cc1101_get_rssi();

            if (rssi > strongest_v) { strongest_v = rssi; strongest_i = i; }

            float f = floor_ema[i];
            if (rssi < f) f += (rssi - f) * 0.30f;
            else          f += (rssi - f) * 0.01f;
            floor_ema[i] = f;

            if (!warm) continue;

            int margin = (int)(rssi - f);
            if (margin >= 8 && rssi > -100) {
                int slot = -1, oldest = -1, oldest_age = -1;
                for (int c = 0; c < MAX_CONTACTS; ++c) {
                    if (!cts[c].active) { slot = c; break; }
                    int rem = cts[c].life - cts[c].age;
                    if (rem > oldest_age) { oldest_age = rem; oldest = c; }
                }
                if (slot < 0) slot = oldest;

                int dup = -1;
                for (int c = 0; c < MAX_CONTACTS; ++c) {
                    if (cts[c].active && abs((int)cts[c].x - i) <= 3) { dup = c; break; }
                }
                contact_t &nc = (dup >= 0) ? cts[dup] : cts[slot];

                if (dup < 0 || nc.age > 4) {
                    nc.x      = (int16_t)i;
                    int strength = margin; if (strength > 60) strength = 60;
                    int yc = SCR_H - 8 - (strength * (PLOT_H - 16)) / 60;
                    yc += (int)(nextr() % 9) - 4;
                    if (yc < PLOT_Y + 6)  yc = PLOT_Y + 6;
                    if (yc > SCR_H - 6)   yc = SCR_H - 6;
                    nc.y      = (int16_t)yc;
                    nc.active = 1;
                    nc.age    = 0;
                    nc.life   = 26 + (uint8_t)(nextr() % 10);
                    nc.rose   = (uint8_t)((spawn_parity++) & 1);
                    nc.rssi   = (int8_t)rssi;
                    nc.prev_r = -1;
                }
            }
        }

        if (!warm) { if (++warm_sweeps >= 2) warm = true; }

        int n_active = 0;
        for (int c = 0; c < MAX_CONTACTS; ++c) {
            contact_t &k = cts[c];
            if (!k.active) continue;

            if (k.prev_r >= 0) {
                d.drawCircle(k.x, k.y, k.prev_r, T_BG);
                if (k.prev_r > 0) d.drawCircle(k.x, k.y, k.prev_r - 1, T_BG);
            }

            k.age++;
            if (k.age >= k.life) { k.active = 0; d.fillCircle(k.x, k.y, 2, T_BG); continue; }
            n_active++;

            float t   = (float)k.age / (float)k.life;
            float env = (t < 0.33f) ? (t / 0.33f) : (1.0f - (t - 0.33f) / 0.67f);
            if (env < 0) env = 0; if (env > 1) env = 1;
            int radius = 2 + (int)(env * 7.0f);

            uint16_t base = k.rose ? ROSE : CYAN;
            float fade = 1.0f - t;
            int br = ((base >> 11) & 0x1F);
            int bg = ((base >> 5)  & 0x3F);
            int bb = (base & 0x1F);
            uint16_t glow = d.color565(
                (int)(((br * 255) / 31) * fade),
                (int)(((bg * 255) / 63) * fade),
                (int)(((bb * 255) / 31) * fade));
            uint16_t core = rssi_color(k.rssi);

            d.drawCircle(k.x, k.y, radius, glow);
            if (radius > 2) d.drawCircle(k.x, k.y, radius - 2,
                                          d.color565(
                                              (int)(((br * 255) / 31) * fade * 0.5f),
                                              (int)(((bg * 255) / 63) * fade * 0.5f),
                                              (int)(((bb * 255) / 31) * fade * 0.5f)));
            d.fillCircle(k.x, k.y, 1, core);
            k.prev_r = (int8_t)radius;
        }

        float pk_f = range.start + strongest_i * step;
        d.fillRect(0, 3, SCR_W - 26, 9, T_BG);
        d.setTextColor(T_ACCENT, T_BG);
        d.setCursor(4, 3); d.printf("SONAR %s", range.name);
        d.fillRect(0, SCR_H - 9, SCR_W, 9, T_BG);
        d.setTextColor(n_active ? CYAN : T_DIM, T_BG);
        d.setCursor(4, SCR_H - 9); d.printf("CONTACTS:%d", n_active);
        d.setTextColor(strongest_v > -90 ? ROSE : T_DIM, T_BG);
        d.setCursor(SCR_W - 118, SCR_H - 9);
        d.printf("pk %.2f %ddBm", pk_f, strongest_v);

        uint16_t key = input_poll();
        if (key == PK_ESC) return;
    }
}

/* ---- Mode/range picker ---- */

void feat_subghz_spectrum(void)
{
    leds_set_mode(LED_MODE_SCAN);   /* ring reflects what this screen is doing */
    radio_switch(RADIO_SUBGHZ);
    if (!cc1101_begin(433.0f)) {
        ui_toast("CC1101 not found", T_BAD, 1500);
        radio_switch(RADIO_NONE);
        return;
    }

    auto &d = M5Cardputer.Display;
    int mode = 0, range = 0;   /* default to the 433 MHz band */
    const char *modes[] = {
        "Bar Spectrum", "Waterfall", "Oscilloscope",
        "Peak Hold", "Radar Scope", "Persistence", "Blip Sonar"
    };
    const char *descs[] = {
        "Gradient bars + peak hold + dBm grid",
        "Full-screen scrolling spectrogram",
        "Live RSSI + GDO0 oscilloscope",
        "Falling-peak analyzer + dBm readout",
        "Polar PPI radar w/ phosphor sweep",
        "Digital-phosphor persistence heatmap",
        "Active-contact sonar pings",
    };
    const int MODE_N = 7;

    /* Static chrome once; the compact mode list + band/desc lines repaint
     * only on cursor/band change, so a keypress never blanks the body.
     * need_chrome forces a full re-lay after returning from a sub-mode. */
    bool need_chrome = true;
    int  last_mode = -1, last_range = -1;

    while (true) {
        if (need_chrome) {
            ui_clear_body();
            d.setTextColor(T_ACCENT2, T_BG);
            d.setCursor(4, BODY_Y + 2); d.print("SIGNAL ANALYZER");
            d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT2);
            ui_draw_footer(";/.=mode  TAB=band  ENTER=go  ESC=quit");
            last_mode = last_range = -1;
            need_chrome = false;
        }

        if (mode != last_mode) {
            for (int i = 0; i < MODE_N; ++i) {
                int y = BODY_Y + 16 + i * 11;
                bool s = (i == mode);
                if (s) d.fillRoundRect(4, y - 1, SCR_W - 8, 11, 2, 0x18C3);
                else   d.fillRect(4, y - 1, SCR_W - 8, 11, T_BG);
                d.setTextColor(s ? T_ACCENT : T_FG, s ? 0x18C3 : T_BG);
                d.setCursor(10, y); d.printf("%s", modes[i]);
            }
            ui_text_w(4, BODY_Y + 16 + MODE_N * 11 + 1, SCR_W - 8, 0x07FF,
                      "%s", descs[mode]);
            last_mode = mode;
            last_range = -1;
        }

        if (range != last_range) {
            ui_text_w(4, BODY_Y + 16 + MODE_N * 11 + 11, SCR_W - 8, T_DIM,
                      "band: %s MHz  (TAB)", RANGES[range].name);
            last_range = range;
        }

        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) break;
        if (k == ';' || k == PK_UP)   mode = (mode + MODE_N - 1) % MODE_N;
        if (k == '.' || k == PK_DOWN) mode = (mode + 1) % MODE_N;
        /* PK_ACTIONS (encoder hold) mirrors TAB. The T-Embed has no TAB key,
         * so band select was unreachable there and the analyzer stayed locked
         * to the first range for the whole session. */
        if (k == '\t' || k == PK_ACTIONS) range = (range + 1) % RANGE_COUNT;
        if (k == PK_ENTER) {
            switch (mode) {
                case 0: run_bar_spectrum(RANGES[range]); break;
                case 1: run_waterfall(RANGES[range]); break;
                case 2: run_waveform(RANGES[range].start + (RANGES[range].end - RANGES[range].start) / 2); break;
                case 3: run_peak_hold(RANGES[range]); break;
                case 4: run_radar(RANGES[range]); break;
                case 5: run_persistence(RANGES[range]); break;
                case 6: run_blip_sonar(RANGES[range]); break;
            }
            need_chrome = true;
        }
    }

    cc1101_end();
    radio_switch(RADIO_NONE);
}
