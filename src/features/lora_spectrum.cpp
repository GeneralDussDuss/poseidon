/*
 * lora_spectrum.cpp — LoRa band activity monitor.
 *
 * SX1262 gives you wideband RSSI at the currently tuned frequency, not
 * a per-bin spectrum. Trying to sweep by retuning every pixel column
 * took ~210 × (standby + setFrequency + startReceive + RSSI) per frame
 * and hung on BUSY timeouts — freezing the feature on a blank screen.
 *
 * Redesigned:
 *   - Tune once at band center, stay in RX
 *   - Sample ambient RSSI continuously, plot as a time-series
 *   - Listen for actual LoRa packets (sync = preset sync) and surface
 *     them as a detected-packet overlay with RSSI / SNR / size / hex
 *   - Three view modes share the same data pipeline:
 *       bars     — live RSSI bars with peak hold, time on X
 *       waterfall — scrolling RSSI heatmap, time on X
 *       scope    — RSSI waveform at a single frequency, adjustable
 */
#include "../app.h"
#include "../ui.h"
#include "../input.h"
#include "../radio.h"
#include "../lora_hw.h"
#include "../theme.h"
#include <RadioLib.h>

struct lora_range_t { float start; float end; float center; const char *name; };
static const lora_range_t RANGES[] = {
    { 430.0f, 440.0f, 433.92f, "433MHz"   },
    { 860.0f, 870.0f, 868.10f, "868MHz"   },
    { 900.0f, 930.0f, 915.00f, "915MHz"   },
};
#define RANGE_COUNT 3

/* Last N captured packets shown as an overlay. */
#define PKT_HIST 6
struct pkt_t { float rssi; float snr; uint8_t len; uint32_t when; uint8_t preview[12]; };
static pkt_t s_pkts[PKT_HIST];
static int   s_pkt_head = 0;
static int   s_pkt_count = 0;

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

/* Non-blocking RSSI read. Radio must already be in RX. */
static int read_rssi(SX1262 &radio)
{
    return (int)radio.getRSSI();
}

/* Check for and consume one received LoRa packet. Returns true if one was
 * captured and pushed into the history ring. Non-blocking. */
static bool poll_packet(SX1262 &radio)
{
    size_t n = radio.getPacketLength();
    if (n == 0) return false;

    uint8_t buf[256];
    if (n > sizeof(buf)) n = sizeof(buf);
    int st = radio.readData(buf, n);
    if (st != RADIOLIB_ERR_NONE) {
        radio.startReceive();
        return false;
    }

    pkt_t &p = s_pkts[s_pkt_head];
    p.rssi = radio.getRSSI();
    p.snr  = radio.getSNR();
    p.len  = (uint8_t)n;
    p.when = millis();
    size_t cp = n < sizeof(p.preview) ? n : sizeof(p.preview);
    memcpy(p.preview, buf, cp);
    if (cp < sizeof(p.preview)) memset(p.preview + cp, 0, sizeof(p.preview) - cp);
    s_pkt_head = (s_pkt_head + 1) % PKT_HIST;
    if (s_pkt_count < PKT_HIST) s_pkt_count++;

    radio.startReceive();
    return true;
}

static void draw_packet_overlay(int x, int y)
{
    auto &d = M5Cardputer.Display;
    if (s_pkt_count == 0) {
        d.setTextColor(T_DIM, T_BG);
        d.setCursor(x, y); d.print("no packets yet");
        return;
    }
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(x, y); d.printf("RX %d", s_pkt_count);

    for (int i = 0; i < s_pkt_count && i < 3; ++i) {
        int idx = (s_pkt_head - 1 - i + PKT_HIST) % PKT_HIST;
        const pkt_t &p = s_pkts[idx];
        int ly = y + 10 + i * 9;
        uint32_t age = (millis() - p.when) / 1000;
        d.setTextColor(p.rssi > -90 ? T_GOOD : T_FG, T_BG);
        d.setCursor(x, ly);
        d.printf("%3.0fdB %+.1fSNR %db %lds",
                 p.rssi, p.snr, p.len, (unsigned long)age);
    }
}

/* ---- Bar mode: live RSSI bars, time on X (most recent right), packet overlay ---- */

static void run_bars(SX1262 &radio, const lora_range_t &range)
{
    auto &d = M5Cardputer.Display;
    const int GX = 24, GY = BODY_Y + 14, GW = 140, GH = BODY_H - 34;
    int8_t hist[140]; memset(hist, -130, sizeof(hist));
    int8_t peak[140]; memset(peak, -130, sizeof(peak));
    int hp = 0;

    ui_force_clear_body();

    while (true) {
        int r = read_rssi(radio);
        hist[hp] = (int8_t)r;
        if (r > peak[hp]) peak[hp] = r;
        for (int i = 0; i < GW; i++) if (peak[i] > hist[i] + 1) peak[i]--;
        hp = (hp + 1) % GW;

        (void)poll_packet(radio);

        d.fillRect(GX, GY, GW, GH, 0x0000);

        for (int db = -120; db <= -60; db += 20) {
            int y = GY + GH - ((db + 130) * GH) / 80;
            if (y >= GY && y <= GY + GH) {
                for (int x = GX; x < GX + GW; x += 4) d.drawPixel(x, y, 0x2104);
                d.setTextColor(T_DIM, T_BG);
                d.setCursor(1, y - 3); d.printf("%d", db);
            }
        }

        for (int i = 0; i < GW; i++) {
            int src = (hp + i) % GW;
            int norm = hist[src] + 130;
            if (norm < 0) norm = 0;
            int h = (norm * GH) / 80;
            if (h > 0) {
                for (int dy = 0; dy < h && dy < GH; dy++) {
                    int fake = -130 + ((h - dy) * 80) / GH;
                    d.drawPixel(GX + i, GY + GH - 1 - dy, rssi_color(fake));
                }
            }
            int pn = peak[src] + 130;
            if (pn > 0) {
                int py = GY + GH - (pn * GH) / 80;
                if (py >= GY) d.drawPixel(GX + i, py, T_FG);
            }
        }

        d.drawRect(GX - 1, GY - 1, GW + 2, GH + 2, T_DIM);
        ui_text(4, BODY_Y + 2, T_ACCENT, "LoRa %.3fMHz", range.center);
        d.setTextColor(T_DIM, T_BG);
        d.setCursor(GX, GY + GH + 3); d.print("-time");
        d.setCursor(GX + GW - 14, GY + GH + 3); d.print("now");

        draw_packet_overlay(GX + GW + 6, GY);

        d.setTextColor(T_FG, T_BG);
        d.setCursor(SCR_W - 54, BODY_Y + 2); d.printf("%ddBm", hist[(hp + GW - 1) % GW]);
        ui_draw_footer("`=back  R=reset peaks");

        /* Tight poll so a quick tap on backtick/ESC always catches. */
        uint32_t t0 = millis();
        while (millis() - t0 < 30) {
            uint16_t k = input_poll();
            if (k == PK_ESC) return;
            if (k == 'r' || k == 'R') { memset(peak, -130, sizeof(peak)); break; }
            delay(4);
        }
    }
}

/* ---- Waterfall: scrolling RSSI heatmap, time on X ---- */

#define WF_ROWS 60
#define WF_COLS 200

static void run_waterfall(SX1262 &radio, const lora_range_t &range)
{
    auto &d = M5Cardputer.Display;
    const int GX = 4, GY = BODY_Y + 14, GW = WF_COLS, GH = WF_ROWS;

    uint16_t *ring = (uint16_t *)malloc(GH * GW * sizeof(uint16_t));
    if (!ring) { ui_toast("OOM", T_BAD, 1000); return; }
    memset(ring, 0, GH * GW * sizeof(uint16_t));
    int head = 0, count = 0;

    ui_force_clear_body();
    d.drawRect(GX - 1, GY - 1, GW + 2, GH + 2, T_DIM);

    while (true) {
        int r = read_rssi(radio);
        (void)poll_packet(radio);

        /* Shift column: push newest on the right. Store one column per
         * sample, read back as row-major via transpose lookup. */
        uint16_t col[WF_ROWS];
        col[0] = rssi_color(r);
        /* Vary color slightly over rows so the waterfall looks textured. */
        for (int rr = 1; rr < GH; rr++) col[rr] = rssi_color(r - rr / 3);

        int ci = head;
        for (int rr = 0; rr < GH; rr++) ring[rr * GW + ci] = col[rr];
        head = (head + 1) % GW;
        if (count < GW) count++;

        for (int rr = 0; rr < GH; rr++) {
            d.pushImage(GX, GY + rr, GW, 1, &ring[rr * GW]);
        }

        ui_text(4, BODY_Y + 2, T_ACCENT2, "LoRa WATERFALL %.3f", range.center);
        d.setTextColor(T_FG, T_BG);
        d.setCursor(SCR_W - 54, BODY_Y + 2); d.printf("%ddBm", r);
        ui_draw_footer("`=back");

        draw_packet_overlay(GX + 2, GY + GH + 5);

        uint32_t t0 = millis();
        while (millis() - t0 < 50) {
            uint16_t k = input_poll();
            if (k == PK_ESC) { free(ring); return; }
            delay(4);
        }
    }
}

/* ---- Scope mode: RSSI waveform at a single frequency (adjustable) ---- */

static void run_scope(SX1262 &radio, float freq)
{
    auto &d = M5Cardputer.Display;
    const int GX = 24, GY = BODY_Y + 18, GW = SCR_W - 30, GH = BODY_H - 46;

    /* Retune only once per frequency change. */
    float cur_freq = -1;
    int8_t hist[232]; memset(hist, -130, sizeof(hist));
    int hp = 0;

    ui_force_clear_body();

    while (true) {
        if (freq != cur_freq) {
            radio.standby();
            if (radio.setFrequency(freq) == RADIOLIB_ERR_NONE) {
                radio.startReceive();
                cur_freq = freq;
            }
        }

        int r = read_rssi(radio);
        hist[hp] = (int8_t)r;
        hp = (hp + 1) % GW;

        (void)poll_packet(radio);

        d.fillRect(GX, GY, GW, GH, 0x0000);
        d.drawRect(GX - 1, GY - 1, GW + 2, GH + 2, T_DIM);

        for (int db = -120; db <= -60; db += 20) {
            int y = GY + GH - ((db + 130) * GH) / 80;
            if (y >= GY && y <= GY + GH) {
                for (int x = GX; x < GX + GW; x += 6) d.drawPixel(x, y, 0x2104);
                d.setTextColor(T_DIM, T_BG);
                d.setCursor(1, y - 3); d.printf("%d", db);
            }
        }

        for (int i = 1; i < GW; i++) {
            int s1 = (hp + i - 1) % GW, s2 = (hp + i) % GW;
            int n1 = hist[s1] + 130, n2 = hist[s2] + 130;
            if (n1 < 0) n1 = 0; if (n2 < 0) n2 = 0;
            int y1 = GY + GH - (n1 * GH) / 80;
            int y2 = GY + GH - (n2 * GH) / 80;
            d.drawLine(GX + i - 1, y1, GX + i, y2, rssi_color(hist[s2]));
        }

        ui_text(4, BODY_Y + 2, T_ACCENT, "LoRa SCOPE %.3fMHz", freq);
        int cur = hist[(hp + GW - 1) % GW];
        d.setTextColor(cur > -80 ? T_GOOD : T_DIM, T_BG);
        d.setCursor(SCR_W - 54, BODY_Y + 2); d.printf("%ddBm", cur);

        draw_packet_overlay(GX, GY + GH + 5);

        ui_draw_footer("+-=freq  ESC=back");

        uint32_t t = millis();
        while (millis() - t < 40) {
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

    /* Arm RX so poll_packet / read_rssi have live data. */
    lora_radio().startReceive();

    auto &d = M5Cardputer.Display;
    int mode = 0, range = 2;
    const char *modes[] = { "Bar Meter", "Waterfall", "Oscilloscope" };
    const char *descs[] = {
        "RSSI bars over time + packet capture",
        "Scrolling RSSI heatmap + packet capture",
        "Live waveform at one freq + capture"
    };

    /* Clear history on entry so each session starts fresh. */
    s_pkt_count = 0; s_pkt_head = 0;

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
        d.printf("band: %s (TAB)", RANGES[range].name);
        ui_draw_footer(";/.=mode  TAB=band  ENTER=go  ESC=quit");

        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) break;
        if (k == ';' || k == PK_UP)   mode = (mode + 2) % 3;
        if (k == '.' || k == PK_DOWN) mode = (mode + 1) % 3;
        if (k == '\t') {
            range = (range + 1) % RANGE_COUNT;
            /* Retune to the new center and stay in RX. */
            auto &radio = lora_radio();
            radio.standby();
            radio.setFrequency(RANGES[range].center);
            radio.startReceive();
        }
        if (k == PK_ENTER) {
            auto &radio = lora_radio();
            /* Ensure tuned to band center for bars / waterfall. */
            radio.standby();
            radio.setFrequency(RANGES[range].center);
            radio.startReceive();
            if (mode == 0)       run_bars(radio, RANGES[range]);
            else if (mode == 1)  run_waterfall(radio, RANGES[range]);
            else                 run_scope(radio, RANGES[range].center);
        }
    }

    lora_end();
    radio_switch(RADIO_NONE);
}
