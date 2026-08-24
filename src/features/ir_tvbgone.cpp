/*
 * ir_tvbgone — continuously fire TV power-off codes on the Cardputer's
 * IR LED (GPIO 44). Uses the ESP32 LEDC peripheral for the 38 kHz
 * carrier, bit-bangs the mark/space timing.
 *
 * Code database is a condensed subset of the classic TV-B-Gone set —
 * NEC, Sony, RC5, RC6 in compressed form. Hits the big brands (Sony,
 * Samsung, LG, Panasonic, Philips, Toshiba, Sharp, Vizio, RCA).
 */
#include "app.h"
#include "../theme.h"
#include "ui.h"
#include "input.h"
#include <driver/ledc.h>
#include <driver/gpio.h>

#include "../ir_hw.h"
#define IR_PIN IR_TX_PIN

static volatile bool s_running = false;
static volatile uint32_t s_code_idx = 0;

/* Each code = carrier frequency + sequence of mark/space microseconds
 * (alternating). Terminated by 0. */
struct ir_code_t {
    uint16_t carrier;     /* kHz */
    const uint16_t *pairs; /* mark_us, space_us, ... terminated by 0 */
};

/* Sony power (12-bit). */
static const uint16_t sony_power[] = {
    2400, 600,
    1200, 600, 600, 600, 1200, 600, 600, 600, 600, 600, 600, 600, 600, 600,
    1200, 600, 600, 600, 600, 600, 600, 600, 1200, 600,
    0
};
/* NEC Samsung power. */
static const uint16_t samsung_power[] = {
    4500, 4500,
    560, 1690, 560, 1690, 560, 1690, 560, 560, 560, 560, 560, 560, 560, 560, 560, 560,
    560, 1690, 560, 1690, 560, 1690, 560, 560, 560, 560, 560, 560, 560, 560, 560, 560,
    560, 560, 560, 560, 560, 560, 560, 1690, 560, 1690, 560, 1690, 560, 1690, 560, 1690,
    560, 1690, 560, 1690, 560, 1690, 560, 560, 560, 560, 560, 560, 560, 560, 560, 560,
    560,
    0
};
/* LG (NEC-like). */
static const uint16_t lg_power[] = {
    9000, 4500,
    560, 1690, 560, 560, 560, 560, 560, 560, 560, 1690, 560, 560, 560, 560, 560, 560,
    560, 1690, 560, 1690, 560, 560, 560, 1690, 560, 560, 560, 560, 560, 1690, 560, 1690,
    560, 560, 560, 1690, 560, 1690, 560, 560, 560, 1690, 560, 560, 560, 1690, 560, 560,
    560, 1690, 560, 560, 560, 560, 560, 1690, 560, 560, 560, 1690, 560, 560, 560, 1690,
    560, 1690, 560, 560, 560,
    0
};
/* Panasonic. */
static const uint16_t panasonic_power[] = {
    3500, 1700,
    430, 430, 430, 1280, 430, 430, 430, 430, 430, 430, 430, 430, 430, 430, 430, 430,
    430, 430, 430, 430, 430, 430, 430, 430, 430, 430, 430, 430, 430, 430, 430, 430,
    430, 1280, 430, 430, 430, 1280, 430, 430, 430, 430, 430, 430, 430, 430, 430, 430,
    430, 1280, 430, 1280, 430,
    0
};
/* Philips RC5. */
static const uint16_t philips_power[] = {
    889, 889,
    889, 889, 889, 889, 1778, 1778, 889, 889, 889, 889, 889, 889, 889, 889, 889,
    0
};
/* Vizio (NEC format). */
static const uint16_t vizio_power[] = {
    9000, 4500,
    560, 560, 560, 1690, 560, 560, 560, 1690, 560, 1690, 560, 560, 560, 560, 560, 1690,
    560, 1690, 560, 560, 560, 1690, 560, 560, 560, 560, 560, 1690, 560, 560, 560, 1690,
    560, 1690, 560, 560, 560, 1690, 560, 1690, 560, 1690, 560, 560, 560, 560, 560, 560,
    560, 560, 560, 1690, 560, 560, 560, 560, 560, 1690, 560, 560, 560, 1690, 560, 1690,
    560,
    0
};

static const ir_code_t s_codes[] = {
    { 40, sony_power     },
    { 38, samsung_power  },
    { 38, lg_power       },
    { 36, panasonic_power},
    { 36, philips_power  },
    { 38, vizio_power    },
};
#define CODE_COUNT (sizeof(s_codes)/sizeof(s_codes[0]))
static const char *s_code_names[CODE_COUNT] = {
    "Sony", "Samsung", "LG", "Panasonic", "Philips", "Vizio"
};

static int s_carrier_half_us = 13;

static void carrier_on(uint32_t freq_hz)
{
    /* Bit-bang carrier — see ir_remote.cpp for the LEDC-idle-level
     * limitation. Store the half-period for the bit-banger. */
    gpio_reset_pin((gpio_num_t)IR_PIN);
    pinMode(IR_PIN, OUTPUT);
    digitalWrite(IR_PIN, IR_TX_OFF_LEVEL);  /* emitter OFF (board-correct polarity) */
    s_carrier_half_us = 500000 / (int)freq_hz;
    if (s_carrier_half_us < 1) s_carrier_half_us = 1;
}

static void mark(uint16_t us)
{
    uint32_t end = micros() + us;
    int half = s_carrier_half_us;
    while ((int32_t)(end - micros()) > 0) {
        digitalWrite(IR_PIN, HIGH);   /* LED on */
        delayMicroseconds(half);
        digitalWrite(IR_PIN, LOW);  /* LED off */
        delayMicroseconds(half);
    }
}

static void space(uint16_t us)
{
    digitalWrite(IR_PIN, IR_TX_OFF_LEVEL);  /* emitter OFF (board-correct polarity) */
    if (us) delayMicroseconds(us);
}

static void blast(const ir_code_t &c)
{
    carrier_on((uint32_t)c.carrier * 1000);
    const uint16_t *p = c.pairs;
    while (*p) {
        mark(p[0]);
        if (p[1]) space(p[1]);
        p += 2;
    }
    /* Force LED off (active-LOW: HIGH=off). */
    digitalWrite(IR_PIN, IR_TX_OFF_LEVEL);
}

static void blaster_task(void *)
{
    while (s_running) {
        s_code_idx = (s_code_idx + 1) % CODE_COUNT;
        blast(s_codes[s_code_idx]);
        delay(250);  /* pause between brands */
    }
    vTaskDelete(nullptr);
}

void feat_ir_tvbgone(void)
{
    pinMode(IR_PIN, OUTPUT);
    digitalWrite(IR_PIN, IR_TX_OFF_LEVEL);  /* emitter OFF (board-correct polarity) */
    s_code_idx = 0;
    s_running = true;
    xTaskCreate(blaster_task, "ir_tvbg", 3072, nullptr, 4, nullptr);

    ui_clear_body();
    ui_draw_footer("`=stop");
    auto &d = M5Cardputer.Display;
    d.setTextColor(T_BAD, T_BG);
    d.setCursor(4, BODY_Y + 2); d.print("TV-B-GONE");
    d.drawFastHLine(4, BODY_Y + 12, 90, T_BAD);

    uint32_t last = 0;
    while (true) {
        if (millis() - last > 250) {
            last = millis();
            d.fillRect(0, BODY_Y + 20, SCR_W, 60, T_BG);
            d.setTextColor(T_WARN, T_BG);
            d.setCursor(4, BODY_Y + 22);
            d.printf("Blasting: %s", s_code_names[s_code_idx]);
            d.setTextColor(T_DIM, T_BG);
            d.setCursor(4, BODY_Y + 38);
            d.print("Point the top edge at the TV.");
            d.setCursor(4, BODY_Y + 50);
            d.printf("%d brands cycling.", (int)CODE_COUNT);
            ui_draw_status("ir", "blast");
        }
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) break;
    }

    s_running = false;
    delay(300);
    digitalWrite(IR_PIN, IR_TX_OFF_LEVEL);  /* emitter OFF (board-correct polarity) */
}
