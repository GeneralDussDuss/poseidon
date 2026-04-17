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

#define CH_N 14  /* channels 1..13 valid + extra slot to keep the math simple */

static volatile int8_t  s_peak[CH_N + 1];
static volatile uint32_t s_pkts[CH_N + 1];
static volatile uint8_t s_current_ch = 1;
static volatile bool    s_running = false;

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
    while (s_running) {
        s_current_ch = (s_current_ch % 13) + 1;
        esp_wifi_set_channel(s_current_ch, WIFI_SECOND_CHAN_NONE);
        delay(80);
    }
    vTaskDelete(nullptr);
}

static void decay_task(void *)
{
    /* Slowly decay peaks so the display responds to changes instead
     * of stuck at the all-time max. Subtract 1 dB every 500ms. */
    while (s_running) {
        delay(500);
        for (int c = 1; c <= 13; ++c) {
            int8_t v = s_peak[c];
            if (v > -100) s_peak[c] = v - 1;
        }
    }
    vTaskDelete(nullptr);
}

static void draw_spectrum(void)
{
    auto &d = M5Cardputer.Display;
    ui_clear_body();
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2);
    d.printf("SPECTRUM  ch%d", s_current_ch);
    d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT);

    /* 13 channels spread across the body. Each bar ~16 px wide. */
    const int bar_w  = 16;
    const int gap    = 2;
    const int top    = BODY_Y + 18;
    const int bottom = FOOTER_Y - 10;
    const int height = bottom - top;
    const int start_x = (SCR_W - (13 * (bar_w + gap))) / 2;

    for (int c = 1; c <= 13; ++c) {
        int x = start_x + (c - 1) * (bar_w + gap);
        int8_t rssi = s_peak[c];

        /* -100 dBm → 0 px, -30 dBm → full height. */
        int bh = (rssi + 100) * height / 70;
        if (bh < 0) bh = 0;
        if (bh > height) bh = height;

        uint16_t color;
        if      (rssi > -50) color = T_BAD;    /* red  */
        else if (rssi > -70) color = T_WARN;   /* amber */
        else if (rssi > -85) color = T_GOOD;   /* green */
        else                 color = 0x2124;     /* dim cyan */

        /* Bar baseline. */
        d.drawFastVLine(x + bar_w / 2 - 1, top, height, 0x0841);
        /* Filled bar. */
        d.fillRect(x, bottom - bh, bar_w, bh, color);

        /* Pulse ring on the current channel being sampled. */
        if (c == s_current_ch) {
            d.drawRect(x - 1, top - 1, bar_w + 2, height + 2, 0xFFFF);
        }

        /* Channel number label. */
        d.setTextColor(c == s_current_ch ? T_ACCENT : T_DIM, T_BG);
        d.setCursor(x + (c < 10 ? bar_w / 2 - 3 : bar_w / 2 - 6), bottom + 1);
        d.printf("%d", c);
    }

    ui_draw_status(radio_name(), "spec");
}

void feat_wifi_spectrum(void)
{
    radio_switch(RADIO_WIFI);
    WiFi.mode(WIFI_STA);

    for (int c = 0; c <= CH_N; ++c) { s_peak[c] = -100; s_pkts[c] = 0; }
    s_current_ch = 1;
    s_running = true;

    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(spec_cb);
    esp_wifi_set_channel(s_current_ch, WIFI_SECOND_CHAN_NONE);

    xTaskCreate(hop_task,   "spec_hop",   3072, nullptr, 4, nullptr);
    xTaskCreate(decay_task, "spec_decay", 2048, nullptr, 3, nullptr);

    ui_draw_footer("R=reset peaks  `=back");
    uint32_t last = 0;
    while (true) {
        if (millis() - last > 80) {
            last = millis();
            draw_spectrum();
        }
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) break;
        if (k == 'r' || k == 'R') {
            for (int c = 0; c <= CH_N; ++c) { s_peak[c] = -100; s_pkts[c] = 0; }
        }
    }

    s_running = false;
    esp_wifi_set_promiscuous(false);
    delay(200);
}
