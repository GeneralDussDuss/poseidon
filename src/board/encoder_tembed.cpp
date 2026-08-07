#if defined(POSEIDON_BOARD_TEMBED)

#include "encoder_tembed.h"

#include <Arduino.h>

#include "board_tembed.h"

/* Raw codes mirrored from src/input.h. */
enum : uint16_t {
    RAW_NONE  = 0,
    RAW_ENTER = 0x0D,
    RAW_ESC   = 0x1B,
    RAW_UP    = 0x100,
    RAW_DOWN  = 0x101,
};

static volatile int8_t  s_delta   = 0;
static volatile uint8_t s_prev    = 0;
static uint32_t         s_btn_ms  = 0;

/* Standard quadrature transition table: index = (prev << 2) | curr. */
static const int8_t QTAB[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};

static void IRAM_ATTR enc_isr(void) {
    const uint8_t curr = (uint8_t)((digitalRead(TE_ENC_A) << 1) | digitalRead(TE_ENC_B));
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
    /* Buttons first, debounced at 200 ms. Active LOW. */
    const uint32_t now = millis();
    if (now - s_btn_ms > 200) {
        if (digitalRead(TE_BTN_SELECT) == LOW) { s_btn_ms = now; return RAW_ENTER; }
        if (digitalRead(TE_BTN_BACK)   == LOW) { s_btn_ms = now; return RAW_ESC; }
    }

    /* Four quarter-steps per physical detent. */
    noInterrupts();
    int8_t d = s_delta;
    if (d >= 4)       { s_delta -= 4; }
    else if (d <= -4) { s_delta += 4; }
    else              { d = 0; }
    interrupts();

    if (d >= 4)  { return RAW_DOWN; }
    if (d <= -4) { return RAW_UP; }
    return RAW_NONE;
}

#endif /* POSEIDON_BOARD_TEMBED */
