/*
 * SaltyJack — pirate icon pack, sprite-backed.
 *
 * Icons are Gemini-generated pixel art in the same phosphor-terminal
 * pixel-art style as the boot splash (seafoam/cyan on black). Each
 * sprite is 24x24 RGB565, baked to flash via tools/sprite_sheet_to_icons.py.
 *
 * Rendering: each icon function pixel-doubles the sprite at the given
 * scale. At scale=1 you get 24x24 native. At scale=2 → 48x48, at 3 → 72x72.
 * Pixel-doubling keeps the art blocky, which matches the splash aesthetic
 * way better than a bilinear upscale would.
 *
 * The `color` parameter is accepted for API compat with the old primitive
 * version but is IGNORED — sprites are pre-colored. Pass anything.
 */
#pragma once

#include <Arduino.h>
#include <M5Cardputer.h>
#include "saltyjack_style.h"
#include "saltyjack_sprites.h"

/* Draws a 24x24 RGB565 sprite at (x, y) scaled up by `s` via pixel
 * doubling. Each source pixel becomes an s×s filled rect. Cheap at the
 * scales we use (max ~72x72). */
static inline void sj_draw_sprite(const uint16_t *sprite, int x, int y, int s)
{
    auto &d = M5Cardputer.Display;
    if (s <= 1) {
        d.pushImage(x, y, SJ_SPRITE_W, SJ_SPRITE_H, sprite);
        return;
    }
    for (int sy = 0; sy < SJ_SPRITE_H; ++sy) {
        for (int sx = 0; sx < SJ_SPRITE_W; ++sx) {
            uint16_t c = sprite[sy * SJ_SPRITE_W + sx];
            if (c == 0x0000) continue;  /* skip true-black pixels — lets the bg show through */
            d.fillRect(x + sx * s, y + sy * s, s, s, c);
        }
    }
}

/* Each icon is just a dispatcher to sj_draw_sprite with its sprite. */
static void icon_flag  (int x, int y, uint16_t, int s = 1) { sj_draw_sprite(sj_spr_flag,   x, y, s); }
static void icon_skull (int x, int y, uint16_t, int s = 1) { sj_draw_sprite(sj_spr_skull,  x, y, s); }
static void icon_swords(int x, int y, uint16_t, int s = 1) { sj_draw_sprite(sj_spr_swords, x, y, s); }
static void icon_wheel (int x, int y, uint16_t, int s = 1) { sj_draw_sprite(sj_spr_wheel,  x, y, s); }
static void icon_horn  (int x, int y, uint16_t, int s = 1) { sj_draw_sprite(sj_spr_horn,   x, y, s); }
static void icon_web   (int x, int y, uint16_t, int s = 1) { sj_draw_sprite(sj_spr_web,    x, y, s); }
static void icon_key   (int x, int y, uint16_t, int s = 1) { sj_draw_sprite(sj_spr_key,    x, y, s); }

typedef void (*sj_icon_fn)(int, int, uint16_t, int);
