/*
 * subghz_record.cpp — RAW signal recording via RMT peripheral.
 *
 * Captures precise pulse timings from CC1101 GDO0 into a buffer,
 * then saves as a Flipper-compatible .sub file on SD.
 */
#include "../app.h"
#include "../theme.h"
#include "../ui.h"
#include "../input.h"
#include "../radio.h"
#include "../cc1101_hw.h"
#include "../sd_helper.h"
#include <SD.h>
#include <ELECHOUSE_CC1101_SRC_DRV.h>
/* driver/rmt.h (legacy) removed — conflicts with IDF 5.5 driver_ng at
 * boot. rmt_rx_init() is a stub returning false; the feature aborts
 * cleanly and shows a toast. Migration to the new rmt_rx.h API pending. */

#define RAW_MAX_PULSES 4096
#define RMT_GPIO       17  /* CC1101_GDO0 placeholder for future migration */

/* Full CC1101 frequency table covering all three bands. */
static const float SCAN_FREQS[] = {
    300.00, 303.875, 304.25, 310.00, 315.00, 318.00,
    330.00, 340.00, 345.00, 348.00,
    390.00, 403.00, 418.00, 430.00, 431.00, 433.07,
    433.42, 433.92, 434.42, 434.775, 438.90, 440.00,
    450.00, 458.00, 464.00,
    779.00, 868.00, 868.30, 868.35, 868.865, 869.50,
    900.00, 903.00, 906.875, 910.00, 915.00, 916.00,
    920.00, 925.00, 928.00
};
#define SCAN_FREQ_COUNT (sizeof(SCAN_FREQS)/sizeof(SCAN_FREQS[0]))

static int16_t *s_raw = nullptr;
static int      s_raw_len = 0;
static volatile bool s_recording = false;

static bool rmt_rx_init(void)
{
    /* Legacy RMT driver removed for v0.4 platform migration — returns
     * false so the feature exits cleanly with a user-visible toast. */
    return false;
}

/* rmt_rx_capture inlined into the record loop with live waveform. */

static bool save_sub_file(const char *path, float freq, const int16_t *raw, int len)
{
    File f = SD.open(path, FILE_WRITE);
    if (!f) return false;
    f.println("Filetype: Flipper SubGhz RAW File");
    f.println("Version: 1");
    f.printf("Frequency: %lu\n", (unsigned long)(freq * 1000000));
    f.println("Preset: FuriHalSubGhzPresetOok270Async");
    f.println("Protocol: RAW");
    int col = 0;
    for (int i = 0; i < len; ++i) {
        if (col == 0) f.print("RAW_Data: ");
        f.printf("%d ", raw[i]);
        col++;
        if (col >= 512) { f.println(); col = 0; }
    }
    if (col) f.println();
    f.close();
    return true;
}

void feat_subghz_record(void)
{
    radio_switch(RADIO_SUBGHZ);
    if (!cc1101_begin(433.92f)) {
        ui_toast("CC1101 not found", T_BAD, 1500);
        radio_switch(RADIO_NONE);
        return;
    }

    s_raw = (int16_t *)malloc(RAW_MAX_PULSES * sizeof(int16_t));
    if (!s_raw) {
        ui_toast("OOM", T_BAD, 1500);
        cc1101_end();
        radio_switch(RADIO_NONE);   /* drop s_active back so next feature re-arms cleanly */
        return;
    }

    /* Set widest RX bandwidth for maximum sensitivity. */
    ELECHOUSE_cc1101.setRxBW(270);  /* match Flipper OOK270 preset */

    auto &d = M5Cardputer.Display;
    float freq = 433.92f;
    s_raw_len = 0;
    bool recorded = false;

    while (true) {
        ui_clear_body();
        ui_draw_status(radio_name(), "record");
        d.setTextColor(T_ACCENT2, T_BG);
        d.setCursor(4, BODY_Y + 2); d.printf("RAW RECORD  %.3f MHz", freq);
        d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT2);

        if (recorded) {
            d.setTextColor(T_GOOD, T_BG);
            d.setCursor(4, BODY_Y + 20); d.printf("captured %d pulses", s_raw_len);
            /* Mini waveform preview */
            int mid_y = BODY_Y + 55;
            d.drawFastHLine(4, mid_y, SCR_W - 8, T_DIM);
            int x = 4;
            for (int i = 0; i < s_raw_len && x < SCR_W - 4; ++i) {
                int w = abs(s_raw[i]) / 50;
                if (w < 1) w = 1;
                if (w > 30) w = 30;
                int y = s_raw[i] > 0 ? mid_y - 10 : mid_y + 1;
                uint16_t c = s_raw[i] > 0 ? T_ACCENT : T_ACCENT2;
                d.fillRect(x, y, w, 10, c);
                x += w;
            }
            ui_draw_footer("S=save  R=retry  ESC=quit");
        } else {
            d.setTextColor(T_WARN, T_BG);
            d.setCursor(4, BODY_Y + 30); d.print("press ENTER to record");
            d.setCursor(4, BODY_Y + 42); d.print("press A to auto-scan all bands");
            d.setTextColor(T_DIM, T_BG);
            d.setCursor(4, BODY_Y + 54); d.print("RxBW=270 kHz (OOK270)  20s limit");
            ui_draw_footer("ENTER=rec  A=autoscan  +-=freq  ESC=quit");
        }

        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) break;
        if (k == '+' || k == '=') { freq += 0.5f; cc1101_set_freq(freq); recorded = false; }
        if (k == '-')             { freq -= 0.5f; cc1101_set_freq(freq); recorded = false; }
        if ((k == 'a' || k == 'A') && !recorded) {
            /* Auto-scan: sweep all bands, find strongest RSSI, lock. */
            ui_clear_body();
            d.setTextColor(T_ACCENT, T_BG);
            d.setCursor(4, BODY_Y + 30); d.print("scanning all bands...");
            int best_rssi = -200;
            float best_freq = freq;
            int total_steps = 2 * (int)SCAN_FREQ_COUNT;
            int step = 0;
            for (int pass = 0; pass < 2; ++pass) {
                for (int i = 0; i < (int)SCAN_FREQ_COUNT; ++i) {
                    ELECHOUSE_cc1101.setMHZ(SCAN_FREQS[i]);
                    ELECHOUSE_cc1101.SetRx();
                    delay(12);
                    int r = cc1101_get_rssi();
                    if (r > best_rssi) { best_rssi = r; best_freq = SCAN_FREQS[i]; }
                    step++;
                    /* Progress bar + current freq. */
                    int bw = (step * (SCR_W - 8)) / total_steps;
                    d.fillRect(4, BODY_Y + 44, bw, 4, T_ACCENT);
                    d.fillRect(4, BODY_Y + 52, 160, 10, T_BG);
                    d.setTextColor(T_DIM, T_BG);
                    d.setCursor(4, BODY_Y + 52);
                    d.printf("%.3f MHz  rssi %d", SCAN_FREQS[i], r);
                    yield();  /* feed watchdog */
                }
            }
            freq = best_freq;
            cc1101_set_freq(freq);
            char msg[48];
            snprintf(msg, sizeof(msg), "locked %.3f MHz (rssi %d)", freq, best_rssi);
            ui_toast(msg, T_GOOD, 1200);
        }

        if (k == PK_ENTER && !recorded) {
            /* Record path disabled pending legacy-RMT → driver_ng migration
             * (v0.4.x). See top of file. Scan / replay menu still renders. */
            ui_toast("record unavailable v0.4", T_WARN, 1500);
        }
        if (k == 'r' || k == 'R') { recorded = false; s_raw_len = 0; }
        if ((k == 's' || k == 'S') && recorded) {
            /* Teardown CC1101 so FSPI releases GPIO matrix for SD's HSPI. */
            cc1101_end();
            delay(10);
            if (sd_remount()) {
                char path[64];
                snprintf(path, sizeof(path), "/poseidon/signals/custom/raw-%lu.sub",
                         (unsigned long)(millis()/1000));
                SD.mkdir("/poseidon/signals/custom");
                if (save_sub_file(path, freq, s_raw, s_raw_len))
                    ui_toast("saved", T_GOOD, 800);
                else
                    ui_toast("save fail", T_BAD, 1000);
            } else {
                ui_toast("SD remount fail", T_BAD, 1000);
            }
            /* Re-init CC1101. */
            cc1101_begin(freq);
            ELECHOUSE_cc1101.SetRx();
            pinMode(CC1101_GDO0, INPUT);
        }
    }

    free(s_raw); s_raw = nullptr;
    cc1101_end();
    radio_switch(RADIO_NONE);
}
