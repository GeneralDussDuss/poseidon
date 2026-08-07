/*
 * menu_icons — pictographic glyphs for the carousel-mode hotkey badge.
 *
 * Source bitmaps live in menu_icons_data.h, generated from
 * assets/icons.jpg by scripts/convert_icons.py. The dispatcher here
 * maps top-level POSEIDON menu hotkeys to their bitmap and renders via
 * M5Cardputer.Display.drawBitmap (1-bit, "1" pixels in `color`,
 * "0" pixels transparent so the badge fill shows through).
 *
 * Submenu items return false from the dispatcher and the carousel
 * falls back to its big-letter rendering — already on-vibe inside a
 * named submenu (the title bar names the parent domain).
 */
#pragma once

#include "menu.h"

/* Returns true if it drew an icon. Returns false if the carousel
 * should render the hotkey letter instead. (cx, cy) is the center of
 * the badge interior — the dispatcher offsets the bitmap by half its
 * width / height to draw it centered. */
bool draw_menu_icon(int cx, int cy, uint16_t color,
                    const menu_node_t *parent, const menu_node_t *item);

/* Raw bitmap lookup by hotkey only (no parent/item gating) — the
 * accessor picons.cpp uses so the MENU_ICON_* table lives in exactly
 * one place. Returns nullptr if the hotkey has no icon. Bitmaps are
 * menu_icon_w() x menu_icon_h() pixels, 1-bit, MSB-first row packing. */
const uint8_t *menu_icon_bitmap_for_hotkey(char hotkey);

/* Native bitmap dimensions (before any picon scaling). Function
 * accessors rather than re-exported #defines so menu_icons_data.h
 * (and its bitmap arrays) never needs a second #include elsewhere. */
int menu_icon_w(void);
int menu_icon_h(void);
