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
#include "sfx.h"

/* Last-seen debug state — shown by input_debug_draw(). */
static uint16_t s_last_key = PK_NONE;

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

uint16_t input_poll(void)
{
    /* Drain injected keys first (from TRIDENT PC Bridge). */
    if (s_inj_head != s_inj_tail) {
        uint16_t code = s_injected[s_inj_head];
        s_inj_head = (s_inj_head + 1) % 16;
        s_last_key = code;
        return code;
    }
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
}

/* -------------------- modal line editor -------------------- */

bool input_line(const char *prompt, char *out_buf, size_t out_sz)
{
    if (!out_buf || out_sz == 0) return false;
    out_buf[0] = '\0';
    size_t len = 0;

    auto &d = M5Cardputer.Display;
    int y0 = BODY_Y + 20;
    d.fillRect(0, y0, SCR_W, 60, COL_BG);
    d.setTextColor(COL_ACCENT, COL_BG);
    d.setCursor(4, y0);
    d.print(prompt);
    d.drawFastHLine(4, y0 + 30, SCR_W - 8, COL_DIM);

    auto redraw = [&]() {
        d.fillRect(4, y0 + 14, SCR_W - 8, 14, COL_BG);
        d.setCursor(4, y0 + 14);
        d.setTextColor(COL_FG, COL_BG);
        d.print(out_buf);
        d.print('_');
    };
    redraw();

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
