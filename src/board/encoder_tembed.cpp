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
    RAW_ACTIONS = 0x105,   /* mirrors PK_ACTIONS in src/input.h */
};

/* Quarter-steps per physical detent. Most encoders emit 4 quadrature
 * transitions per detent; some cheaper ones emit only 2. One-line change
 * if POSEIDON_ENC_DEBUG ever shows delta topping out at +/-2 and resetting. */
static const int8_t ENC_STEPS_PER_DETENT = 4;

static volatile int8_t  s_delta   = 0;
static volatile uint8_t s_prev    = 0;
static uint32_t         s_btn_ms  = 0;

/* Gesture timings. DOUBLE is the click-pairing window and is also the
 * latency a single press pays before it registers as SELECT, so keep it
 * short enough to feel instant. HOLD must comfortably exceed DOUBLE or a
 * slow double-click would trip the hold. */
#define BTN_DEBOUNCE_MS  180
#define BTN_DOUBLE_MS    280
#define BTN_HOLD_MS      550

static bool     s_sel_was_down   = false;
static uint32_t s_sel_press_ms   = 0;
static bool     s_hold_fired     = false;
static bool     s_pending_click  = false;
static uint32_t s_last_click_ms  = 0;

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

    /* ---- encoder-button gesture layer ----
     *
     * One button has to carry three meanings on this board:
     *   single press        -> SELECT   (RAW_ENTER)
     *   double press        -> BACK     (RAW_ESC)
     *   press and hold      -> ACTIONS  (RAW_ACTIONS, secondary menu)
     *
     * Because a single press cannot be distinguished from the first half
     * of a double press until the double-click window expires, SELECT is
     * emitted on release plus a short wait, not on the initial edge. Hold
     * fires while the button is still down, so it feels immediate, and
     * sets a flag so the subsequent release is swallowed rather than also
     * counting as a press.
     *
     * The dedicated side button stays a plain BACK, so there is always a
     * one-action escape for anyone who does not want to double-click. */
    const uint32_t now = millis();

    const bool sel_down  = (digitalRead(TE_BTN_SELECT) == LOW);
    const bool back_down = (digitalRead(TE_BTN_BACK)   == LOW);

    /* Side button: unchanged, simple debounced BACK. */
    if (back_down && (now - s_btn_ms > BTN_DEBOUNCE_MS)) {
        s_btn_ms = now;
        return RAW_ESC;
    }

    if (sel_down && !s_sel_was_down) {
        /* Fresh press edge. */
        s_sel_was_down  = true;
        s_sel_press_ms  = now;
        s_hold_fired    = false;
    } else if (sel_down && s_sel_was_down) {
        /* Still held: fire ACTIONS once we cross the hold threshold. */
        if (!s_hold_fired && (now - s_sel_press_ms) >= BTN_HOLD_MS) {
            s_hold_fired   = true;
            s_pending_click = false;   /* a hold is not a click */
            return RAW_ACTIONS;
        }
    } else if (!sel_down && s_sel_was_down) {
        /* Release edge. */
        s_sel_was_down = false;
        if (s_hold_fired) {
            /* Hold already delivered; swallow this release. */
        } else if (s_pending_click &&
                   (now - s_last_click_ms) <= BTN_DOUBLE_MS) {
            /* Second click inside the window -> BACK. */
            s_pending_click = false;
            return RAW_ESC;
        } else {
            /* First click: arm the double-click window. */
            s_pending_click  = true;
            s_last_click_ms  = now;
        }
    }

    /* Double-click window expired with only one click -> SELECT. */
    if (s_pending_click && (now - s_last_click_ms) > BTN_DOUBLE_MS) {
        s_pending_click = false;
        return RAW_ENTER;
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
