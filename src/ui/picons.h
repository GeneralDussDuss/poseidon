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

/* Draw a root icon centred on (cx, cy), scaled by `scale` (integer,
 * 1 = native 24x24), with a bloom halo. glow: 0 = none, 1 = subtle,
 * 2 = strong. Returns false if there is no icon for that hotkey (not a
 * MENU_ROOT item), so the caller can fall back to the hotkey letter. */
bool picon_draw_hotkey(char hotkey, int cx, int cy, int scale,
                       uint16_t colour, int glow);
