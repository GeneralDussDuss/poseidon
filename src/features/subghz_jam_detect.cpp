/*
 * subghz_jam_detect — RSSI anomaly monitor.
 *
 * Passive baseline-learning peer to the WiFi deauth detector: watches
 * the CC1101 RSSI floor on a chosen frequency, learns the noise
 * baseline during the first 10 seconds, then alerts on sustained
 * spikes above baseline — the signature of a jammer or active TX in
 * the band.
 *
 * Detection logic:
 *   - 10-second warmup: compute running mean + max of the RSSI floor.
 *   - Runtime: RSSI ≥ baseline_mean + 15 dBm for ≥ 5 consecutive samples
 *     (≈500 ms of sustained elevated noise) = JAM. Screen flashes red +
 *     a two-tone siren fires.
 *   - After alert, a 2-second cooldown before re-arming.
 *
 * Use cases:
 *   - Paired with the wardrive/scan flow to detect if a nearby CC1101
 *     jammer is drowning a band you're trying to capture in.
 *   - Honeypot-style: leave running on 433.92 near a target car, log
 *     alerts to SD with timestamp + peak RSSI.
 */
#include "../app.h"
#include "../theme.h"
#include "../ui.h"
#include "../input.h"
#include "../radio.h"
#include "../cc1101_hw.h"
#include "../sd_helper.h"
#include "menu.h"
#include <ELECHOUSE_CC1101_SRC_DRV.h>
#include <SD.h>

static const float JAM_FREQS[] = {
    303.875f, 315.00f, 390.00f, 418.00f, 433.92f, 868.35f, 915.00f
};
static const int JAM_FREQ_COUNT = sizeof(JAM_FREQS) / sizeof(JAM_FREQS[0]);

static int pick_freq(void)
{
    auto &d = M5Cardputer.Display;
    int sel = 4;  /* default 433.92 */
    while (true) {
        ui_clear_body();
        d.setTextColor(T_ACCENT, T_BG);
        d.setCursor(4, BODY_Y + 2); d.print("JAM DETECT");
        d.drawFastHLine(4, BODY_Y + 12, 100, T_ACCENT);
        d.setTextColor(T_DIM, T_BG);
        d.setCursor(4, BODY_Y + 18); d.print("pick freq to monitor");
        for (int i = 0; i < JAM_FREQ_COUNT; ++i) {
            int y = BODY_Y + 32 + i * 10;
            bool s = (i == sel);
            if (s) d.fillRect(2, y - 1, SCR_W - 4, 10, T_SEL_BG);
            d.setTextColor(s ? T_ACCENT : T_FG, s ? T_SEL_BG : T_BG);
            d.setCursor(8, y);
            d.printf("%.3f MHz", JAM_FREQS[i]);
        }
        ui_draw_footer(";/.=pick  ENTER=start  `=back");
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) return -1;
        if (k == ';' || k == PK_UP)   sel = (sel - 1 + JAM_FREQ_COUNT) % JAM_FREQ_COUNT;
        if (k == '.' || k == PK_DOWN) sel = (sel + 1) % JAM_FREQ_COUNT;
        if (k == PK_ENTER) return sel;
    }
}

void feat_subghz_jam_detect(void)
{
    radio_switch(RADIO_SUBGHZ);

    int fi = pick_freq();
    if (fi < 0) return;
    float freq = JAM_FREQS[fi];

    if (!cc1101_begin(freq)) {
        ui_toast("CC1101 init failed", T_BAD, 1500);
        return;
    }
    ELECHOUSE_cc1101.setRxBW(270);   /* wider = catches more noise */
    cc1101_set_rx();

    auto &d = M5Cardputer.Display;
    ui_clear_body();
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2); d.printf("JAM DETECT %.3f", freq);
    d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT);
    ui_draw_footer("`=stop  ?=help");

    /* ----- Phase 1: warmup / baseline learning (10s) ----- */
    uint32_t warmup_end = millis() + 10000;
    int      samples   = 0;
    int32_t  sum       = 0;
    int      peak_warm = -200;
    while (millis() < warmup_end) {
        int r = cc1101_get_rssi();
        sum += r;
        samples++;
        if (r > peak_warm) peak_warm = r;

        if ((samples & 0x0F) == 0) {
            d.fillRect(0, BODY_Y + 20, SCR_W, 60, T_BG);
            d.setTextColor(T_WARN, T_BG);
            d.setCursor(4, BODY_Y + 24);
            d.printf("learning baseline... %lus",
                     (unsigned long)((warmup_end - millis()) / 1000));
            d.setTextColor(T_FG, T_BG);
            d.setCursor(4, BODY_Y + 40);
            d.printf("floor    : %d dBm", samples ? (int)(sum / samples) : 0);
            d.setCursor(4, BODY_Y + 52);
            d.printf("peak warm: %d dBm", peak_warm);
        }
        uint16_t k = input_poll();
        if (k == PK_ESC) return;
        if (k == '?') { ui_show_current_help(); ui_draw_footer("`=stop  ?=help"); }
        delay(20);
    }
    int baseline = samples ? (int)(sum / samples) : -90;
    int trigger  = baseline + 15;

    /* ----- Phase 2: monitor + alert ----- */
    int consecutive = 0;
    uint32_t alerts = 0;
    uint32_t last_alert_ms = 0;
    int      peak_live  = -200;
    int      cur_rssi   = baseline;
    uint32_t last_draw  = 0;

    while (true) {
        cur_rssi = cc1101_get_rssi();
        if (cur_rssi > peak_live) peak_live = cur_rssi;

        bool alerting = false;
        if (cur_rssi >= trigger) consecutive++;
        else consecutive = 0;
        if (consecutive >= 5 && millis() - last_alert_ms > 2000) {
            alerting = true;
            alerts++;
            last_alert_ms = millis();
            consecutive = 0;

            /* Siren + overlay. */
            M5Cardputer.Speaker.tone(2400, 120);
            delay(130);
            M5Cardputer.Speaker.tone(1600, 120);
            char sub[48];
            snprintf(sub, sizeof(sub), "%.3f MHz  %d dBm", freq, cur_rssi);
            ui_action_overlay("JAM DETECTED", sub, ACT_BG_GLITCH, T_BAD, 1200);

            /* Log to SD. */
            if (sd_mount()) {
                SD.mkdir("/poseidon");
                File f = SD.open("/poseidon/jamdetect.csv", FILE_APPEND);
                if (f) {
                    f.printf("%lu,%.3f,%d,%d,%d\n",
                             (unsigned long)(millis() / 1000),
                             freq, cur_rssi, baseline, (int)alerts);
                    f.close();
                }
            }

            /* Redraw header after overlay cleared the body. */
            ui_clear_body();
            d.setTextColor(T_ACCENT, T_BG);
            d.setCursor(4, BODY_Y + 2); d.printf("JAM DETECT %.3f", freq);
            d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT);
            last_draw = 0;
        }

        uint32_t now = millis();
        if (!alerting && now - last_draw > 250) {
            last_draw = now;
            d.fillRect(0, BODY_Y + 20, SCR_W, 80, T_BG);
            d.setTextColor(T_FG, T_BG);
            d.setCursor(4, BODY_Y + 22);
            d.printf("baseline : %d dBm", baseline);
            d.setCursor(4, BODY_Y + 34);
            d.printf("trigger  : %d dBm", trigger);
            uint16_t cur_col = (cur_rssi >= trigger) ? T_BAD
                             : (cur_rssi >= trigger - 5) ? T_WARN : T_GOOD;
            d.setTextColor(cur_col, T_BG);
            d.setCursor(4, BODY_Y + 46);
            d.printf("current  : %d dBm", cur_rssi);
            d.setTextColor(T_FG, T_BG);
            d.setCursor(4, BODY_Y + 58);
            d.printf("peak     : %d dBm", peak_live);
            d.setTextColor(alerts > 0 ? T_BAD : T_DIM, T_BG);
            d.setCursor(4, BODY_Y + 70);
            d.printf("alerts   : %lu", (unsigned long)alerts);
            ui_draw_status(radio_name(), "jamdet");
        }

        uint16_t k = input_poll();
        if (k == PK_ESC) break;
        if (k == '?') {
            ui_show_current_help();
            ui_draw_footer("`=stop  ?=help");
            last_draw = 0;
        }
        delay(30);
    }

    cc1101_set_idle();
    cc1101_end();
}
