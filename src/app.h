/*
 * POSEIDON — shared types, colors, constants.
 */
#pragma once

#include <Arduino.h>
#include <M5Cardputer.h>

/* ---- palette (16-bit 565, via M5Cardputer.Display) ---- */
#define COL_BG       0x0000  /* black */
#define COL_FG       0xFFFF  /* white */
#define COL_ACCENT   0x07FF  /* cyan */
#define COL_WARN     0xFFE0  /* yellow */
#define COL_BAD      0xF800  /* red */
#define COL_GOOD     0x07E0  /* green */
#define COL_DIM      0x7BEF  /* grey */
#define COL_MAGENTA  0xF81F

/* ---- display geometry ---- */
#if defined(POSEIDON_BOARD_TEMBED)
/* T-Embed CC1101: ST7789 170x320 physical, run at rotation 3. */
#define SCR_W 320
#define SCR_H 170
#else
/* M5Stack Cardputer. */
#define SCR_W 240
#define SCR_H 135
#endif
#define STATUS_H 12
/* No footer on the T-Embed: its only content was a keyboard key legend,
 * which is meaningless on an encoder-driven board. Zeroing the height
 * gives those rows back to the body rather than reserving dead space. */
#if defined(POSEIDON_BOARD_TEMBED)
#define FOOTER_H 0
#else
#define FOOTER_H 10
#endif
#define BODY_Y   (STATUS_H)
#define BODY_H   (SCR_H - STATUS_H - FOOTER_H)
#define FOOTER_Y (SCR_H - FOOTER_H)
#define MENU_ROW_H 13
#define MENU_ROWS  (BODY_H / MENU_ROW_H)

/* Visible rows for a scrolling list: how many `pitch`-px rows fit in the body
 * below a `top`-px header, where row r is drawn at BODY_Y + top + r * pitch.
 *
 * Every list screen used to hardcode this for the Cardputer's 113 px body, so
 * on the T-Embed (158 px, no footer) they all left roughly a third of the
 * screen empty -- and several were actually one row too MANY on the Cardputer,
 * overlapping the footer. Deriving it fixes both directions at once. */
#define LIST_ROWS(top, pitch)  ((BODY_H - (top)) / (pitch))

/* Characters of the built-in 6px font that fit from x to the right edge.
 * Trailing text fields (SSID, device name) were clipped to constants chosen for
 * the 240px Cardputer, so on the 320px T-Embed they truncated with ~80px of
 * empty panel to their right. Use with "%.*s". */
#define FIT_CHARS(x)  ((SCR_W - (x) - 2) / 6)

/* ---- build info ---- */
/* POSEIDON_VERSION comes from -D in platformio.ini; src/version.h
 * provides an #ifndef-guarded fallback. Defining it here too caused
 * a compiler "redefined" warning on every build. */
