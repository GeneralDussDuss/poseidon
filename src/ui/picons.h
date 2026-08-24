/*
 * picons - glowing renderer for the root menu icon bitmaps.
 *
 * Reuses the 1-bit bitmap table owned by menu_icons.cpp (menu_icons_data.h)
 * instead of duplicating it. Adds two things menu_icons.cpp doesn't do:
 * integer upscaling (drawBitmap has no scale parameter) and a bloom halo
 * so the icon reads as "lit" on the 320x170 T-Embed carousel card.
 *
 * Pure rendering helper — no navigation/state, board-neutral (uses
 * M5Cardputer.Display like the rest of the UI layer on both boards).
 */
#pragma once

#include <stdint.h>

/* Draw a root icon centred on (cx, cy) at `size` pixels square, alpha-blended
 * over `bg` with a horizontal gradient tint from c1 to c2 (pass the same
 * colour twice for a flat fill). Edges are anti-aliased; the neon halo is
 * baked into the source coverage map, so no separate glow pass is needed.
 * `size` is clamped to the 72 px native art - it is never upscaled past that.
 * Returns false if the hotkey has no icon, so the caller can fall back to
 * drawing the hotkey letter. */
/* Draw the icon for one menu entry, resolved from (menu, hotkey).
 *
 * Hotkeys are only unique WITHIN a menu (s is Scan in WiFi, Sniffer in nRF24,
 * Settings in System), so the parent menu label selects which sheet and mapping
 * to use. Pass nullptr for the root menu. Returns false when that entry has no
 * icon, so the caller can fall back to drawing the hotkey letter. */
bool picon_draw_menu(const char *menu_label, char hotkey, int cx, int cy,
                     int size, uint16_t c1, uint16_t c2, uint16_t bg);

bool picon_draw_tinted(char hotkey, int cx, int cy, int size,
                       uint16_t c1, uint16_t c2, uint16_t bg);

/* Back-compat wrapper for the old scale-based API (scale is interpreted as a
 * multiple of 24 px). `glow` is accepted but ignored: the halo now comes from
 * the artwork's own alpha. Prefer picon_draw_tinted for new code. */
bool picon_draw_hotkey(char hotkey, int cx, int cy, int scale,
                       uint16_t colour, int glow);
