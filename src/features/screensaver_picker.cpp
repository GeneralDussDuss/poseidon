/*
 * screensaver_picker.cpp — pick a specific screensaver or shuffle mode.
 *
 * Lists "SHUFFLE" (random rotation, exclude-last) plus every painter in
 * the pool. ENTER commits to NVS, P previews the selected painter (no
 * commit), ESC backs out without saving.
 */
#include "../app.h"
#include "../ui.h"
#include "../input.h"
#include "../theme.h"
#include "../screensaver.h"

void feat_screensaver_picker(void)
{
    auto &d = M5Cardputer.Display;
    int pool_n = screensaver_pool_count();
    int total  = pool_n + 1;          /* +1 for SHUFFLE row at index 0 */
    int saved  = screensaver_pick_get();
    /* sel ranges 0..total-1; row 0 = SHUFFLE, rows 1..N = pool indices 0..N-1 */
    int sel    = (saved == SCREENSAVER_PICK_SHUFFLE) ? 0 : (saved + 1);

    auto row_to_pick = [&](int row) -> int {
        return (row == 0) ? SCREENSAVER_PICK_SHUFFLE : (row - 1);
    };

    int prev_sel = -1;
    while (true) {
        if (sel != prev_sel) { ui_force_clear_body(); prev_sel = sel; }
        ui_draw_status("screensaver", "");

        d.setTextColor(T_ACCENT, T_BG);
        d.setCursor(4, BODY_Y + 2); d.print("SCREENSAVER");
        d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT2);
        d.drawFastHLine(4, BODY_Y + 13, SCR_W - 8, T_ACCENT2);

        const int rows = LIST_ROWS(18, 12);   /* fill the panel: was hardcoded for the 113px Cardputer body */
        const int row_h = 12;
        const int first_y = BODY_Y + 18;
        int first = sel - rows / 2;
        if (first < 0) first = 0;
        if (first + rows > total) first = total - rows;
        if (first < 0) first = 0;

        if (total > rows) {
            char pos[12];
            snprintf(pos, sizeof(pos), "%d/%d", sel + 1, total);
            int pw = d.textWidth(pos);
            d.setTextColor(T_DIM, T_BG);
            d.setCursor(SCR_W - pw - 4, BODY_Y + 2);
            d.print(pos);
        }

        for (int r = 0; r < rows && first + r < total; ++r) {
            int i = first + r;
            int y = first_y + r * row_h;
            bool s = (i == sel);
            bool current = (row_to_pick(i) == saved);

            if (s) {
                d.fillRoundRect(2, y - 1, SCR_W - 4, 11, 2, T_SEL_BG);
                d.drawRoundRect(2, y - 1, SCR_W - 4, 11, 2, T_SEL_BD);
            }
            uint16_t bg = s ? T_SEL_BG : T_BG;
            /* "current" marker — bullet on the left for the saved choice. */
            d.setTextColor(current ? T_ACCENT2 : T_DIM, bg);
            d.setCursor(6, y); d.print(current ? ">" : " ");

            if (i == 0) {
                d.setTextColor(s ? T_ACCENT2 : T_ACCENT, bg);
                d.setCursor(16, y); d.print("SHUFFLE");
                d.setTextColor(T_DIM, bg);
                d.setCursor(80, y); d.print("// random rotation");
            } else {
                int pi = i - 1;
                d.setTextColor(s ? T_FG : T_ACCENT, bg);
                d.setCursor(16, y); d.print(screensaver_pool_name(pi));
                /* Index tag on the right for at-a-glance ordering. */
                char idxs[6];
                snprintf(idxs, sizeof(idxs), "[%d]", pi);
                d.setTextColor(T_DIM, bg);
                int iw = d.textWidth(idxs);
                d.setCursor(SCR_W - iw - 8, y); d.print(idxs);
            }
        }

        /* Up/down arrows when off-screen rows exist. */
        if (first > 0) {
            d.fillTriangle(SCR_W - 7, first_y - 3,
                           SCR_W - 3, first_y - 3,
                           SCR_W - 5, first_y - 6, T_ACCENT2);
        }
        if (first + rows < total) {
            int ay = first_y + rows * row_h - 2;
            d.fillTriangle(SCR_W - 7, ay,
                           SCR_W - 3, ay,
                           SCR_W - 5, ay + 3, T_ACCENT2);
        }

        ui_draw_footer(";/.=move  ENTER=apply  P=preview  `=back");

        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }

        if (k == PK_ESC) return;
        if (k == ';' || k == PK_UP)   sel = (sel - 1 + total) % total;
        else if (k == '.' || k == PK_DOWN) sel = (sel + 1) % total;
        else if (k == PK_ENTER) {
            screensaver_pick_set(row_to_pick(sel));
            saved = screensaver_pick_get();
            ui_toast("screensaver saved", T_GOOD, 600);
            ui_force_clear_body();
        } else if (k == 'p' || k == 'P') {
            int pick = row_to_pick(sel);
            if (pick == SCREENSAVER_PICK_SHUFFLE) {
                ui_toast("pick a saver to preview", T_WARN, 700);
            } else {
                screensaver_run_index(pick);
                /* On return, drain the wake-keypress and force redraw. */
                ui_force_clear_body();
            }
        }
    }
}
