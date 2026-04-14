/*
 * input.cpp — keyboard polling + modal line editor.
 *
 * M5Cardputer.Keyboard.isChange() fires on both press and release.
 * We gate on isPressed() to only emit on the press edge. The M5 driver
 * does its own debouncing, so a single physical tap maps to one event
 * followed by a release with isChange()==true, isPressed()==false.
 */
#include "input.h"
#include "app.h"

uint16_t input_poll(void)
{
    M5Cardputer.update();
    if (!M5Cardputer.Keyboard.isChange()) return PK_NONE;
    if (!M5Cardputer.Keyboard.isPressed()) return PK_NONE;

    auto status = M5Cardputer.Keyboard.keysState();

    /* Control keys first — they take precedence over printable. */
    if (status.enter) return PK_ENTER;
    if (status.del)   return PK_BKSP;
    if (status.tab)   return PK_TAB;

    /* Cardputer has no arrow keys; FN+;/./,// gives us four directions.
     * FN+` is our ESC (no dedicated ESC key on the 56-key layout). */
    if (status.fn) {
        for (char c : status.word) {
            switch (c) {
            case ';': return PK_UP;
            case '.': return PK_DOWN;
            case ',': return PK_LEFT;
            case '/': return PK_RIGHT;
            case '`': return PK_ESC;
            }
        }
    }

    if (status.space) return PK_SPACE;

    /* First printable char goes through. Ctrl+[ → ESC. */
    if (!status.word.empty()) {
        char c = status.word[0];
        if (status.ctrl && c == '[') return PK_ESC;
        if (status.ctrl && c == 'c') return PK_ESC;  /* Ctrl+C = cancel */
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
        if (k >= 0x20 && k < 0x7F && len + 1 < out_sz) {
            out_buf[len++] = (char)k;
            out_buf[len]   = '\0';
            redraw();
        }
    }
}
