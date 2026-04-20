/*
 * subghz_scan.cpp — CC1101 raw signal detector + capture.
 *
 * Uses interrupt-driven edge counting on GDO0 to detect signal bursts,
 * then captures timing data via tight polling loop. No RCSwitch.
 */
#include "../app.h"
#include "../theme.h"
#include "../ui.h"
#include "../input.h"
#include "../radio.h"
#include "../cc1101_hw.h"
#include "../sd_helper.h"
#include "../subghz_decode.h"
#include "../subghz_types.h"
#include <ELECHOUSE_CC1101_SRC_DRV.h>
#include <SD.h>

#define MAX_PULSES 512
#define MIN_PULSES 8
#define GAP_TIMEOUT_US 50000
#define MIN_PULSE_US 50

/* Check if captured pulses look like a real signal vs random noise.
 * Real OOK signals have at least some repeated pulse durations. */
static bool is_real_signal(const int16_t *p, int n)
{
    if (n < MIN_PULSES) return false;
    /* Count how many pulse durations appear more than once (±30%). */
    int matches = 0;
    for (int i = 0; i < n && i < 40; ++i) {
        int d = abs(p[i]);
        if (d < MIN_PULSE_US) continue;
        for (int j = i + 1; j < n && j < 40; ++j) {
            int d2 = abs(p[j]);
            if (d2 > d * 7 / 10 && d2 < d * 13 / 10) { matches++; break; }
        }
    }
    return matches >= 3;
}

static int16_t s_pulses[MAX_PULSES];
static volatile int s_pulse_count = 0;
static volatile uint32_t s_isr_edges = 0;
static volatile uint32_t s_last_edge_us = 0;

/* Published last-capture so replay/record features can offer "play the
 * last thing you just caught" without forcing an SD save. Reset to
 * empty whenever a new scan session starts. */
subghz_capture_t g_subghz_last_cap = {};
bool             g_subghz_last_valid = false;

static const float COMMON_FREQS[] = {
    300.00, 303.875, 310.00, 315.00, 318.00,
    390.00, 418.00, 433.07, 433.42, 433.92, 434.42,
    868.00, 868.35, 915.00, 925.00
};
#define FREQ_COUNT (sizeof(COMMON_FREQS)/sizeof(COMMON_FREQS[0]))

/* ISR: just count edges + record timestamp. Minimal work. */
static void IRAM_ATTR gdo0_isr(void)
{
    s_isr_edges++;
    s_last_edge_us = micros();
}

/* Capture pulse timings in a tight poll loop. Call when ISR has
 * detected a burst of edges. Returns pulse count. */
static int capture_now(int16_t *out, int max)
{
    int n = 0;
    uint8_t prev = digitalRead(CC1101_GDO0);
    uint32_t t = micros();

    while (n < max) {
        uint8_t cur = digitalRead(CC1101_GDO0);
        uint32_t now = micros();
        if (cur != prev) {
            int32_t dur = (int32_t)(now - t);
            if (dur >= MIN_PULSE_US && dur < 60000)
                out[n++] = prev ? (int16_t)dur : -(int16_t)dur;
            t = now;
            prev = cur;
        } else if (now - t > GAP_TIMEOUT_US) {
            break;
        }
    }
    return n;
}

static float s_save_freq = 433.92f;

static bool save_sub(float freq, const int16_t *p, int count)
{
    /* Full CC1101 teardown so FSPI releases GPIO matrix. */
    cc1101_end();
    delay(10);

    /* Force fresh SD remount on HSPI. */
    bool ok = false;
    if (sd_remount()) {
        SD.mkdir("/poseidon/signals/custom");
        char path[64];
        snprintf(path, sizeof(path), "/poseidon/signals/custom/cap-%lu.sub",
                 (unsigned long)(millis() / 1000));
        File f = SD.open(path, FILE_WRITE);
        if (f) {
            f.println("Filetype: Flipper SubGhz RAW File");
            f.println("Version: 1");
            f.printf("Frequency: %lu\n", (unsigned long)(freq * 1000000));
            f.println("Preset: FuriHalSubGhzPresetOok270Async");
            f.println("Protocol: RAW");
            int col = 0;
            for (int i = 0; i < count; ++i) {
                if (col == 0) f.print("RAW_Data: ");
                f.printf("%d ", p[i]);
                if (++col >= 512) { f.println(); col = 0; }
            }
            if (col) f.println();
            f.close();
            ok = true;
        }
    }

    /* Re-init CC1101 from scratch. */
    cc1101_begin(freq);
    ELECHOUSE_cc1101.SetRx();
    pinMode(CC1101_GDO0, INPUT);
    return ok;
}

void feat_subghz_scan(void)
{
    radio_switch(RADIO_SUBGHZ);
    if (!cc1101_begin(433.92f)) {
        ui_toast("CC1101 not found", T_BAD, 1500);
        radio_switch(RADIO_NONE);
        return;
    }

    ELECHOUSE_cc1101.SetRx();
    pinMode(CC1101_GDO0, INPUT);

    /* Attach interrupt for fast edge detection. */
    s_isr_edges = 0;
    attachInterrupt(digitalPinToInterrupt(CC1101_GDO0), gdo0_isr, CHANGE);

    auto &d = M5Cardputer.Display;
    float freq = 433.92f;
    uint32_t captures = 0;
    bool has_capture = false;
    uint32_t last_draw = 0;
    uint32_t prev_edges = 0;
    subghz_decoded_t decoded = {};

    while (true) {
        /* Check if ISR detected a burst of edges. Use rate-based
         * detection: if edges jumped significantly since last check. */
        uint32_t edges = s_isr_edges;
        if (edges > prev_edges + 8) {
            /* Signal burst detected — capture immediately. */
            detachInterrupt(digitalPinToInterrupt(CC1101_GDO0));
            s_pulse_count = capture_now(s_pulses, MAX_PULSES);
            if (s_pulse_count >= MIN_PULSES) {
                captures++;
                has_capture = true;
                decoded = subghz_decode(s_pulses, s_pulse_count);
                last_draw = 0;
                /* Publish to the shared global so replay/record can
                 * consume it without needing an SD round-trip. */
                int cap_n = s_pulse_count < SUBGHZ_MAX_PULSES
                          ? s_pulse_count : SUBGHZ_MAX_PULSES;
                memcpy(g_subghz_last_cap.pulses, s_pulses,
                       cap_n * sizeof(int16_t));
                g_subghz_last_cap.pulse_count = cap_n;
                g_subghz_last_cap.freq_mhz    = freq;
                g_subghz_last_cap.ts          = millis();
                g_subghz_last_valid           = true;
            }
            /* Re-attach interrupt. */
            s_isr_edges = 0;
            prev_edges = 0;
            attachInterrupt(digitalPinToInterrupt(CC1101_GDO0), gdo0_isr, CHANGE);
        } else {
            prev_edges = edges;
        }

        uint32_t now = millis();
        if (now - last_draw > 300) {
            last_draw = now;
            if (last_draw == 0) ui_clear_body();  /* first frame only */
            ui_draw_status(radio_name(), "scan");

            ui_text(4, BODY_Y + 2, T_ACCENT2, "SCAN %.3f MHz", freq);
            d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT2);

            ui_text(4, BODY_Y + 18, T_FG, "captures: %lu  isr: %lu",
                    (unsigned long)captures, (unsigned long)s_isr_edges);
            ui_text(4, BODY_Y + 30, T_FG, "rssi: %d dBm", cc1101_get_rssi());

            if (has_capture) {
                if (decoded.valid) {
                    ui_text(4, BODY_Y + 44, T_GOOD, "%s  %lu (%u bit)",
                            decoded.protocol, (unsigned long)decoded.value, decoded.bits);
                } else {
                    ui_text(4, BODY_Y + 44, T_WARN, "RAW %d pulses (no protocol match)", s_pulse_count);
                }
                int mid = BODY_Y + 72;
                d.fillRect(4, mid - 11, SCR_W - 8, 23, T_BG);
                d.drawFastHLine(4, mid, SCR_W - 8, T_DIM);
                int x = 4;
                for (int i = 0; i < s_pulse_count && x < SCR_W - 4; ++i) {
                    int pw = abs(s_pulses[i]) / 80;
                    if (pw < 1) pw = 1;
                    if (pw > 30) pw = 30;
                    uint16_t c = s_pulses[i] > 0 ? T_ACCENT : T_ACCENT2;
                    d.fillRect(x, s_pulses[i] > 0 ? mid - 10 : mid + 1, pw, 10, c);
                    x += pw;
                }
            } else {
                ui_text(4, BODY_Y + 44, T_DIM, "waiting for signal...");
                ui_text(4, BODY_Y + 56, T_DIM, "press a remote nearby");
            }

            ui_draw_footer(has_capture ? "S=save R=replay +-=freq A=scan ESC=exit"
                                       : "+-=freq A=autoscan ESC=exit");
        }

        uint16_t k = input_poll();
        if (k == PK_ESC) break;
        if (k == '+' || k == '=') {
            freq += 0.5f;
            ELECHOUSE_cc1101.setSidle();
            ELECHOUSE_cc1101.setMHZ(freq);
            ELECHOUSE_cc1101.SetRx();
            pinMode(CC1101_GDO0, INPUT);
            s_isr_edges = 0; prev_edges = 0;
        }
        if (k == '-') {
            freq -= 0.5f;
            ELECHOUSE_cc1101.setSidle();
            ELECHOUSE_cc1101.setMHZ(freq);
            ELECHOUSE_cc1101.SetRx();
            pinMode(CC1101_GDO0, INPUT);
            s_isr_edges = 0; prev_edges = 0;
        }
        if (k == 'a' || k == 'A') {
            detachInterrupt(digitalPinToInterrupt(CC1101_GDO0));
            ui_toast("scanning...", T_ACCENT, 200);
            int best_rssi = -200; float best_freq = freq;
            for (int i = 0; i < (int)FREQ_COUNT; ++i) {
                ELECHOUSE_cc1101.setSidle();
                ELECHOUSE_cc1101.setMHZ(COMMON_FREQS[i]);
                ELECHOUSE_cc1101.SetRx();
                delay(15);
                int r = cc1101_get_rssi();
                if (r > best_rssi) { best_rssi = r; best_freq = COMMON_FREQS[i]; }
                yield();
            }
            freq = best_freq;
            ELECHOUSE_cc1101.setSidle();
            ELECHOUSE_cc1101.setMHZ(freq);
            ELECHOUSE_cc1101.SetRx();
            pinMode(CC1101_GDO0, INPUT);
            s_isr_edges = 0; prev_edges = 0;
            attachInterrupt(digitalPinToInterrupt(CC1101_GDO0), gdo0_isr, CHANGE);
            char msg[48];
            snprintf(msg, sizeof(msg), "%.3f MHz rssi %d", freq, best_rssi);
            ui_toast(msg, T_GOOD, 800);
        }
        if ((k == 's' || k == 'S') && has_capture) {
            detachInterrupt(digitalPinToInterrupt(CC1101_GDO0));
            if (save_sub(freq, s_pulses, s_pulse_count))
                ui_toast("saved", T_GOOD, 600);
            else
                ui_toast("save fail", T_BAD, 800);
            s_isr_edges = 0; prev_edges = 0;
            attachInterrupt(digitalPinToInterrupt(CC1101_GDO0), gdo0_isr, CHANGE);
        }
        if ((k == 'r' || k == 'R') && has_capture) {
            detachInterrupt(digitalPinToInterrupt(CC1101_GDO0));
            pinMode(CC1101_GDO0, OUTPUT);
            ELECHOUSE_cc1101.SetTx();
            for (int i = 0; i < s_pulse_count; ++i) {
                digitalWrite(CC1101_GDO0, s_pulses[i] > 0 ? HIGH : LOW);
                delayMicroseconds(abs(s_pulses[i]));
            }
            digitalWrite(CC1101_GDO0, LOW);
            ELECHOUSE_cc1101.setSidle();
            ELECHOUSE_cc1101.SetRx();
            pinMode(CC1101_GDO0, INPUT);
            s_isr_edges = 0; prev_edges = 0;
            attachInterrupt(digitalPinToInterrupt(CC1101_GDO0), gdo0_isr, CHANGE);
            ui_toast("replayed", T_GOOD, 400);
        }
        delay(1);
    }

    detachInterrupt(digitalPinToInterrupt(CC1101_GDO0));
    cc1101_end();
    radio_switch(RADIO_NONE);
}
