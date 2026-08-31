/*
 * input.cpp — keyboard polling + modal line editor.
 *
 * Model: input_poll() returns one event per key press. Printable chars
 * come through as their ASCII value. Navigation keys come through as
 * the raw punctuation char (;, ., ,, /) — the menu layer translates
 * those into scroll actions. This keeps text entry unambiguous: when
 * input_line() is active, ';' is ';', not UP.
 *
 * Special keys:
 *   ENTER, BKSP, TAB, SPACE                    — always as PK_*
 *   FN+backtick, Ctrl+[, Ctrl+C               — all map to PK_ESC
 *   Arrow-like nav keys                        — returned as their raw
 *                                                char; layer above decides
 */
#include "input.h"
#include "app.h"
#include "theme.h"
#include "sfx.h"
#include "ui.h"
#include "board/encoder_tembed.h"
#include "board/leds_tembed.h"

/* Last-seen debug state — shown by input_debug_draw(). */
static uint16_t s_last_key = PK_NONE;

/* millis() of last real (non-PK_NONE) event. Drives screensaver idle
 * trigger. Updated via the input_poll() wrapper below. */
static uint32_t s_last_input_ms = 0;

uint32_t input_last_input_ms(void) { return s_last_input_ms; }

/* ---- injected key ring buffer (for TRIDENT PC Bridge) ---- */
static uint16_t s_injected[16];
static uint8_t s_inj_head = 0, s_inj_tail = 0;

void input_inject(uint16_t code)
{
    uint8_t next = (s_inj_tail + 1) % 16;
    if (next == s_inj_head) return;
    s_injected[s_inj_tail] = code;
    s_inj_tail = next;
}

uint16_t input_last_key(void) { return s_last_key; }

/* Forward decl of the raw poller, then a thin wrapper that records
 * idle-tracking on every real (non-PK_NONE) event. */
static uint16_t input_poll_raw(void);

uint16_t input_poll(void)
{
    /* Radio self-test suite, requested over serial (see selftest.h). Runs here
     * so it executes on the UI task exactly like a real feature would. */
    {
        extern volatile char g_selftest_req;
        extern void selftest_run(char which);
        static bool running = false;
        if (g_selftest_req && !running) {
            running = true;
            char w = g_selftest_req;
            g_selftest_req = 0;
            selftest_run(w);
            running = false;
        }
    }
#if defined(POSEIDON_BOARD_TEMBED)
    /* Drive the LED ring from here.
     *
     * leds_tick() has to be called continuously or the ring freezes on
     * whatever frame it last rendered -- which is exactly what happened: the
     * boot event fired and then nothing ticked it again, so the whole
     * animation system sat dead despite being fully implemented.
     *
     * input_poll() is the one function EVERY feature's main loop calls, so
     * ticking here animates the ring across the entire firmware without
     * touching 90 feature files. It is internally rate-limited to ~60 fps and
     * never blocks, so the cost is a millis() compare on most calls. */
    leds_tick();
#endif
    uint16_t k = input_poll_raw();
    if (k != PK_NONE) s_last_input_ms = millis();
    return k;
}

static uint16_t input_poll_raw(void)
{
    /* Drain injected keys first (from TRIDENT PC Bridge). */
    if (s_inj_head != s_inj_tail) {
        uint16_t code = s_injected[s_inj_head];
        s_inj_head = (s_inj_head + 1) % 16;
        s_last_key = code;
        return code;
    }
#if defined(POSEIDON_BOARD_TEMBED)
    return tembed_input_poll();
#else
    M5Cardputer.update();
    if (!M5Cardputer.Keyboard.isChange()) return PK_NONE;
    if (!M5Cardputer.Keyboard.isPressed()) return PK_NONE;

    auto status = M5Cardputer.Keyboard.keysState();

    /* Control keys take precedence. */
    if (status.enter) { s_last_key = PK_ENTER; sfx_select(); return PK_ENTER; }
    if (status.del)   { s_last_key = PK_BKSP;  sfx_click();  return PK_BKSP;  }
    if (status.tab)   { s_last_key = PK_TAB;   sfx_click();  return PK_TAB;   }

    if (status.space) { s_last_key = PK_SPACE; sfx_click(); return PK_SPACE; }

    /* Any other printable — return raw. Multiple aliases map to ESC:
     *   backtick alone      (top-left of the keyboard, no modifier)
     *   Ctrl + [ or Ctrl+C  (familiar "cancel") */
    if (!status.word.empty()) {
        char c = status.word[0];
        if (c == '`') { s_last_key = PK_ESC; sfx_back(); return PK_ESC; }
        if (status.ctrl && (c == '[' || c == 'c' || c == 'C')) {
            s_last_key = PK_ESC;
            sfx_back();
            return PK_ESC;
        }
        sfx_click();
        s_last_key = (uint16_t)c;
        return (uint16_t)c;
    }
    return PK_NONE;
#endif
}

/* -------------------- modal line editor -------------------- */

bool input_line(const char *prompt, char *out_buf, size_t out_sz)
{
    if (!out_buf || out_sz == 0) return false;
    out_buf[0] = '\0';
    size_t len = 0;

    auto &d = M5Cardputer.Display;
    int y0 = BODY_Y + 20;
    d.fillRect(0, y0, SCR_W, 60, T_BG);
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, y0);
    d.print(prompt);
    d.drawFastHLine(4, y0 + 30, SCR_W - 8, T_DIM);

    auto redraw = [&]() {
        d.fillRect(4, y0 + 14, SCR_W - 8, 14, T_BG);
        d.setCursor(4, y0 + 14);
        d.setTextColor(T_FG, T_BG);
        d.print(out_buf);
        d.print('_');
    };
    redraw();

#if defined(POSEIDON_BOARD_TEMBED)
    /* ---- encoder character wheel ----
     *
     * The T-Embed has no keyboard, and the loop below only accepts printable
     * ASCII, so on that board input_line() could NEVER return a non-empty
     * string: every feature that asks the operator to type an SSID, password,
     * hostname, frequency or filename was dead. This is the single root cause
     * behind most "feature does nothing on T-Embed" reports.
     *
     * Gestures, matching the rest of the encoder UI:
     *   turn        -> scroll the character wheel
     *   press       -> commit the highlighted character
     *   hold        -> action menu (Backspace / Done / Cancel)
     *   side button -> cancel
     *
     * The charset is ordered for real use: lowercase first (most SSIDs and
     * passwords), then digits, then uppercase, then punctuation. */
    {
        static const char WHEEL[] =
            "abcdefghijklmnopqrstuvwxyz"
            "0123456789"
            "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
            "-_. :/@#!?+*&%$()[]{}<>,;'\"\\|~^`=";
        const int WHEEL_N = (int)(sizeof(WHEEL) - 1);
        int wsel = 0;

        auto draw_wheel = [&]() {
            /* Show a short window around the selection so the operator can see
             * what is coming without scrolling blind. */
            d.fillRect(4, y0 + 32, SCR_W - 8, 16, T_BG);
            d.setCursor(4, y0 + 34);
            for (int off = -4; off <= 4; ++off) {
                const int i = ((wsel + off) % WHEEL_N + WHEEL_N) % WHEEL_N;
                d.setTextColor(off == 0 ? T_ACCENT2 : T_DIM, T_BG);
                if (off == 0) d.print('[');
                d.print(WHEEL[i]);
                if (off == 0) d.print(']');
            }
        };
        draw_wheel();

        while (true) {
            uint16_t k = input_poll();
            if (k == PK_NONE) { delay(10); continue; }

            if (k == PK_ESC) return false;
            if (k == PK_UP)   { wsel = (wsel - 1 + WHEEL_N) % WHEEL_N; draw_wheel(); continue; }
            if (k == PK_DOWN) { wsel = (wsel + 1) % WHEEL_N;           draw_wheel(); continue; }

            if (k == PK_ENTER) {
                if (len + 1 < out_sz) {
                    out_buf[len++] = WHEEL[wsel];
                    out_buf[len]   = '\0';
                    redraw();
                }
                continue;
            }

            if (k == PK_ACTIONS) {
                static const char *const ACTS[] = { "Backspace", "Done", "Cancel" };
                const int pick = ui_action_menu(prompt, ACTS, 3);
                /* The menu painted over us; restore the whole editor. */
                d.fillRect(0, y0, SCR_W, 60, T_BG);
                d.setTextColor(T_ACCENT, T_BG);
                d.setCursor(4, y0); d.print(prompt);
                d.drawFastHLine(4, y0 + 30, SCR_W - 8, T_DIM);
                redraw();
                draw_wheel();
                if (pick == 0) { if (len) { out_buf[--len] = '\0'; redraw(); } }
                else if (pick == 1) { out_buf[len] = '\0'; return true; }
                else if (pick == 2) { return false; }
                continue;
            }
        }
    }
#endif

    while (true) {
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(10); continue; }
        if (k == PK_ESC) return false;
        if (k == PK_ENTER) {
            out_buf[len] = '\0';
            return true;
        }
        if (k == PK_BKSP) {
            if (len > 0) { len--; out_buf[len] = '\0'; redraw(); }
            continue;
        }
        if (k == PK_SPACE && len + 1 < out_sz) {
            out_buf[len++] = ' ';
            out_buf[len]   = '\0';
            redraw();
            continue;
        }
        if (k >= 0x20 && k < 0x7F && len + 1 < out_sz) {
            out_buf[len++] = (char)k;
            out_buf[len]   = '\0';
            redraw();
        }
    }
}
