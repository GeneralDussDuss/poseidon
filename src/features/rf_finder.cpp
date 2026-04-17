/*
 * rf_finder.cpp — hot/cold RF signal locator.
 *
 * Two modes:
 *   nRF24: track a specific channel or discovered device address
 *   CC1101: track a sub-GHz frequency via RSSI
 *
 * Display: large thermometer bar (blue→cyan→green→yellow→red),
 * numeric dBm/strength, direction arrows, and audio beep whose
 * rate increases as signal gets stronger.
 */
#include "../app.h"
#include "../theme.h"
#include "../ui.h"
#include "../input.h"
#include "../radio.h"
#include "../cc1101_hw.h"
#include "../nrf24_hw.h"
#include <ELECHOUSE_CC1101_SRC_DRV.h>

/* Visual-only feedback — speaker removed to avoid blocking input. */

static uint16_t heat_color(int pct)
{
    auto &d = M5Cardputer.Display;
    if (pct < 20)  return d.color565(0, 0, 80 + pct * 4);       /* deep blue */
    if (pct < 40)  return d.color565(0, (pct - 20) * 12, 160);  /* blue→cyan */
    if (pct < 60)  return d.color565(0, 255, 160 - (pct-40)*8); /* cyan→green */
    if (pct < 80)  return d.color565((pct-60)*12, 255, 0);       /* green→yellow */
    return d.color565(255, 255 - (pct-80)*12, 0);                /* yellow→red */
}

static void draw_meter(int pct, int rssi, const char *label)
{
    auto &d = M5Cardputer.Display;
    const int BAR_X = 20, BAR_Y = BODY_Y + 30, BAR_W = SCR_W - 40, BAR_H = 30;

    /* Background frame. */
    d.fillRect(BAR_X - 2, BAR_Y - 2, BAR_W + 4, BAR_H + 4, 0x2104);
    d.fillRect(BAR_X, BAR_Y, BAR_W, BAR_H, T_BG);

    /* Filled bar with gradient. */
    int fill_w = (pct * BAR_W) / 100;
    for (int x = 0; x < fill_w; x++) {
        int local_pct = (x * 100) / BAR_W;
        d.drawFastVLine(BAR_X + x, BAR_Y, BAR_H, heat_color(local_pct));
    }

    /* Tick marks at 25/50/75%. */
    for (int t = 25; t <= 75; t += 25) {
        int tx = BAR_X + (t * BAR_W) / 100;
        d.drawFastVLine(tx, BAR_Y - 2, BAR_H + 4, 0x4208);
    }

    /* Big percentage text. */
    d.setTextSize(2);
    d.setTextColor(pct > 70 ? T_BAD : pct > 40 ? T_WARN : T_ACCENT, T_BG);
    char buf[16];
    snprintf(buf, sizeof(buf), "%d%%", pct);
    int tw = d.textWidth(buf);
    d.setCursor((SCR_W - tw) / 2, BAR_Y + BAR_H + 10);
    d.print(buf);
    d.setTextSize(1);

    /* Signal strength + label. */
    d.setTextColor(T_FG, T_BG);
    d.setCursor(BAR_X, BAR_Y + BAR_H + 30);
    d.printf("%d dBm", rssi);
    d.setCursor(SCR_W - 60, BAR_Y + BAR_H + 30);
    d.printf("%s", pct > 70 ? "HOT!" : pct > 40 ? "WARM" : "COLD");

    /* Heat indicator dots across the bottom. */
    int dot_y = BAR_Y + BAR_H + 42;
    for (int i = 0; i < 20; i++) {
        int dp = (i * 100) / 20;
        uint16_t c = (dp < pct) ? heat_color(dp) : 0x2104;
        d.fillCircle(BAR_X + i * (BAR_W / 20) + 4, dot_y, 3, c);
    }
}

/* ---- Sub-GHz finder (CC1101 RSSI) ---- */

void feat_subghz_finder(void)
{
    radio_switch(RADIO_SUBGHZ);
    if (!cc1101_begin(433.92f)) {
        ui_toast("CC1101 not found", T_BAD, 1500);
        radio_switch(RADIO_NONE);
        return;
    }

    auto &d = M5Cardputer.Display;
    float freq = 433.92f;
    ELECHOUSE_cc1101.SetRx();
    int peak_rssi = -120;
    int floor_rssi = -40;  /* will auto-adjust down */
    int ceil_rssi = -100;   /* will auto-adjust up */
    uint32_t last_draw = 0, last_beep = 0;
    int ema_rssi = cc1101_get_rssi();

    ui_clear_body();

    while (true) {
        int raw = cc1101_get_rssi();
        /* EMA smoothing for stable display. */
        ema_rssi = (ema_rssi * 3 + raw) / 4;
        int rssi = ema_rssi;
        if (rssi > peak_rssi) peak_rssi = rssi;
        /* Auto-calibrate floor and ceiling from observed values. */
        if (rssi < floor_rssi) floor_rssi = rssi;
        if (rssi > ceil_rssi) ceil_rssi = rssi;
        /* Ensure at least 10 dBm range to avoid div-by-zero jitter. */
        int range = ceil_rssi - floor_rssi;
        if (range < 10) range = 10;
        int pct = ((rssi - floor_rssi) * 100) / range;
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;

        /* Beep rate proportional to signal strength. */
        uint32_t now = millis();
        int beep_interval = 1000 - (pct * 9);  /* 1000ms at 0% → 100ms at 100% */
        if (beep_interval < 50) beep_interval = 50;
        (void)last_beep;

        if (now - last_draw > 150) {
            last_draw = now;
            ui_draw_status(radio_name(), "finder");
            ui_text(4, BODY_Y + 2, T_ACCENT2, "RF FINDER  %.3f MHz", freq);
            d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT2);

            draw_meter(pct, rssi, "");

            ui_text(4, BODY_Y + 108, T_DIM, "peak:%d floor:%d ceil:%d", peak_rssi, floor_rssi, ceil_rssi);
            ui_draw_footer("+-=freq  R=reset  ESC=back");
        }

        uint16_t k = input_poll();
        if (k == PK_ESC) break;
        if (k == '+' || k == '=') {
            freq += 0.5f;
            ELECHOUSE_cc1101.setSidle();
            ELECHOUSE_cc1101.setMHZ(freq);
            ELECHOUSE_cc1101.SetRx();
        }
        if (k == '-') {
            freq -= 0.5f;
            ELECHOUSE_cc1101.setSidle();
            ELECHOUSE_cc1101.setMHZ(freq);
            ELECHOUSE_cc1101.SetRx();
        }
        if (k == 'r' || k == 'R') { peak_rssi = -120; floor_rssi = rssi; ceil_rssi = rssi; }
        delay(10);
    }

    cc1101_end();
    radio_switch(RADIO_NONE);
}

/* ---- 2.4 GHz finder (nRF24 RPD polling) ---- */

void feat_nrf24_finder(void)
{
    radio_switch(RADIO_NRF24);
    if (!nrf24_begin()) {
        ui_toast("nRF24 not found", T_BAD, 1500);
        radio_switch(RADIO_NONE);
        return;
    }

    auto &rf = nrf24_radio();
    rf.setAutoAck(false);
    rf.setDataRate(RF24_2MBPS);

    auto &d = M5Cardputer.Display;
    uint8_t ch = 40;
    int strength = 0;
    int peak_str = 0;
    int floor_str = 50, ceil_str = 0;
    uint32_t last_draw = 0, last_beep = 0;

    ui_clear_body();

    while (true) {
        int hits = 0;
        for (int i = 0; i < 100; i++) {
            rf.setChannel(ch);
            rf.startListening();
            delayMicroseconds(130);
            rf.stopListening();
            if (rf.testRPD()) hits++;
        }
        if (hits < floor_str) floor_str = hits;
        if (hits > ceil_str) ceil_str = hits;
        int range = ceil_str - floor_str;
        if (range < 5) range = 5;
        strength = ((hits - floor_str) * 100) / range;
        if (strength < 0) strength = 0;
        if (strength > 100) strength = 100;
        if (strength > peak_str) peak_str = strength;

        uint32_t now = millis();
        int beep_interval = 1000 - (strength * 9);
        if (beep_interval < 50) beep_interval = 50;
        (void)last_beep;

        if (now - last_draw > 150) {
            last_draw = now;
            ui_draw_status(radio_name(), "finder");
            ui_text(4, BODY_Y + 2, T_ACCENT, "2.4G FINDER  ch%u (%uMHz)", ch, 2400 + ch);
            d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT);

            draw_meter(strength, -64 + (100 - strength) * 36 / 100, "");

            ui_text(4, BODY_Y + 108, T_DIM, "peak: %d%%  hits: %d/50", peak_str, hits);
            ui_draw_footer("+-=channel  R=reset  ESC=back");
        }

        uint16_t k = input_poll();
        if (k == PK_ESC) break;
        if (k == '+' || k == '=') { ch = (ch < 125) ? ch + 1 : 125; }
        if (k == '-') { ch = (ch > 0) ? ch - 1 : 0; }
        if (k == 'r' || k == 'R') peak_str = 0;
    }

    nrf24_end();
    radio_switch(RADIO_NONE);
}
