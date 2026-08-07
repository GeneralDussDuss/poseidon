#if defined(POSEIDON_BOARD_TEMBED)

#include "encoder_tembed.h"

#include <Arduino.h>
#include <driver/gpio.h>

#include "board_tembed.h"

/* Raw codes mirrored from src/input.h. */
enum : uint16_t {
    RAW_NONE  = 0,
    RAW_ENTER = 0x0D,
    RAW_ESC   = 0x1B,
    RAW_UP    = 0x100,
    RAW_DOWN  = 0x101,
};

/* Quarter-steps per physical detent. Most encoders emit 4 quadrature
 * transitions per detent; some cheaper ones emit only 2. One-line change
 * if POSEIDON_ENC_DEBUG ever shows delta topping out at +/-2 and resetting. */
static const int8_t ENC_STEPS_PER_DETENT = 4;

static volatile int8_t  s_delta   = 0;
static volatile uint8_t s_prev    = 0;
static uint32_t         s_btn_ms  = 0;

/* Standard quadrature transition table: index = (prev << 2) | curr. */
static const int8_t QTAB[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};

/* digitalRead() is not guaranteed IRAM-safe on ESP32-S3 (Arduino core can
 * fall through to a non-IRAM pin-info lookup) — that risks a crash when
 * this ISR fires from flash-cache-disabled contexts (e.g. mid SPI flash
 * write). gpio_get_level() is the IDF driver call and is IRAM-safe. */
static void IRAM_ATTR enc_isr(void) {
    const uint8_t curr = (uint8_t)((gpio_get_level((gpio_num_t)TE_ENC_A) << 1) |
                                     gpio_get_level((gpio_num_t)TE_ENC_B));
    s_delta += QTAB[(s_prev << 2) | curr];
    s_prev = curr;
}

void tembed_input_begin(void) {
    pinMode(TE_ENC_A, INPUT_PULLUP);
    pinMode(TE_ENC_B, INPUT_PULLUP);
    pinMode(TE_BTN_SELECT, INPUT_PULLUP);
    pinMode(TE_BTN_BACK, INPUT_PULLUP);
    s_prev = (uint8_t)((digitalRead(TE_ENC_A) << 1) | digitalRead(TE_ENC_B));
    attachInterrupt(digitalPinToInterrupt(TE_ENC_A), enc_isr, CHANGE);
    attachInterrupt(digitalPinToInterrupt(TE_ENC_B), enc_isr, CHANGE);
}

uint16_t tembed_input_poll(void) {
#if defined(POSEIDON_ENC_DEBUG)
    {
        static uint32_t s_dbg_ms = 0;
        const uint32_t now_dbg = millis();
        if (now_dbg - s_dbg_ms > 250) {
            s_dbg_ms = now_dbg;
            noInterrupts();
            int8_t d = s_delta;
            interrupts();
            Serial.printf("ENC A=%d B=%d delta=%d sel=%d back=%d\n",
                          digitalRead(TE_ENC_A), digitalRead(TE_ENC_B), d,
                          digitalRead(TE_BTN_SELECT), digitalRead(TE_BTN_BACK));
        }
    }
#endif

    /* Buttons first, debounced at 200 ms. Active LOW. */
    const uint32_t now = millis();
    if (now - s_btn_ms > 200) {
        if (digitalRead(TE_BTN_SELECT) == LOW) { s_btn_ms = now; return RAW_ENTER; }
        if (digitalRead(TE_BTN_BACK)   == LOW) { s_btn_ms = now; return RAW_ESC; }
    }

    noInterrupts();
    int8_t d = s_delta;
    if (d >= ENC_STEPS_PER_DETENT)       { s_delta -= ENC_STEPS_PER_DETENT; }
    else if (d <= -ENC_STEPS_PER_DETENT) { s_delta += ENC_STEPS_PER_DETENT; }
    else                                  { d = 0; }
    interrupts();

    if (d >= ENC_STEPS_PER_DETENT)  { return RAW_DOWN; }
    if (d <= -ENC_STEPS_PER_DETENT) { return RAW_UP; }
    return RAW_NONE;
}

#endif /* POSEIDON_BOARD_TEMBED */
