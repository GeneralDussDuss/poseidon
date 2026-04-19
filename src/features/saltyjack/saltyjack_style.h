/*
 * SaltyJack — shared UI style.
 *
 * Full RaspyJack aesthetic, minus color and plus wave motif. The moves:
 *   1. Thick phosphor-color border (3px) around the entire body.
 *   2. Title text "cuts through" the top border — white/cyan on black,
 *      positioned on the border line, not below it. This is RaspyJack's
 *      signature top-frame look.
 *   3. Pitch-black interior. Seafoam text. Cyan accents.
 *   4. Selected row / active counter: solid deep-teal rect with cyan text.
 *   5. Status markers on every status line:  [+] ok  [!] warn  [-] bad  [*] info
 *   6. Labeled info-block helper for capture data (user/domain/IP readouts).
 *   7. Wave watermark `≋≋` bottom-right, and `≋ ` prefix on the footer.
 *
 * Colors picked to match RaspyJack's semantic roles exactly, just shifted
 * from pure-green phosphor to seafoam/cyan.
 */
#pragma once

#include <Arduino.h>
#include <M5Cardputer.h>

/* ===== palette ===== */
/* RGB565. */
#define SJ_BG           0x0000   /* pitch black interior */
#define SJ_FG           0x07FB   /* seafoam — primary text */
#define SJ_FG_DIM       0x0471   /* muted seafoam for secondary/dim text */
#define SJ_ACCENT       0x07FF   /* bright cyan — titles, active values */
#define SJ_ACCENT_DIM   0x0475   /* deep ocean teal — dividers */
#define SJ_FRAME        0x07FB   /* iconic frame color — matches FG */
#define SJ_SEL_BG       0x0313   /* deep teal — selected-row rect */
#define SJ_SEL_FG       0x07FF   /* bright cyan — text on selected rect */
#define SJ_WARN         0xFDE0   /* amber — warnings */
#define SJ_BAD          0xFB4A   /* coral — errors */
#define SJ_GOOD         0x07F0   /* pure green — success */
#define SJ_INFO         0x7FFF   /* white-ish — neutral info */

/* ===== frame geometry ===== */
#define SJ_FRAME_X       1
#define SJ_FRAME_Y       (BODY_Y + 1)
#define SJ_FRAME_W       (SCR_W - 2)
#define SJ_FRAME_H       (BODY_H - 2)
#define SJ_FRAME_TH      3                                 /* border thickness */
#define SJ_CONTENT_X     (SJ_FRAME_X + SJ_FRAME_TH + 2)
#define SJ_CONTENT_Y     (SJ_FRAME_Y + SJ_FRAME_TH + 4)    /* below title */

/* ===== frame + title ===== */

/* Draws the phosphor border (3px thick) around the body, title text
 * "cutting through" the top border, and the ≋≋ watermark bottom-right.
 * Call this as the first thing on every SaltyJack page. */
static inline void sj_frame(const char *title)
{
    auto &d = M5Cardputer.Display;

    /* Wipe the body. */
    d.fillRect(0, BODY_Y, SCR_W, BODY_H, SJ_BG);

    /* 3px thick border. Four concentric rectangles is cheaper than four
     * fillRects and looks identical. */
    for (int i = 0; i < SJ_FRAME_TH; ++i) {
        d.drawRect(SJ_FRAME_X + i, SJ_FRAME_Y + i,
                   SJ_FRAME_W - 2 * i, SJ_FRAME_H - 2 * i, SJ_FRAME);
    }

    /* Title cutting through the top border — paint a black-bg band where
     * the title sits, then draw the title in cyan on black. This is the
     * "RaspyJack" signature top look. */
    int title_x = SJ_FRAME_X + 6;
    int title_y = SJ_FRAME_Y - 1;   /* sits over the top border */
    int title_w = (int)strlen(title) * 6 + 6;     /* default 6x8 glyphs, pad */
    d.fillRect(title_x - 2, title_y, title_w, 9, SJ_BG);
    d.setTextColor(SJ_ACCENT, SJ_BG);
    d.setCursor(title_x, title_y);
    d.print((const char *)"[");
    d.setTextColor(SJ_SEL_FG, SJ_BG);
    d.print(title);
    d.setTextColor(SJ_ACCENT, SJ_BG);
    d.print((const char *)"]");

    /* Wave watermark bottom-right, just inside the frame. */
    d.setTextColor(SJ_FG_DIM, SJ_BG);
    d.setCursor(SJ_FRAME_X + SJ_FRAME_W - 14, SJ_FRAME_Y + SJ_FRAME_H - 10);
    d.print((const char *)"\xE2\x89\x8B\xE2\x89\x8B");  /* ≋≋ */
}

/* Legacy alias so existing callers still compile during the retrofit. */
static inline void sj_clear(void)
{
    M5Cardputer.Display.fillRect(0, BODY_Y, SCR_W, BODY_H, SJ_BG);
}

/* ===== dividers + rows ===== */

/* Deep-ocean horizontal divider line at y, inside the frame. */
static inline void sj_divider(int y)
{
    auto &d = M5Cardputer.Display;
    d.drawFastHLine(SJ_CONTENT_X, y, SJ_FRAME_W - 10, SJ_ACCENT_DIM);
}

/* Counter row: "label : value" — label in fg, value in accent. */
static inline void sj_row(int y, const char *label, uint32_t value)
{
    auto &d = M5Cardputer.Display;
    d.setTextColor(SJ_FG, SJ_BG);
    d.setCursor(SJ_CONTENT_X, y);
    d.print(label);
    d.setTextColor(SJ_ACCENT, SJ_BG);
    d.printf(": %lu", (unsigned long)value);
}

/* Counter row with a colored value (for ACK=green, NAK=red, etc). */
static inline void sj_row_colored(int y, const char *label, uint32_t value, uint16_t val_color)
{
    auto &d = M5Cardputer.Display;
    d.setTextColor(SJ_FG, SJ_BG);
    d.setCursor(SJ_CONTENT_X, y);
    d.print(label);
    d.setTextColor(val_color, SJ_BG);
    d.printf(": %lu", (unsigned long)value);
}

/* "Active" counter row drawn with a solid highlight rect behind it. */
static inline void sj_row_highlight(int y, const char *label, uint32_t value)
{
    auto &d = M5Cardputer.Display;
    d.fillRect(SJ_CONTENT_X - 2, y - 1, SJ_FRAME_W - 10, 10, SJ_SEL_BG);
    d.setTextColor(SJ_SEL_FG, SJ_SEL_BG);
    d.setCursor(SJ_CONTENT_X, y);
    d.print(label);
    d.printf(": %lu", (unsigned long)value);
}

/* ===== status-marker lines (RaspyJack console look) =====
 *   sj_print_ok(y, "sent discover")     →   [+] sent discover     (green)
 *   sj_print_warn(y, "pool near empty") →   [!] pool near empty   (amber)
 *   sj_print_bad(y, "bind failed")      →   [-] bind failed       (coral)
 *   sj_print_info(y, "listening :80")   →   [*] listening :80     (seafoam)
 */
static inline void sj_print_marker(int y, const char *marker, uint16_t color, const char *msg)
{
    auto &d = M5Cardputer.Display;
    d.setTextColor(color, SJ_BG);
    d.setCursor(SJ_CONTENT_X, y);
    d.print(marker);
    d.setTextColor(SJ_FG, SJ_BG);
    d.print(" ");
    d.print(msg);
}

static inline void sj_print_ok(int y, const char *msg)   { sj_print_marker(y, "[+]", SJ_GOOD, msg); }
static inline void sj_print_warn(int y, const char *msg) { sj_print_marker(y, "[!]", SJ_WARN, msg); }
static inline void sj_print_bad(int y, const char *msg)  { sj_print_marker(y, "[-]", SJ_BAD,  msg); }
static inline void sj_print_info(int y, const char *msg) { sj_print_marker(y, "[*]", SJ_ACCENT, msg); }

/* ===== info block (labeled mini-border) =====
 *
 *  Used for capture data — user/domain/IP readouts on Responder + WPAD.
 *
 *  +- LAST -----------------------+
 *  | user    : DAVE\jsmith        |
 *  | ws      : WORKSTATION-01     |
 *  +------------------------------+
 */
static inline void sj_info_box(int x, int y, int w, int h, const char *label)
{
    auto &d = M5Cardputer.Display;
    d.drawRect(x, y, w, h, SJ_ACCENT_DIM);
    /* Label sits on top of the top border, inset, with black-bg knockout. */
    int lx = x + 4;
    int ly = y - 3;
    int lw = (int)strlen(label) * 6 + 4;
    d.fillRect(lx - 1, ly, lw, 7, SJ_BG);
    d.setTextColor(SJ_ACCENT, SJ_BG);
    d.setCursor(lx + 1, ly);
    d.print(label);
}

/* Put a two-column "label : value" line inside an info box at box-relative y. */
static inline void sj_info_row(int box_x, int box_y, int line_idx,
                               const char *label, const char *value)
{
    auto &d = M5Cardputer.Display;
    int y = box_y + 5 + line_idx * 9;
    d.setTextColor(SJ_FG_DIM, SJ_BG);
    d.setCursor(box_x + 4, y);
    d.print(label);
    d.setTextColor(SJ_ACCENT, SJ_BG);
    d.setCursor(box_x + 46, y);
    d.print(value);
}

/* ===== progress bar ===== */

/* Horizontal bar: x,y = top-left; w,h = size; cur/total = fill ratio. */
static inline void sj_progress_bar(int x, int y, int w, int h, uint32_t cur, uint32_t total)
{
    auto &d = M5Cardputer.Display;
    d.drawRect(x, y, w, h, SJ_ACCENT_DIM);
    int fill = 0;
    if (total > 0) fill = (int)((uint64_t)(w - 2) * cur / total);
    if (fill < 0) fill = 0;
    if (fill > w - 2) fill = w - 2;
    d.fillRect(x + 1, y + 1, fill, h - 2, SJ_GOOD);
    d.fillRect(x + 1 + fill, y + 1, w - 2 - fill, h - 2, SJ_BG);
}

/* ===== legacy helpers retained for compat ===== */

static inline void sj_header(int y, const char *title)
{
    auto &d = M5Cardputer.Display;
    d.setTextColor(SJ_ACCENT, SJ_BG);
    d.setCursor(SJ_CONTENT_X, y);
    d.print((const char *)"\xE2\x89\x8B\xE2\x89\x8B\xE2\x89\x8B [ ");
    d.print(title);
    d.print((const char *)" ] \xE2\x89\x8B\xE2\x89\x8B");
}

/* Footer — RaspyJack-style with a little wave prefix. */
static inline void sj_footer(const char *hint)
{
    auto &d = M5Cardputer.Display;
    d.fillRect(0, SCR_H - 11, SCR_W, 11, SJ_BG);
    d.drawFastHLine(0, SCR_H - 12, SCR_W, SJ_ACCENT_DIM);
    d.setTextColor(SJ_FG_DIM, SJ_BG);
    d.setCursor(3, SCR_H - 9);
    d.print((const char *)"\xE2\x89\x8B ");
    d.print(hint);
}

static inline void sj_status_dot(int x, int y, bool active)
{
    M5Cardputer.Display.fillCircle(x, y, 3, active ? SJ_GOOD : SJ_BAD);
}
