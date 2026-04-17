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

#define IR_PIN 44  /* Cardputer IR LED */

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

static void carrier_on(uint32_t freq_hz)
{
    ledc_timer_config_t t = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num       = LEDC_TIMER_0,
        .freq_hz         = freq_hz,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&t);
    ledc_channel_config_t c = {
        .gpio_num   = IR_PIN,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_0,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = LEDC_TIMER_0,
        .duty       = 128,  /* 50% for IR */
        .hpoint     = 0,
    };
    ledc_channel_config(&c);
}

static void mark(uint16_t us)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 128);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    delayMicroseconds(us);
}

static void space(uint16_t us)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    delayMicroseconds(us);
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
    space(0);
    /* Force LED off. */
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
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
    digitalWrite(IR_PIN, LOW);
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
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}
