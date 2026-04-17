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

#define IR_PIN 44

/* Samsung NEC-style header. */
static const uint16_t SAMSUNG_HEADER[] = { 4500, 4500 };

/* Samsung command bytes (8-bit cmd, repeated inverted). */
struct ir_cmd_t { const char *label; char key; uint8_t cmd; };
static const ir_cmd_t s_cmds[] = {
    { "Power",     'p', 0x40 },
    { "Mute",      'm', 0x0F },
    { "Vol+",      '+', 0x07 },
    { "Vol-",      '-', 0x0B },
    { "Ch+",       ';', 0x12 },
    { "Ch-",       '.', 0x10 },
    { "Source",    'i', 0x01 },
    { "Home",      'h', 0x79 },
    { "Back",      'b', 0x58 },
    { "1",         '1', 0x04 },
    { "2",         '2', 0x05 },
    { "3",         '3', 0x06 },
    { "4",         '4', 0x08 },
    { "5",         '5', 0x09 },
    { "6",         '6', 0x0A },
    { "7",         '7', 0x0C },
    { "8",         '8', 0x0D },
    { "9",         '9', 0x0E },
};
#define CMD_N (sizeof(s_cmds)/sizeof(s_cmds[0]))

static void carrier_on(void)
{
    ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 38000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&t);
    ledc_channel_config_t c = {
        .gpio_num = IR_PIN, .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0, .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0, .duty = 0, .hpoint = 0,
    };
    ledc_channel_config(&c);
}
static inline void mark(uint16_t us)  { ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 128); ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0); delayMicroseconds(us); }
static inline void space(uint16_t us) { ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);   ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0); delayMicroseconds(us); }

static void send_samsung(uint8_t cmd)
{
    /* Header */
    mark(SAMSUNG_HEADER[0]); space(SAMSUNG_HEADER[1]);
    /* Address: 0x07 0x07 (Samsung TV) */
    uint8_t addr = 0x07;
    uint8_t bytes[4] = { addr, (uint8_t)~addr, cmd, (uint8_t)~cmd };
    for (int b = 0; b < 4; ++b) {
        for (int i = 0; i < 8; ++i) {
            mark(560);
            space((bytes[b] & (1 << i)) ? 1690 : 560);
        }
    }
    mark(560); space(0);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

void feat_ir_remote(void)
{
    pinMode(IR_PIN, OUTPUT);
    digitalWrite(IR_PIN, LOW);
    carrier_on();

    ui_clear_body();
    auto &d = M5Cardputer.Display;
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2); d.print("IR REMOTE (Samsung)");
    d.drawFastHLine(4, BODY_Y + 12, 150, T_ACCENT);
    d.setTextColor(T_FG, T_BG);
    d.setCursor(4, BODY_Y + 22); d.print("P=pwr M=mute +/-=vol");
    d.setCursor(4, BODY_Y + 34); d.print(";=ch+ .=ch- 1-9=digit");
    d.setCursor(4, BODY_Y + 46); d.print("I=src H=home B=back");
    ui_draw_footer("press key to send  `=back");

    while (true) {
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) break;
        for (size_t i = 0; i < CMD_N; ++i) {
            if ((char)tolower((int)k) == s_cmds[i].key) {
                send_samsung(s_cmds[i].cmd);
                d.fillRect(0, BODY_Y + 60, SCR_W, 14, T_BG);
                d.setTextColor(T_GOOD, T_BG);
                d.setCursor(4, BODY_Y + 60);
                d.printf("> %s", s_cmds[i].label);
                break;
            }
        }
    }
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}
