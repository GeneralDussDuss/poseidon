/*
 * ir_remote — virtual Samsung TV remote mapped to Cardputer keys.
 *
 * Key mapping:
 *   P   power       M   mute         +/-  volume up/down
 *   ; . channel up/down     1-9 digit
 *   I   input/source    H   home      B    back
 */
#include "app.h"
#include "../theme.h"
#include "ui.h"
#include "input.h"
#include <driver/ledc.h>
#include <driver/gpio.h>

#include "../ir_hw.h"
#define IR_PIN IR_TX_PIN

/* Samsung NEC-style header. */
static const uint16_t SAMSUNG_HEADER[] = { 4500, 4500 };

/* Full Samsung Smart Remote command set. Single-key bindings only — no
 * shift / ctrl / fn needed. Sent as Samsung32 with address 0x07 unless
 * the entry overrides via the `addr` field. */
struct ir_cmd_t {
    const char *label;
    uint16_t    key;   /* uint16 to fit arrow / ENTER keycodes */
    uint8_t     cmd;
    uint8_t     addr;  /* 0 = use default 0x07 */
};
static const ir_cmd_t s_cmds[] = {
    /* Primary controls — verified against Flipper-IRDB Samsung BN59 remotes. */
    { "Power",       'p',      0x02, 0 },  /* Acts as toggle on consumer TVs;
                                            * 0x02/0x98 are discretes on
                                            * commercial firmware. Was 0x40 (wrong). */
    { "Power On",    'o',      0x02, 0 },  /* Discrete Power On (commercial) */
    { "Power Off",   'q',      0x98, 0 },  /* Discrete Power Off */
    { "Source",      'i',      0x01, 0 },
    { "Home/Hub",    'h',      0x79, 0 },  /* Same code as "Smart Hub" on BN59 */
    { "Back",        'b',      0x58, 0 },  /* Return; closes one menu level */
    { "Exit",        'a',      0x2D, 0 },  /* Closes all overlays (was Smart Hub slot) */
    { "Menu",        's',      0x1A, 0 },
    { "Tools",       't',      0x4B, 0 },
    { "Info",        'f',      0x1F, 0 },
    { "Guide",       'g',      0x4F, 0 },

    /* Volume / Channel — letter bindings (no shift needed) */
    { "Mute",        'm',      0x0F, 0 },
    { "Vol Up",      'u',      0x07, 0 },
    { "Vol Dn",      'd',      0x0B, 0 },
    { "Ch Up",       'c',      0x12, 0 },
    { "Ch Dn",       'v',      0x10, 0 },
    /* Legacy `+`/`-` aliases for vol — `;/./,//` are now D-pad. */
    { "Vol+",        '+',      0x07, 0 },
    { "Vol-",        '-',      0x0B, 0 },

    /* Directional D-pad — Cardputer's arrow cluster emits ; . , / as
     * plain punctuation, NOT PK_UP/DOWN/LEFT/RIGHT (no driver produces
     * those codes on this hardware). Bind directly to the punctuation. */
    { "Up",          ';',      0x60, 0 },
    { "Down",        '.',      0x61, 0 },
    { "Left",        ',',      0x65, 0 },
    { "Right",       '/',      0x62, 0 },
    { "OK/Enter",    PK_ENTER, 0x68, 0 },
    { "OK/Enter",    'e',      0x68, 0 },

    /* Color buttons (A=red, B=green, C=yellow, D=blue) */
    { "Red",         'r',      0x6C, 0 },
    { "Green",       'n',      0x14, 0 },  /* 'n' for greeN */
    { "Yellow",      'y',      0x15, 0 },
    { "Blue",        'l',      0x16, 0 },

    /* Media transport */
    { "Play",        'k',      0x47, 0 },
    { "Pause",       'x',      0x4A, 0 },
    { "Stop",        'z',      0x46, 0 },
    { "Rew",         'w',      0x45, 0 },
    { "FF",          'j',      0x48, 0 },  /* `,` is now Left-arrow */

    /* Number pad */
    { "0",           '0',      0x11, 0 },
    { "1",           '1',      0x04, 0 },
    { "2",           '2',      0x05, 0 },
    { "3",           '3',      0x06, 0 },
    { "4",           '4',      0x08, 0 },
    { "5",           '5',      0x09, 0 },
    { "6",           '6',      0x0A, 0 },
    { "7",           '7',      0x0C, 0 },
    { "8",           '8',      0x0D, 0 },
    { "9",           '9',      0x0E, 0 },
};
#define CMD_N (sizeof(s_cmds)/sizeof(s_cmds[0]))

/* No LEDC — bit-bang the 38 kHz carrier directly. LEDC's duty=0 idle
 * state sits at the configured idle level (LOW by default) regardless
 * of output_invert, so an active-LOW LED ends up solidly ON during the
 * spaces between marks. Bit-banging makes polarity explicit. */
static void carrier_on(void)
{
    /* Hard-stop any LEDC channels that may have been configured on
     * pin 44 by a prior IR feature (ir_tvbgone uses LEDC, the in-file
     * LED Test diagnostic uses LEDC channel 2). If a leftover LEDC
     * channel is still driving with duty>0 and inverted output, the
     * pin sits LOW = LED stuck ON the moment we enter Samsung Remote.
     * Stopping ALL three potentially-used channels covers every path. */
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 1);  /* idle HIGH */
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2, 1);
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_3, 1);  /* tvbgone */
    gpio_reset_pin((gpio_num_t)IR_PIN);
    pinMode(IR_PIN, OUTPUT);
    digitalWrite(IR_PIN, IR_TX_OFF_LEVEL);  /* emitter OFF (board-correct polarity) */
}

/* Half-period = 13 us → period = 26 us → ~38.5 kHz, within IR-receiver
 * tolerance (±5%). Active-LOW: pin LOW = LED on. */
static inline void mark(uint16_t us)
{
    uint32_t end = micros() + us;
    while ((int32_t)(end - micros()) > 0) {
        digitalWrite(IR_PIN, HIGH);   /* LED on */
        delayMicroseconds(13);
        digitalWrite(IR_PIN, LOW);  /* LED off */
        delayMicroseconds(13);
    }
}
static inline void space(uint16_t us)
{
    digitalWrite(IR_PIN, IR_TX_OFF_LEVEL);  /* emitter OFF (board-correct polarity) */
    if (us) delayMicroseconds(us);
}

static void send_samsung(uint8_t cmd, uint8_t addr_override)
{
    /* SAMSUNG32 protocol:
     *   - Header: 4500us mark + 4500us space (NEC is 9000/4500)
     *   - Default address: 0x07 (most Samsung TVs); some E-series use 0xBF.
     *   - Bytes sent: addr, addr, cmd, ~cmd  (address is NOT inverted)
     *   - Bit timing: 560us mark, 560/1690us space (same as NEC)
     */
    mark(SAMSUNG_HEADER[0]); space(SAMSUNG_HEADER[1]);
    uint8_t addr = addr_override ? addr_override : 0x07;
    uint8_t bytes[4] = { addr, addr, cmd, (uint8_t)~cmd };
    for (int b = 0; b < 4; ++b) {
        for (int i = 0; i < 8; ++i) {
            mark(560);
            space((bytes[b] & (1 << i)) ? 1690 : 560);
        }
    }
    mark(560);
    digitalWrite(IR_PIN, IR_TX_OFF_LEVEL);  /* emitter OFF (board-correct polarity) */
}

void feat_ir_remote(void)
{
    carrier_on();

    ui_clear_body();
    auto &d = M5Cardputer.Display;
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2); d.print("SAMSUNG SMART REMOTE");
    d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT);
    d.setTextColor(T_FG, T_BG);
    /* Compact letter-only legend — no modifier keys needed. */
    d.setCursor(4, BODY_Y + 16); d.print("p=pwr o=on q=off m=mute");
    d.setCursor(4, BODY_Y + 26); d.print("u/d=vol  c/v=ch  i=src");
    d.setCursor(4, BODY_Y + 36); d.print(";.,/=Dpad e=OK h=hub b=back a=exit");
    d.setCursor(4, BODY_Y + 46); d.print("s=menu g=guide t=tools f=info");
    d.setCursor(4, BODY_Y + 56); d.print("k=play x=pause z=stop w/j=rew/ff");
    d.setCursor(4, BODY_Y + 66); d.print("r=red n=grn y=yel l=blu  0-9=num");
    ui_draw_footer("ESC=back");

    while (true) {
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) break;
        /* Letter keys: case-insensitive. Special keys (arrows, ENTER)
         * carry their full 16-bit code through unchanged. */
        /* Encoder path: every command except OK/Enter is bound to a printable
         * character, so the T-Embed could fire exactly ONE of ~44 codes. Build
         * the picker straight from s_cmds so it can never drift from the table,
         * then rewrite k to the chosen command's key. */
        if (k == PK_ACTIONS) {
            const char *labels[CMD_N];
            for (size_t i = 0; i < CMD_N; ++i) labels[i] = s_cmds[i].label;
            const int pick = ui_action_menu("IR REMOTE", labels, (int)CMD_N);
            if (pick < 0) { continue; }
            k = s_cmds[pick].key;
        }

        uint16_t match_k = (k >= 0x20 && k < 0x7F) ? (uint16_t)tolower((int)k) : k;
        const ir_cmd_t *matched = nullptr;
        for (size_t i = 0; i < CMD_N; ++i) {
            if (match_k == s_cmds[i].key) { matched = &s_cmds[i]; break; }
        }
        d.fillRect(0, BODY_Y + 78, SCR_W, 14, T_BG);
        if (matched) {
            send_samsung(matched->cmd, matched->addr);
            d.setTextColor(T_GOOD, T_BG);
            d.setCursor(4, BODY_Y + 78);
            d.printf("> %s  (k=0x%X)", matched->label, (unsigned)k);
        } else {
            /* Unmapped — show raw keycode so unknown D-pad / extras can
             * be identified and added to s_cmds. */
            d.setTextColor(T_WARN, T_BG);
            d.setCursor(4, BODY_Y + 78);
            d.printf("? raw key = 0x%X (%d)", (unsigned)k, (int)k);
        }
    }
    digitalWrite(IR_PIN, IR_TX_OFF_LEVEL);  /* park OFF on exit (active-LOW: HIGH=off) */
}

/* ===== IR LED hardware diagnostic =====================================
 * If no IR device responds to any remote feature, the question is:
 * is the LED even lighting up? Phone cameras can see IR — point one at
 * the LED and you'll see purple/white light when it's being driven.
 * This walks through pin / polarity candidates so we know which combo
 * actually drives the LED on this board. */
static void diag_drive(int pin, int freq_hz, bool invert, int dur_ms)
{
    gpio_reset_pin((gpio_num_t)pin);
    ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_2,
        .freq_hz = (uint32_t)freq_hz,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&t);
    ledc_channel_config_t c = {
        .gpio_num = pin, .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_2, .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_2, .duty = 128, .hpoint = 0,
        .flags = { .output_invert = (unsigned)(invert ? 1 : 0) },
    };
    ledc_channel_config(&c);
    /* The LEDC carrier is generated in hardware and keeps running while
     * the CPU idles here, so repainting a spinner does not disturb it. */
    uint32_t end = millis() + (uint32_t)dur_ms;
    while ((int32_t)(end - millis()) > 0) {
        ui_spinner(SCR_W - 16, BODY_Y + 70, T_ACCENT);
        delay(50);
    }
    /* Stop the LEDC channel cleanly, with idle level HIGH (= LED OFF
     * on active-low). Earlier this fn did ledc_set_duty(0) + a manual
     * digitalWrite(LOW), which on an active-LOW LED leaves it stuck
     * ON. That stale-on state then contaminated Samsung Remote on
     * entry — fixed by stopping at idle HIGH and parking the pin HIGH. */
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2, 1);
    gpio_reset_pin((gpio_num_t)pin);
    pinMode(pin, OUTPUT);
    digitalWrite(pin, HIGH);  /* active-LOW: HIGH = LED OFF */
}

struct diag_step_t { int pin; bool invert; const char *label; };
static const diag_step_t DIAG_STEPS[] = {
    { 44, false, "GPIO 44  normal" },
    { 44, true,  "GPIO 44  inverted" },
    {  9, false, "GPIO  9  normal" },
    { 41, false, "GPIO 41  normal" },
};
#define DIAG_N (sizeof(DIAG_STEPS) / sizeof(DIAG_STEPS[0]))

void feat_ir_test(void)
{
    ui_clear_body();
    ui_draw_footer("ESC=stop");
    auto &d = M5Cardputer.Display;
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2); d.print("IR LED HARDWARE TEST");
    d.drawFastHLine(4, BODY_Y + 12, 180, T_ACCENT);
    d.setTextColor(T_FG, T_BG);
    d.setCursor(4, BODY_Y + 18); d.print("Point a phone camera at the");
    d.setCursor(4, BODY_Y + 28); d.print("IR LED. Note which step lights");
    d.setCursor(4, BODY_Y + 38); d.print("it up purple/white.");

    for (size_t i = 0; i < DIAG_N; ++i) {
        d.fillRect(0, BODY_Y + 54, SCR_W, 36, T_BG);
        d.setTextColor(T_WARN, T_BG);
        d.setCursor(4, BODY_Y + 56); d.printf("step %d/%d", (int)i + 1, (int)DIAG_N);
        d.setTextColor(T_GOOD, T_BG);
        d.setCursor(4, BODY_Y + 68); d.print(DIAG_STEPS[i].label);
        d.setTextColor(T_DIM, T_BG);
        d.setCursor(4, BODY_Y + 80); d.print("carrier 38 kHz, 50% duty");

        diag_drive(DIAG_STEPS[i].pin, 38000, DIAG_STEPS[i].invert, 4000);

        if (input_poll() == PK_ESC) break;
    }
    d.fillRect(0, BODY_Y + 54, SCR_W, 36, T_BG);
    d.setTextColor(T_FG, T_BG);
    d.setCursor(4, BODY_Y + 60); d.print("done — press any key");
    while (input_poll() == PK_NONE) delay(20);
}
