/*
 * ir_clone — multi-profile IR remote tool.
 *
 * v1 ships with three built-in TV profiles (Samsung / LG / Sony) so
 * you can drive a TV out of the box without capturing anything. Each
 * profile is a labeled list of buttons; selecting a profile opens a
 * grid where individual buttons fire IR codes via the Cardputer-Adv's
 * onboard IR TX LED on GPIO 44.
 *
 * v2 will add capture (IR RX on GPIO 1 via Adafruit-IRremote) and
 * Flipper-Zero-compatible `.ir` file storage at /poseidon/ir/*.ir,
 * letting you clone any remote into a new profile and share files
 * with the community.
 *
 * Protocols supported now:
 *   - NEC (38 kHz, Samsung + LG variant address/cmd layouts)
 *   - SIRC 12-bit (40 kHz, Sony)
 */
#include "app.h"
#include "../theme.h"
#include "ui.h"
#include "input.h"
#include "ir_clone.h"
#include <driver/ledc.h>
#include <driver/gpio.h>
#include <ctype.h>

#include "../ir_hw.h"

/* ---- carrier control (lifted from ir_remote.cpp) ---- */

static int s_carrier_hz = 38000;
static int s_half_us = 13;  /* 1e6 / (2 * carrier_hz) */

static void carrier_setup(int hz)
{
    s_carrier_hz = hz;
    s_half_us = 500000 / hz;  /* half-period in us */
    if (s_half_us < 1) s_half_us = 1;
    gpio_reset_pin((gpio_num_t)IR_TX_PIN);
    pinMode(IR_TX_PIN, OUTPUT);
    digitalWrite(IR_TX_PIN, IR_TX_OFF_LEVEL);  /* emitter OFF (board-correct polarity) */
}

/* Bit-banged carrier — see ir_remote.cpp for why we don't use LEDC.
 * Active-LOW: pin LOW = LED on. */
static inline void mark(uint16_t us)
{
    uint32_t end = micros() + us;
    int half = s_half_us;
    while ((int32_t)(end - micros()) > 0) {
        digitalWrite(IR_TX_PIN, HIGH);
        delayMicroseconds(half);
        digitalWrite(IR_TX_PIN, LOW);
        delayMicroseconds(half);
    }
}
static inline void space(uint16_t us)
{
    digitalWrite(IR_TX_PIN, IR_TX_OFF_LEVEL);  /* emitter OFF (board-correct polarity) */
    if (us) delayMicroseconds(us);
}

/* ---- NEC senders (Samsung + LG) ----
 *
 * Both use the 4500/4500 us short header (NOT the full-NEC 9000/4500).
 * The 9000us mark caused brownout resets on USB-bus-powered units —
 * twice as much LED-on time at the start of every burst. Samsung's
 * spec is 4500/4500 anyway; LG receivers tolerate both. Matches the
 * working timings in feat_ir_remote.cpp. */

static void send_nec_short(uint8_t a0, uint8_t a1, uint8_t c0, uint8_t c1)
{
    carrier_setup(38000);
    mark(4500); space(4500);
    uint8_t bytes[4] = { a0, a1, c0, c1 };
    for (int b = 0; b < 4; b++) {
        for (int i = 0; i < 8; i++) {
            mark(560);
            space((bytes[b] & (1 << i)) ? 1690 : 560);
        }
    }
    mark(560); space(0);
}

/* Samsung TV — addr 0x07, NEC checksum-style ~addr = 0xF8
 * Non-static so prank drivers in ir_extras_data.h can link to it. */
void send_samsung(uint8_t cmd)
{
    send_nec_short(0x07, 0xF8, cmd, (uint8_t)~cmd);
}

/* LG TV — addr 0x04, ~addr = 0xFB */
void send_lg(uint8_t cmd)
{
    send_nec_short(0x04, 0xFB, cmd, (uint8_t)~cmd);
}

/* ---- Sony SIRC 12-bit ----
 *
 * Sony spec uses 40 kHz carrier but most receivers tolerate 38 kHz.
 * Switching LEDC mid-feature (38→40 kHz) was glitching the timer and
 * causing resets. Sticking to 38 kHz across all protocols avoids the
 * reconfig hazard and works for nearly all SIRC TVs. */

void send_sony12(uint8_t cmd_7bit, uint8_t addr_5bit)
{
    carrier_setup(38000);  /* shared with Samsung/LG — no reconfig */
    /* Header: 2.4 ms mark, 0.6 ms space */
    mark(2400); space(600);
    /* 7 command bits LSB-first */
    for (int i = 0; i < 7; i++) {
        bool one = (cmd_7bit >> i) & 1;
        mark(one ? 1200 : 600);
        space(600);
    }
    /* 5 address bits LSB-first */
    for (int i = 0; i < 5; i++) {
        bool one = (addr_5bit >> i) & 1;
        mark(one ? 1200 : 600);
        space(600);
    }
    space(0);
}

/* ---- profile model ---- */

enum ir_proto_t {
    IR_PROTO_SAMSUNG = 0,
    IR_PROTO_LG      = 1,
    IR_PROTO_SONY    = 2,
};

struct ir_btn_t {
    const char *label;
    char        key;        /* keyboard char to fire it */
    uint8_t     cmd;        /* protocol-specific command byte */
    uint8_t     addr;       /* Sony only (5-bit) */
};

struct ir_profile_t {
    const char     *name;
    const char     *device;     /* human description */
    ir_proto_t      proto;
    const ir_btn_t *btns;
    int             n_btns;
};

/* ---- Samsung TV preset (18 buttons, lifted from ir_remote.cpp) ---- */

static const ir_btn_t SAMSUNG_BTNS[] = {
    { "Power",  'p', 0x40, 0 }, { "Mute",   'm', 0x0F, 0 },
    { "Vol+",   '+', 0x07, 0 }, { "Vol-",   '-', 0x0B, 0 },
    { "Ch+",    ';', 0x12, 0 }, { "Ch-",    '.', 0x10, 0 },
    { "Source", 'i', 0x01, 0 }, { "Home",   'h', 0x79, 0 },
    { "Back",   'b', 0x58, 0 }, { "Menu",   'n', 0x1A, 0 },
    { "Enter",  'e', 0x68, 0 }, { "Exit",   'x', 0x2D, 0 },
    { "1",      '1', 0x04, 0 }, { "2",      '2', 0x05, 0 },
    { "3",      '3', 0x06, 0 }, { "4",      '4', 0x08, 0 },
    { "5",      '5', 0x09, 0 }, { "6",      '6', 0x0A, 0 },
    { "7",      '7', 0x0C, 0 }, { "8",      '8', 0x0D, 0 },
    { "9",      '9', 0x0E, 0 }, { "0",      '0', 0x11, 0 },
};

/* ---- LG TV preset ---- */

static const ir_btn_t LG_BTNS[] = {
    { "Power",  'p', 0x08, 0 }, { "Mute",   'm', 0x09, 0 },
    { "Vol+",   '+', 0x02, 0 }, { "Vol-",   '-', 0x03, 0 },
    { "Ch+",    ';', 0x00, 0 }, { "Ch-",    '.', 0x01, 0 },
    { "Input",  'i', 0x0B, 0 }, { "Menu",   'n', 0x43, 0 },
    { "Back",   'b', 0x28, 0 }, { "Home",   'h', 0x7E, 0 },
    { "Enter",  'e', 0x44, 0 }, { "Exit",   'x', 0x5B, 0 },
    { "Up",     'u', 0x40, 0 }, { "Down",   'd', 0x41, 0 },
    { "Left",   'l', 0x07, 0 }, { "Right",  'r', 0x06, 0 },
    { "1",      '1', 0x10, 0 }, { "2",      '2', 0x11, 0 },
    { "3",      '3', 0x12, 0 }, { "4",      '4', 0x13, 0 },
    { "5",      '5', 0x14, 0 }, { "6",      '6', 0x15, 0 },
    { "7",      '7', 0x16, 0 }, { "8",      '8', 0x17, 0 },
    { "9",      '9', 0x18, 0 }, { "0",      '0', 0x10, 0 },
};

/* ---- Sony TV preset (SIRC, address 1) ---- */

static const ir_btn_t SONY_BTNS[] = {
    { "Power",  'p', 0x15, 1 }, { "Mute",   'm', 0x14, 1 },
    { "Vol+",   '+', 0x12, 1 }, { "Vol-",   '-', 0x13, 1 },
    { "Ch+",    ';', 0x10, 1 }, { "Ch-",    '.', 0x11, 1 },
    { "Input",  'i', 0x25, 1 }, { "Menu",   'n', 0x60, 1 },
    { "Enter",  'e', 0x65, 1 }, { "Back",   'b', 0x23, 1 },
    { "Home",   'h', 0x60, 1 }, { "Exit",   'x', 0x63, 1 },
    { "1",      '1', 0x00, 1 }, { "2",      '2', 0x01, 1 },
    { "3",      '3', 0x02, 1 }, { "4",      '4', 0x03, 1 },
    { "5",      '5', 0x04, 1 }, { "6",      '6', 0x05, 1 },
    { "7",      '7', 0x06, 1 }, { "8",      '8', 0x07, 1 },
    { "9",      '9', 0x08, 1 }, { "0",      '0', 0x09, 1 },
};

static const ir_profile_t PROFILES[] = {
    { "Samsung TV", "TVs (most years)",   IR_PROTO_SAMSUNG,
      SAMSUNG_BTNS, (int)(sizeof(SAMSUNG_BTNS) / sizeof(SAMSUNG_BTNS[0])) },
    { "LG TV",      "TVs / soundbars",    IR_PROTO_LG,
      LG_BTNS,      (int)(sizeof(LG_BTNS)      / sizeof(LG_BTNS[0]))      },
    { "Sony TV",    "TVs (SIRC 12-bit)",  IR_PROTO_SONY,
      SONY_BTNS,    (int)(sizeof(SONY_BTNS)    / sizeof(SONY_BTNS[0]))    },
};
static const int PROFILES_N = (int)(sizeof(PROFILES) / sizeof(PROFILES[0]));

/* ---- sender dispatch ---- */

static void send_btn(const ir_profile_t &prof, const ir_btn_t &btn)
{
    Serial.printf("[ir_clone] tx proto=%d cmd=0x%02x addr=0x%02x label=%s\n",
                  (int)prof.proto, btn.cmd, btn.addr, btn.label);
    switch (prof.proto) {
    case IR_PROTO_SAMSUNG: send_samsung(btn.cmd); break;
    case IR_PROTO_LG:      send_lg(btn.cmd); break;
    case IR_PROTO_SONY:    send_sony12(btn.cmd, btn.addr); break;
    }
    /* Park pin HIGH (LED off) between bursts. */
    digitalWrite(IR_TX_PIN, IR_TX_OFF_LEVEL);
    delay(10);
}

/* ---- per-profile button screen ---- */

static void profile_screen(const ir_profile_t &prof)
{
    auto &d = M5Cardputer.Display;
    ui_clear_body();
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2); d.printf("%s", prof.name);
    d.setTextColor(T_DIM, T_BG);
    d.setCursor(SCR_W - 84, BODY_Y + 2);
    d.printf("(%d buttons)", prof.n_btns);
    d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT);
    ui_draw_footer("press button key  `=back");

    /* Render a button grid showing key + label. 4 cols. */
    const int cols = 4;
    const int col_w = SCR_W / cols;
    const int row_h = 10;
    for (int i = 0; i < prof.n_btns; i++) {
        int row = i / cols;
        int col = i % cols;
        int x = col * col_w + 2;
        int y = BODY_Y + 18 + row * row_h;
        if (y + row_h > FOOTER_Y - 14) break;
        d.setTextColor(T_ACCENT2, T_BG);
        d.setCursor(x, y); d.printf("[%c]", prof.btns[i].key);
        d.setTextColor(T_FG, T_BG);
        d.setCursor(x + 18, y); d.printf("%-6s", prof.btns[i].label);
    }

    while (true) {
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) return;

        /* Encoder path: profile buttons are all printable chars and no profile
         * contains PK_ENTER, so NO button could ever be sent on the T-Embed. */
        if (k == PK_ACTIONS || k == PK_ENTER) {
            const char *labels[24];
            int n = prof.n_btns; if (n > 24) n = 24;
            for (int i = 0; i < n; ++i) labels[i] = prof.btns[i].label;
            const int pick = ui_action_menu(prof.name, labels, n);
            if (pick < 0) continue;
            k = (uint16_t)(uint8_t)prof.btns[pick].key;
        }

        char ch = (char)tolower((int)k);
        for (int i = 0; i < prof.n_btns; i++) {
            if (prof.btns[i].key == ch) {
                send_btn(prof, prof.btns[i]);
                /* Flash status footer row. */
                d.fillRect(0, FOOTER_Y - 14, SCR_W, 12, T_BG);
                d.setTextColor(T_GOOD, T_BG);
                d.setCursor(4, FOOTER_Y - 12);
                d.printf("> %s (%s)", prof.btns[i].label, prof.name);
                break;
            }
        }
    }
}

/* ---- profile picker ---- */

void feat_ir_clone(void)
{
    pinMode(IR_TX_PIN, OUTPUT);
    digitalWrite(IR_TX_PIN, IR_TX_OFF_LEVEL);  /* emitter OFF (board-correct polarity) */
    /* Pre-arm carrier so the first send doesn't pay timer-init latency. */
    s_carrier_hz = 0;
    carrier_setup(38000);

    int cursor = 0;
    int prev   = -1;
    auto &d = M5Cardputer.Display;
    ui_draw_footer(";/. pick  ENTER=open  `=back");
    while (true) {
        if (cursor != prev) {
            prev = cursor;
            ui_clear_body();
            d.setTextColor(T_ACCENT, T_BG);
            d.setCursor(4, BODY_Y + 2); d.print("IR CLONE — profiles");
            d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT);
            for (int i = 0; i < PROFILES_N; i++) {
                int y = BODY_Y + 22 + i * 18;
                bool sel = (i == cursor);
                if (sel) d.fillRect(0, y - 2, SCR_W, 16, 0x3007);
                d.setTextColor(sel ? T_ACCENT : T_FG, sel ? 0x3007 : T_BG);
                d.setCursor(4, y); d.print(PROFILES[i].name);
                d.setTextColor(T_DIM, sel ? 0x3007 : T_BG);
                d.setCursor(4, y + 8); d.printf("%s — %d btns",
                                                PROFILES[i].device, PROFILES[i].n_btns);
            }
            /* Hint at v2 capture support. */
            d.setTextColor(T_DIM, T_BG);
            d.setCursor(4, FOOTER_Y - 22);
            d.print("(capture-from-remote: v2)");
        }
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(30); continue; }
        if (k == PK_ESC) break;
        if (k == ';' || k == PK_UP)   { if (cursor > 0) cursor--; }
        if (k == '.' || k == PK_DOWN) { if (cursor < PROFILES_N - 1) cursor++; }
        if (k == PK_ENTER) {
            profile_screen(PROFILES[cursor]);
            prev = -1;
            ui_draw_footer(";/. pick  ENTER=open  `=back");
        }
    }
    /* Park OFF on exit (active-LOW: HIGH=off). */
    digitalWrite(IR_TX_PIN, IR_TX_OFF_LEVEL);
}

/* =====================================================================
 * Prank drivers (ir_extras_data.h Section 6)
 *
 * blast_raw() is the bit-banger entry point the prank inlines expect.
 * It mirrors ir_tvbgone.cpp's blast() but takes (carrier_kHz, pairs)
 * directly so the prank helpers don't need to construct an ir_code_t.
 * ===================================================================*/

void blast_raw(uint16_t carrier_khz, const uint16_t *pairs)
{
    carrier_setup((int)carrier_khz * 1000);
    const uint16_t *p = pairs;
    while (*p) {
        mark(p[0]);
        if (p[1]) space(p[1]);
        p += 2;
    }
    digitalWrite(IR_TX_PIN, IR_TX_OFF_LEVEL);  /* emitter OFF (board-correct polarity) */
}

#include "ir_extras_data.h"

/* ---- shared prank UI: render a tag + run the prank inline ---- */

static void prank_run_screen(const char *title, const char *blurb,
                             void (*body)(void))
{
    pinMode(IR_TX_PIN, OUTPUT);
    digitalWrite(IR_TX_PIN, IR_TX_OFF_LEVEL);  /* emitter OFF (board-correct polarity) */
    s_carrier_hz = 0;
    carrier_setup(38000);

    auto &d = M5Cardputer.Display;
    ui_clear_body();
    ui_draw_footer("running...  `=skip after");
    d.setTextColor(T_BAD, T_BG);
    d.setCursor(4, BODY_Y + 2); d.print(title);
    d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_BAD);
    d.setTextColor(T_WARN, T_BG);
    d.setCursor(4, BODY_Y + 22); d.print("Point top edge at target.");
    d.setTextColor(T_DIM, T_BG);
    int y = BODY_Y + 36;
    /* word-wrap blurb at ~38 chars/line */
    const char *p = blurb;
    while (*p && y < FOOTER_Y - 14) {
        char line[40]; int n = 0;
        while (*p && n < 38) line[n++] = *p++;
        if (*p && n == 38) {
            /* back up to last space */
            while (n > 0 && line[n - 1] != ' ') { n--; p--; }
            if (n == 0) n = 38;
        }
        line[n] = 0;
        d.setCursor(4, y); d.print(line);
        y += 10;
        while (*p == ' ') p++;
    }
    /* IR blast timing inside body() is carrier critical, so we cannot
     * repaint during it — draw one labeled spinner frame here so the
     * screen does not read as frozen while the prank runs. */
    ui_spinner(SCR_W - 16, BODY_Y + 22, T_BAD);
    delay(120);
    body();
    digitalWrite(IR_TX_PIN, IR_TX_OFF_LEVEL);  /* emitter OFF (board-correct polarity) */

    /* Done — wait for any key. */
    d.setTextColor(T_GOOD, T_BG);
    d.setCursor(4, FOOTER_Y - 12); d.print("DONE — any key to exit");
    while (true) {
        uint16_t k = input_poll();
        if (k != PK_NONE) break;
        delay(20);
    }
}

/* Wrappers — one per menu entry. Each just routes to the inline driver
 * from ir_extras_data.h with a UI shell. */

void feat_ir_prank_power_bomb(void)
{
    prank_run_screen("POWER BOMB",
        "Fires every brand's power code (27 raw codes including projectors "
        "and soundbars). Single press toggles — most displays in line of "
        "sight will blink off within 5 sec.",
        prank_power_bomb);
}

void feat_ir_prank_channel_scramble(void)
{
    prank_run_screen("CHANNEL SCRAMBLE",
        "Hammers Ch+ on Samsung and LG for ~5 sec. ~50 channel changes "
        "before the user can grab the remote.",
        prank_channel_scramble);
}

void feat_ir_prank_volume_bomb(void)
{
    prank_run_screen("VOLUME BOMB",
        "Vol+ x30 on Samsung and LG TVs in under 3 sec. Pairs nicely with "
        "a commercial. Use responsibly.",
        prank_volume_bomb);
}

void feat_ir_prank_source_roulette(void)
{
    prank_run_screen("SOURCE ROULETTE",
        "Cycles inputs 12 times with random delays. TV flashes through "
        "HDMI / USB / AV / ATV / DTV — 'No Signal' chaos.",
        prank_source_roulette);
}

void feat_ir_prank_mute_torture(void)
{
    prank_run_screen("MUTE TORTURE",
        "Toggles mute every 800 ms for 30 sec on Samsung + LG. Audio "
        "stutters until the user power-cycles the TV.",
        prank_mute_torture);
}

static void prank_permanent_5min(void) { prank_permanent_power_toggle(5 * 60 * 1000); }
void feat_ir_prank_permanent_power(void)
{
    prank_run_screen("CABLE OUT (5 MIN)",
        "Full power-bomb every 3 sec for 5 min. Every TV in line of sight "
        "stays off for more than ~3 sec at a time.",
        prank_permanent_5min);
}

static void prank_cable_3cycles(void) { prank_cable_is_out(3); }
void feat_ir_prank_cable_is_out(void)
{
    prank_run_screen("CABLE FLAKES",
        "Three input-loss simulations spaced ~45 sec apart. Looks like the "
        "cable box keeps losing signal.",
        prank_cable_3cycles);
}

void feat_ir_prank_caption_chaos(void)
{
    prank_run_screen("CAPTION CHAOS",
        "Toggles closed-captioning every 600 ms on Samsung + LG. CC text "
        "appears/disappears so fast the user can't read either feed.",
        prank_caption_chaos);
}

void feat_ir_prank_sleep_timer(void)
{
    prank_run_screen("SLEEP TIMER 180",
        "Cycles Samsung sleep-timer 5x to land on 180-min auto-off. TV "
        "powers off 3h later with no apparent trigger.",
        prank_sleep_timer_set);
}

void feat_ir_prank_ac_chaos(void)
{
    prank_run_screen("AC CHAOS",
        "Daikin / Mitsubishi / LG AC power-toggle captures. ~50% hit rate "
        "per brand; usually one lands. Office HVAC roulette.",
        prank_ac_chaos);
}
