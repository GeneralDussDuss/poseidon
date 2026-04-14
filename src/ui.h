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

/* Radial wave pulse animation — 3 expanding glow rings + sweeping
 * arcs at (cx, cy). Ported from Evil-Cardputer's NTLM waiting anim.
 * Call in a refresh loop. Caller clears the region. */
void ui_waves(int cx, int cy, int max_radius, uint16_t base_color);

/* Radar sweep — rotating line with phosphor afterglow + dot blips.
 * Perfect for scan-in-progress screens. Call in refresh loop. */
void ui_radar(int cx, int cy, int radius, uint16_t color);

/* Horizontal hex data stream — fake packet-capture scrolling readout.
 * Draws 3 rows of random 2-digit hex pairs flowing right-to-left at
 * varying speeds. Use on capture / attack screens. */
void ui_hexstream(int x, int y, int w, int h, uint16_t color);

/* Glitch block effect — random horizontal stripes in glitch colors.
 * Use on DoS/flood screens for that hacker-chaos vibe. */
void ui_glitch(int x, int y, int w, int h);

/* Signal bars — 5 animated EQ-style bouncing bars. Good for any
 * active-transmission indicator. Call in refresh loop. */
void ui_eq_bars(int x, int y, int bar_w, int bar_h_max, uint16_t color);

/* Full-screen dramatic overlay — big headline + subtitle + choice of
 * backdrop animation. Blocks for ~duration_ms. Use for "HANDSHAKE!",
 * "TARGET ACQUIRED", "SIGNAL LOST", etc. */
enum action_anim_t {
    ACT_BG_RADAR,     /* central radar sweep behind text */
    ACT_BG_WAVES,     /* radial pulse behind text */
    ACT_BG_MATRIX,    /* full-screen matrix rain behind text */
    ACT_BG_GLITCH,    /* intense glitch blocks behind text */
};
void ui_action_overlay(const char *headline, const char *subtitle,
                       action_anim_t bg, uint16_t color, uint32_t duration_ms);
