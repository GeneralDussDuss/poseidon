/*
 * ui — drawing helpers for the POSEIDON terminal UI.
 *
 * Screen layout:
 *   +------------------------------ 240 x 135 -----------------------------+
 *   | status bar (12px): radio | heap | batt | time                        |
 *   +----------------------------------------------------------------------+
 *   |                                                                      |
 *   |  body: menus, lists, status readouts                                 |
 *   |                                                                      |
 *   +----------------------------------------------------------------------+
 *   | footer (10px): hotkey hints for current screen                       |
 *   +----------------------------------------------------------------------+
 */
#pragma once

#include "app.h"

void ui_init(void);
void ui_clear_body(void);
void ui_draw_status(const char *radio, const char *extra);
void ui_draw_footer(const char *hints);
void ui_toast(const char *msg, uint16_t color, uint32_t ms);
void ui_splash(void);

/* Convenience wrappers around M5Cardputer.Display for body text drawing. */
void ui_body_println(int row, uint16_t color, const char *fmt, ...);

/* ---- animations / polish ---- */

/* Slide-in animation: push the current body off to the left over 8
 * frames while the new screen slides in from the right. Call this
 * right BEFORE a screen redraw — it captures the current body bitmap,
 * calls build_new() to get the new bitmap, then animates between. */
typedef void (*ui_draw_fn)(void);
void ui_slide_transition(ui_draw_fn build_new, int direction);
/* direction: +1 = slide right→left (forward nav), -1 = left→right (back) */

/* Spinner: draw a rotating trident at (cx, cy). Call repeatedly;
 * uses millis() for phase. */
void ui_spinner(int cx, int cy, uint16_t color);

/* Animated notification slide-in. Draws a banner from the top edge
 * that descends into view over ~150ms, then holds for hold_ms,
 * then slides back up. */
void ui_notify_slide(const char *title, const char *sub,
                     uint16_t color, uint32_t hold_ms);

/* Keypress ripple — brief expanding ring at screen center, runs
 * for ~120ms. Use from any screen for keystroke visual feedback. */
void ui_ripple(int cx, int cy, uint16_t color);

/* Matrix rain single-frame renderer. Call in a refresh loop — phase
 * advances internally via millis(). Draws into a given rect. Caller
 * is responsible for clearing the rect before the first call. */
void ui_matrix_rain(int x, int y, int w, int h, uint16_t color);
