/*
 * stubs.cpp — only the About screen now. Everything else is implemented.
 */
#include "app.h"
#include "ui.h"
#include "input.h"

void feat_about(void)
{
    ui_clear_body();
    auto &d = M5Cardputer.Display;
    d.setTextColor(COL_ACCENT, COL_BG);
    d.setCursor(4, BODY_Y + 2);
    d.print("POSEIDON");
    d.drawFastHLine(4, BODY_Y + 12, 90, COL_ACCENT);
    d.setTextColor(COL_FG, COL_BG);
    d.setCursor(4, BODY_Y + 22); d.printf("v%s", POSEIDON_VERSION);
    d.setCursor(4, BODY_Y + 34); d.print("keyboard-first pentesting");
    d.setCursor(4, BODY_Y + 46); d.print("M5Stack Cardputer");
    d.setTextColor(COL_DIM, COL_BG);
    d.setCursor(4, BODY_Y + 64); d.print("github.com/../poseidon");
    d.setCursor(4, BODY_Y + 76); d.print("commander of the deep");
    ui_draw_footer("`=back");
    while (true) {
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) break;
    }
}
