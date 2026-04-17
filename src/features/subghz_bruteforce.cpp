/*
 * subghz_bruteforce.cpp — protocol-specific code brute force.
 *
 * Supports fixed-code protocols: Came 12bit, Nice 12bit, Linear 10bit,
 * Chamberlain 9bit, Holtek 12bit, Ansonic 12bit.
 */
#include "../app.h"
#include "../theme.h"
#include "../ui.h"
#include "../input.h"
#include "../radio.h"
#include "../cc1101_hw.h"
#include <ELECHOUSE_CC1101_SRC_DRV.h>
#include <RCSwitch.h>

struct brute_proto_t {
    const char *name;
    int bits;
    int rc_proto;     /* RCSwitch protocol number */
    float freq;
    int pulse_len;
};

static const brute_proto_t PROTOS[] = {
    { "Came 12bit",       12, 1, 433.92f, 320 },
    { "Nice 12bit",       12, 1, 433.92f, 700 },
    { "Chamberlain 9bit",  9, 6, 315.00f, 1500 },
    { "Linear 10bit",     10, 6, 300.00f, 500 },
    { "Holtek 12bit",     12, 5, 433.92f, 500 },
    { "Ansonic 12bit",    12, 1, 433.92f, 380 },
};
#define PROTO_COUNT (sizeof(PROTOS)/sizeof(PROTOS[0]))

void feat_subghz_bruteforce(void)
{
    auto &d = M5Cardputer.Display;
    int sel = 0;
    while (true) {
        ui_clear_body();
        d.setTextColor(T_ACCENT2, T_BG);
        d.setCursor(4, BODY_Y + 2); d.print("BRUTE FORCE");
        d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT2);
        for (int i = 0; i < (int)PROTO_COUNT; ++i) {
            int y = BODY_Y + 18 + i * 13;
            bool s = (i == sel);
            if (s) d.fillRoundRect(2, y - 1, SCR_W - 4, 12, 2, 0x3007);
            d.setTextColor(s ? T_ACCENT : T_FG, s ? 0x3007 : T_BG);
            d.setCursor(8, y); d.printf("%s (%d bit)", PROTOS[i].name, PROTOS[i].bits);
        }
        ui_draw_footer(";/.=sel  ENTER=start  ESC=back");
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) return;
        if (k == ';' || k == PK_UP)   sel = (sel - 1 + PROTO_COUNT) % PROTO_COUNT;
        if (k == '.' || k == PK_DOWN) sel = (sel + 1) % PROTO_COUNT;
        if (k == PK_ENTER) break;
    }

    const brute_proto_t &p = PROTOS[sel];
    radio_switch(RADIO_SUBGHZ);
    if (!cc1101_begin(p.freq)) {
        ui_toast("CC1101 not found", T_BAD, 1500);
        radio_switch(RADIO_NONE);
        return;
    }

    RCSwitch tx;
    tx.enableTransmit(CC1101_GDO0);
    tx.setProtocol(p.rc_proto);
    tx.setPulseLength(p.pulse_len);
    tx.setRepeatTransmit(3);

    uint32_t total = 1UL << p.bits;
    uint32_t code = 0;
    bool running = true;
    uint32_t last_draw = 0;

    while (running && code < total) {
        /* Transmit current code. */
        cc1101_set_tx();
        tx.send(code, p.bits);
        cc1101_set_idle();
        code++;

        uint32_t now = millis();
        if (now - last_draw > 200) {
            last_draw = now;
            ui_clear_body();
            ui_draw_status(radio_name(), "brute");
            d.setTextColor(T_BAD, T_BG);
            d.setCursor(4, BODY_Y + 2); d.printf("BRUTE: %s", p.name);
            d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_BAD);
            d.setTextColor(T_FG, T_BG);
            d.setCursor(4, BODY_Y + 20); d.printf("code %lu / %lu", (unsigned long)code, (unsigned long)total);
            /* Progress bar. */
            int bw = (int)((code * (SCR_W - 16)) / total);
            d.drawRect(4, BODY_Y + 36, SCR_W - 8, 12, T_ACCENT);
            d.fillRect(6, BODY_Y + 38, bw, 8, T_ACCENT2);
            d.setCursor(4, BODY_Y + 54);
            d.printf("%.1f%% complete", (code * 100.0f) / total);
            ui_draw_footer("ESC=abort");
        }

        uint16_t k = input_poll();
        if (k == PK_ESC) running = false;
        delay(2);
    }

    tx.disableTransmit();
    cc1101_end();
    radio_switch(RADIO_NONE);
    ui_toast(running ? "done" : "aborted", running ? T_GOOD : T_WARN, 1000);
}
