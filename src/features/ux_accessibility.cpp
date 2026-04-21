/*
 * ux_accessibility.cpp — one-screen accessibility panel.
 *
 * Small, focused screen for users who need a more readable UI. Intent
 * is a shortcut, not a grab-bag — full theme browse still lives under
 * Settings → Theme. This screen is for "I just want bigger / clearer
 * text NOW" and biases toward the HI-CONTRAST palette.
 *
 * Current toggles:
 *   H  — jump straight to HI-CONTRAST theme (white on black, saturated
 *        semantic colours, no dim gray hints).
 *   P  — default POSEIDON theme.
 *   T  — open the full theme picker.
 *
 * Why a dedicated screen: "which theme helps me read better" isn't
 * obvious from the name-only list in the theme picker. Here we preview
 * HI-CONTRAST on a sample caption so the improvement is visible before
 * committing.
 */
#include "app.h"
#include "../theme.h"
#include "ui.h"
#include "input.h"
#include "radio.h"

extern void feat_theme_picker(void);

static void draw_screen(void)
{
    auto &d = M5Cardputer.Display;
    ui_clear_body();
    ui_draw_status(radio_name(), "a11y");
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2); d.print("UI / ACCESSIBILITY");
    d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT);

    d.setTextColor(T_FG, T_BG);
    d.setCursor(4, BODY_Y + 18);
    d.print("Can't read the UI clearly?");

    d.setTextColor(T_DIM, T_BG);
    d.setCursor(4, BODY_Y + 30);
    d.print("HI-CONTRAST replaces gray hint");
    d.setCursor(4, BODY_Y + 40);
    d.print("text with full white, swaps dim");
    d.setCursor(4, BODY_Y + 50);
    d.print("backgrounds for pure black.");

    d.setTextColor(T_GOOD, T_BG);
    d.setCursor(4, BODY_Y + 64); d.print("[H] apply HI-CONTRAST");
    d.setTextColor(T_FG, T_BG);
    d.setCursor(4, BODY_Y + 74); d.print("[P] reset to POSEIDON");
    d.setCursor(4, BODY_Y + 84); d.print("[T] open theme picker");
    d.setTextColor(ui_big_text() ? T_GOOD : T_FG, T_BG);
    d.setCursor(4, BODY_Y + 94);
    d.printf("[B] Big toasts: %s", ui_big_text() ? "ON" : "off");

    /* Current theme label — shows what's active now. */
    d.setTextColor(T_WARN, T_BG);
    d.setCursor(4, BODY_Y + 106);
    d.printf("now: %s", theme().name);

    ui_draw_footer("letter=go  `=back");
}

void feat_ux_accessibility(void)
{
    draw_screen();
    while (true) {
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC)  return;
        if (k == 'h' || k == 'H') {
            theme_set(THEME_HICONTRAST);
            draw_screen();
            ui_toast("hi-contrast on", T_GOOD, 700);
        }
        if (k == 'p' || k == 'P') {
            theme_set(THEME_POSEIDON);
            draw_screen();
            ui_toast("default theme", T_GOOD, 700);
        }
        if (k == 't' || k == 'T') {
            feat_theme_picker();
            draw_screen();
        }
        if (k == 'b' || k == 'B') {
            bool on = !ui_big_text();
            ui_big_text_set(on);
            draw_screen();
            ui_toast(on ? "big toasts on" : "toasts normal",
                     T_GOOD, 900);
        }
    }
}
