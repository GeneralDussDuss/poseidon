/*
 * lora_spectrum.cpp — LoRa band spectrum analyzer.
 *
 * Uses the SX1262's RSSI reading to sweep LoRa bands with the same
 * professional visualizations as the CC1101 analyzer: bar spectrum,
 * waterfall spectrogram, and oscilloscope waveform.
 */
#include "../app.h"
#include "../ui.h"
#include "../input.h"
#include "../radio.h"
#include "../lora_hw.h"
#include "../theme.h"
#include <RadioLib.h>

struct lora_range_t { float start; float end; const char *name; };
static const lora_range_t RANGES[] = {
    { 430.0f, 440.0f, "430-440" },
    { 860.0f, 870.0f, "860-870" },
    { 900.0f, 930.0f, "900-930" },
};
#define RANGE_COUNT 3

static uint16_t rssi_color(int rssi)
{
    auto &d = M5Cardputer.Display;
    int n = rssi + 130;
    if (n < 0) n = 0; if (n > 80) n = 80;
    int p = (n * 255) / 80;
    if (p < 50)  return d.color565(0, 0, 40 + p * 2);
    if (p < 100) return d.color565(0, (p - 50) * 5, 140 - (p - 50) * 2);
    if (p < 170) return d.color565((p - 100) * 3, 255, 0);
    return d.color565(255, 255 - (p - 170) * 3, 0);
}

/* Read RSSI via the CC1101-style approach: reconfigure freq through
 * the low-level Module SPI, avoiding RadioLib's BUSY-polling methods
 * which timeout with RADIOLIB_NC on some lib versions. */
static int lora_read_rssi_fast(SX1262 &radio, float freq)
{
    /* For sweep: use getRSSI in continuous RX mode. The SX1262 reports
     * wideband RSSI regardless of tuned frequency when in RX. For a
     * true per-frequency sweep we'd need to retune, but that triggers
     * BUSY waits. Instead, use the single-frequency RSSI for scope mode
     * and a simulated sweep based on current ambient for bar/waterfall. */
    (void)freq;
    return (int)radio.getRSSI();
}

static int lora_read_rssi(SX1262 &radio, float freq)
{
    /* Sweep loop: the radio may already be in RX mode from the previous
     * iteration. SX1262 requires standby before retuning or BUSY never
     * deasserts and RadioLib eventually errors out mid-sweep. Always
     * drop to standby first. */
    radio.standby();
    int st = radio.setFrequency(freq);
    if (st != RADIOLIB_ERR_NONE) return -130;
    st = radio.startReceive();
    if (st != RADIOLIB_ERR_NONE) return -130;
    delay(2);
    int rssi = (int)radio.getRSSI();
    return rssi;
}

/* ---- Bar Spectrum ---- */

static void run_bars(SX1262 &radio, const lora_range_t &range)
{
    auto &d = M5Cardputer.Display;
    const int GX = 24, GY = BODY_Y + 14, GW = SCR_W - 30, GH = BODY_H - 30;
    int bins = GW;
    float step = (range.end - range.start) / bins;
    int8_t rssi[232] = {0};
    int8_t peak[232] = {0};
    memset(peak, -130, sizeof(peak));

    ui_force_clear_body();

    while (true) {
        for (int i = 0; i < bins; i++) {
            rssi[i] = (int8_t)lora_read_rssi(radio, range.start + i * step);
            if (rssi[i] > peak[i]) peak[i] = rssi[i];
        }
        for (int i = 0; i < bins; i++)
            if (peak[i] > rssi[i] + 1) peak[i]--;

        d.fillRect(GX, GY, GW, GH, 0x0000);

        for (int db = -120; db <= -60; db += 20) {
            int y = GY + GH - ((db + 130) * GH) / 80;
            if (y >= GY && y <= GY + GH) {
                for (int x = GX; x < GX + GW; x += 4) d.drawPixel(x, y, 0x2104);
                d.setTextColor(T_DIM, T_BG);
                d.setCursor(1, y - 3); d.printf("%d", db);
            }
        }

        for (int i = 0; i < bins; i++) {
            int norm = rssi[i] + 130;
            if (norm < 0) norm = 0;
            int h = (norm * GH) / 80;
            if (h > 0) {
                for (int dy = 0; dy < h && dy < GH; dy++) {
                    int fake = -130 + ((h - dy) * 80) / GH;
                    d.drawPixel(GX + i, GY + GH - 1 - dy, rssi_color(fake));
                }
            }
            int pn = peak[i] + 130;
            if (pn > 0) {
                int py = GY + GH - (pn * GH) / 80;
                if (py >= GY) d.drawPixel(GX + i, py, T_FG);
            }
        }

        d.drawRect(GX - 1, GY - 1, GW + 2, GH + 2, T_DIM);
        ui_text(4, BODY_Y + 2, T_ACCENT, "LoRa SPECTRUM %s MHz", range.name);
        d.setTextColor(T_DIM, T_BG);
        d.setCursor(GX, GY + GH + 3); d.printf("%.0f", range.start);
        d.setCursor(GX + GW - 24, GY + GH + 3); d.printf("%.0f", range.end);

        int pi = 0, pv = -130;
        for (int i = 0; i < bins; i++) if (rssi[i] > pv) { pv = rssi[i]; pi = i; }
        d.setTextColor(T_FG, T_BG);
        d.setCursor(GX + GW / 2 - 36, GY + GH + 3);
        d.printf("pk:%.1f %ddB", range.start + pi * step, pv);
        ui_draw_footer("ESC=back  R=reset");

        uint16_t k = input_poll();
        if (k == PK_ESC) return;
        if (k == 'r' || k == 'R') memset(peak, -130, sizeof(peak));
    }
}

/* ---- Waterfall ---- */

#define WF_ROWS 40
#define WF_BINS 210

static void run_waterfall(SX1262 &radio, const lora_range_t &range)
{
    auto &d = M5Cardputer.Display;
    const int GX = 24, GY = BODY_Y + 14, GW = WF_BINS, GH = WF_ROWS;
    float step = (range.end - range.start) / GW;

    uint16_t *ring = (uint16_t *)malloc(GH * GW * sizeof(uint16_t));
    if (!ring) { ui_toast("OOM", T_BAD, 1000); return; }
    int head = 0, count = 0;

    ui_force_clear_body();
    d.drawRect(GX - 1, GY - 1, GW + 2, GH + 2, T_DIM);

    while (true) {
        uint16_t *row = &ring[head * GW];
        for (int i = 0; i < GW; i++) {
            row[i] = rssi_color(lora_read_rssi(radio, range.start + i * step));
        }
        head = (head + 1) % GH;
        if (count < GH) count++;

        for (int r = 0; r < count; r++) {
            int ri = (head - count + r + GH) % GH;
            d.pushImage(GX, GY + r, GW, 1, &ring[ri * GW]);
        }

        ui_text(4, BODY_Y + 2, T_ACCENT2, "LoRa WATERFALL %s", range.name);
        d.setTextColor(T_DIM, T_BG);
        d.setCursor(GX, GY + GH + 3); d.printf("%.0f", range.start);
        d.setCursor(GX + GW - 24, GY + GH + 3); d.printf("%.0f", range.end);
        ui_draw_footer("ESC=back");

        uint16_t k = input_poll();
        if (k == PK_ESC) { free(ring); return; }
    }
}

/* ---- Oscilloscope ---- */

static void run_scope(SX1262 &radio, float freq)
{
    auto &d = M5Cardputer.Display;
    const int GX = 24, GY = BODY_Y + 18, GW = SCR_W - 30, GH = BODY_H - 34;
    int mid = GY + GH / 2;

    ui_force_clear_body();

    while (true) {
        int8_t hist[232];
        for (int i = 0; i < GW; i++) {
            hist[i] = (int8_t)lora_read_rssi(radio, freq);
        }

        d.fillRect(GX, GY, GW, GH, 0x0000);
        d.drawRect(GX - 1, GY - 1, GW + 2, GH + 2, T_DIM);

        for (int db = -100; db <= -60; db += 20) {
            int y = mid - ((db + 100) * (GH / 2)) / 40;
            if (y >= GY && y <= GY + GH) {
                for (int x = GX; x < GX + GW; x += 6) d.drawPixel(x, y, 0x2104);
                d.setTextColor(T_DIM, T_BG);
                d.setCursor(1, y - 3); d.printf("%d", db);
            }
        }
        for (int x = GX; x < GX + GW; x += 3) d.drawPixel(x, mid, 0x3186);

        for (int i = 1; i < GW; i++) {
            int n1 = hist[i - 1] + 130, n2 = hist[i] + 130;
            if (n1 < 0) n1 = 0; if (n2 < 0) n2 = 0;
            int y1 = GY + GH - (n1 * GH) / 80;
            int y2 = GY + GH - (n2 * GH) / 80;
            d.drawLine(GX + i - 1, y1, GX + i, y2, rssi_color(hist[i]));
            if (y2 < GY + GH)
                d.drawFastVLine(GX + i, y2, GY + GH - y2, rssi_color(hist[i] - 20));
        }

        ui_text(4, BODY_Y + 2, T_ACCENT, "LoRa SCOPE %.3f MHz", freq);
        int cur = hist[GW / 2];
        d.setTextColor(cur > -80 ? T_GOOD : T_DIM, T_BG);
        d.setCursor(SCR_W - 54, BODY_Y + 2); d.printf("%d dBm", cur);
        ui_draw_footer("+-=freq  ESC=back");

        uint32_t t = millis();
        while (millis() - t < 60) {
            uint16_t k = input_poll();
            if (k == PK_ESC) return;
            if (k == '+' || k == '=') freq += 0.1f;
            if (k == '-') freq -= 0.1f;
        }
    }
}

/* ---- Entry point with mode picker ---- */

void feat_lora_spectrum(void)
{
    radio_switch(RADIO_LORA);
    lora_config_t cfg = lora_preset(LORA_BAND_915);
    int lora_st = lora_begin(cfg);
    if (lora_st != RADIOLIB_ERR_NONE) {
        char msg[32]; snprintf(msg, sizeof(msg), "LoRa err %d", lora_st);
        ui_toast(msg, T_BAD, 2000);
        radio_switch(RADIO_NONE);
        return;
    }

    auto &d = M5Cardputer.Display;
    int mode = 0, range = 2;
    const char *modes[] = { "Bar Spectrum", "Waterfall", "Oscilloscope" };
    const char *descs[] = {
        "Gradient bars + peak hold + dBm grid",
        "Scrolling color heatmap spectrogram",
        "Live RSSI waveform with filled area"
    };

    while (true) {
        ui_force_clear_body();
        d.setTextColor(T_ACCENT, T_BG);
        d.setCursor(4, BODY_Y + 2); d.print("LoRa ANALYZER");
        d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT);

        for (int i = 0; i < 3; i++) {
            int y = BODY_Y + 20 + i * 22;
            bool s = (i == mode);
            if (s) {
                d.fillRoundRect(4, y - 2, SCR_W - 8, 20, 3, T_SEL_BG);
                d.drawRoundRect(4, y - 2, SCR_W - 8, 20, 3, T_ACCENT);
            }
            d.setTextColor(s ? T_ACCENT : T_FG, s ? T_SEL_BG : T_BG);
            d.setCursor(10, y); d.print(modes[i]);
            d.setTextColor(s ? T_FG : T_DIM, s ? T_SEL_BG : T_BG);
            d.setCursor(10, y + 10); d.print(descs[i]);
        }

        d.setTextColor(T_DIM, T_BG);
        d.setCursor(4, BODY_Y + 90);
        d.printf("band: %s MHz  (TAB)", RANGES[range].name);
        ui_draw_footer(";/.=mode  TAB=band  ENTER=go  ESC=quit");

        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) break;
        if (k == ';' || k == PK_UP)   mode = (mode + 2) % 3;
        if (k == '.' || k == PK_DOWN) mode = (mode + 1) % 3;
        if (k == '\t') range = (range + 1) % RANGE_COUNT;
        if (k == PK_ENTER) {
            auto &radio = lora_radio();
            if (mode == 0) run_bars(radio, RANGES[range]);
            else if (mode == 1) run_waterfall(radio, RANGES[range]);
            else run_scope(radio, RANGES[range].start + (RANGES[range].end - RANGES[range].start) / 2);
        }
    }

    lora_end();
    radio_switch(RADIO_NONE);
}
