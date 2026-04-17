/*
 * subghz_jammer.cpp — intermittent + continuous carrier jammer.
 *
 * Two modes:
 *   Intermittent: random-width pulses to disrupt without saturating
 *   Full carrier: constant TX for maximum denial (20s safety cap)
 */
#include "../app.h"
#include "../theme.h"
#include "../ui.h"
#include "../input.h"
#include "../radio.h"
#include "../cc1101_hw.h"
#include <ELECHOUSE_CC1101_SRC_DRV.h>

#define JAM_MAX_MS 20000

void feat_subghz_jammer(void)
{
    radio_switch(RADIO_SUBGHZ);
    if (!cc1101_begin(433.92f)) {
        ui_toast("CC1101 not found", T_BAD, 1500);
        radio_switch(RADIO_NONE);
        return;
    }

    auto &d = M5Cardputer.Display;
    float freq = 433.92f;
    int mode = 0;  /* 0=intermittent, 1=full carrier */
    bool active = false;
    uint32_t jam_start = 0;

    while (true) {
        uint32_t now = millis();
        if (active && now - jam_start > JAM_MAX_MS) {
            cc1101_set_idle();
            active = false;
            ui_toast("20s limit", T_WARN, 800);
        }

        if (active && mode == 0) {
            /* Intermittent: random on/off bursts. */
            cc1101_set_tx();
            delayMicroseconds(esp_random() % 2000 + 500);
            cc1101_set_idle();
            delayMicroseconds(esp_random() % 1000 + 200);
        }

        ui_clear_body();
        ui_draw_status(radio_name(), "jammer");
        d.setTextColor(active ? T_BAD : T_ACCENT2, T_BG);
        d.setCursor(4, BODY_Y + 2);
        d.printf("JAMMER %s", active ? "ACTIVE" : "ARMED");
        d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, active ? T_BAD : T_ACCENT2);

        d.setTextColor(T_FG, T_BG);
        d.setCursor(4, BODY_Y + 20); d.printf("freq: %.3f MHz", freq);
        d.setCursor(4, BODY_Y + 32);
        d.printf("mode: %s", mode == 0 ? "INTERMITTENT" : "FULL CARRIER");

        if (active) {
            uint32_t elapsed = (now - jam_start) / 1000;
            uint32_t remain  = (JAM_MAX_MS / 1000) - elapsed;
            d.setTextColor(T_BAD, T_BG);
            d.setCursor(4, BODY_Y + 48);
            d.printf("TX  %lus / %ds max", (unsigned long)elapsed, JAM_MAX_MS / 1000);

            /* Animated TX bars. */
            for (int i = 0; i < 8; ++i) {
                int h = (esp_random() % 20) + 5;
                int x = 10 + i * 28;
                d.fillRect(x, BODY_Y + 70, 20, h, T_BAD);
            }
        }
        ui_draw_footer(active ? "ESC=STOP" : "ENTER=start  M=mode  +-=freq  ESC=quit");

        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(active ? 2 : 20); continue; }
        if (k == PK_ESC) {
            if (active) { cc1101_set_idle(); active = false; }
            else break;
        }
        if (!active) {
            if (k == 'm' || k == 'M') mode = 1 - mode;
            if (k == '+' || k == '=') { freq += 0.5f; cc1101_set_freq(freq); }
            if (k == '-')             { freq -= 0.5f; cc1101_set_freq(freq); }
            if (k == PK_ENTER) {
                active = true;
                jam_start = millis();
                cc1101_set_freq(freq);
                if (mode == 1) cc1101_set_tx();
            }
        }
    }

    cc1101_end();
    radio_switch(RADIO_NONE);
}
