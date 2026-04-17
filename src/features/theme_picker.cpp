/*
 * theme_picker.cpp — visual theme selector with live preview.
 */
#include "../app.h"
#include "../ui.h"
#include "../input.h"
#include "../theme.h"

void feat_theme_picker(void)
{
    auto &d = M5Cardputer.Display;
    int sel = (int)theme_current_id();

    int prev_sel = -1;
    while (true) {
        theme_set((theme_id_t)sel);
        if (sel != prev_sel) { ui_force_clear_body(); prev_sel = sel; }
        ui_draw_status("theme", "");

        d.setTextColor(T_ACCENT, T_BG);
        d.setCursor(4, BODY_Y + 2); d.print("THEME");
        d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT);

        for (int i = 0; i < THEME__COUNT; i++) {
            int y = BODY_Y + 18 + i * 14;
            theme_set((theme_id_t)i);
            bool s = (i == sel);
            if (s) {
                d.fillRoundRect(2, y - 2, SCR_W - 4, 13, 2, T_SEL_BG);
                d.drawRoundRect(2, y - 2, SCR_W - 4, 13, 2, T_SEL_BD);
            }
            uint16_t bg = s ? T_SEL_BG : theme().bg;
            d.setTextColor(T_ACCENT, bg);
            d.setCursor(8, y);
            d.printf("%s", theme().name);
            /* Color swatches. */
            d.fillRect(140, y, 12, 8, T_ACCENT);
            d.fillRect(154, y, 12, 8, theme().accent2);
            d.fillRect(168, y, 12, 8, T_GOOD);
            d.fillRect(182, y, 12, 8, T_BAD);
            d.fillRect(196, y, 12, 8, T_DIM);
        }
        theme_set((theme_id_t)sel);

        ui_draw_footer(";/.=browse  ENTER=apply  ESC=back");

        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) return;
        if (k == ';' || k == PK_UP)   sel = (sel - 1 + THEME__COUNT) % THEME__COUNT;
        if (k == '.' || k == PK_DOWN) sel = (sel + 1) % THEME__COUNT;
        if (k == PK_ENTER) {
            theme_set((theme_id_t)sel);
            d.fillScreen(T_BG);
            ui_toast("theme applied", T_GOOD, 600);
            return;
        }
    }
}
