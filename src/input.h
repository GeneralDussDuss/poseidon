/*
 * input — Cardputer keyboard event abstraction.
 *
 * Polls the hardware keyboard and normalizes key presses into a single
 * code point per event. `PK_` prefix is used on special keys because
 * M5Cardputer's own Keyboard.h defines KEY_ENTER/KEY_DELETE/etc. for
 * HID keycodes — a different namespace entirely.
 */
#pragma once

#include <Arduino.h>

/* Special keys (above the 0x7F printable range). */
enum : uint16_t {
    PK_NONE   = 0,
    PK_ENTER  = 0x0D,
    PK_ESC    = 0x1B,
    PK_BKSP   = 0x08,
    PK_TAB    = 0x09,
    PK_SPACE  = 0x20,
    PK_UP     = 0x100,
    PK_DOWN   = 0x101,
    PK_LEFT   = 0x102,
    PK_RIGHT  = 0x103,
    PK_FN     = 0x104,
};

/* One event per key press (not repeat). Returns PK_NONE if no event. */
uint16_t input_poll(void);

/* Last key code returned (for debug overlays / diagnostics). */
uint16_t input_last_key(void);

/* Modal line editor. Returns true on ENTER, false on ESC. */
bool input_line(const char *prompt, char *out_buf, size_t out_sz);

/* Inject a keypress into the poll queue (for TRIDENT PC Bridge). */
void input_inject(uint16_t code);
