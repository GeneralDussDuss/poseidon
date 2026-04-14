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
