/*
 * picons.cpp — see picons.h.
 *
 * The 1-bit bitmaps and their hotkey table live in menu_icons_data.h /
 * menu_icons.cpp; this file only adds scaling (drawBitmap has no scale
 * parameter, so the mask is blitted manually, one filled scale x scale
 * block per set bit) and the bloom halo.
 */
#include "picons.h"
#include "../menu_icons.h"
#include <M5Cardputer.h>

/* Dim an RGB565 colour by right-shifting each channel — used for the
 * halo so it reads as a soft out-of-focus copy of the crisp icon
 * rather than a second solid outline. shift 3 = subtle, shift 2 =
 * strong (glow strength 1 / 2 respectively). */
static uint16_t dim565(uint16_t c, int shift)
{
    uint8_t r = (uint8_t)((c >> 11) & 0x1F) >> shift;
    uint8_t g = (uint8_t)((c >> 5)  & 0x3F) >> shift;
    uint8_t b = (uint8_t)(c & 0x1F)         >> shift;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

/* Blit the bitmap scaled, offset by (ox, oy) pixels from the icon's
 * top-left, in a single flat colour. Clear bits are transparent (left
 * untouched), matching drawBitmap's own behaviour. */
static void picon_blit(const uint8_t *bmp, int w, int h, int stride,
                       int x0, int y0, int scale,
                       int ox, int oy, uint16_t colour)
{
    auto &d = M5Cardputer.Display;
    for (int row = 0; row < h; ++row) {
        const uint8_t *rowp = bmp + row * stride;
        for (int col = 0; col < w; ++col) {
            int byte_idx = col >> 3;
            int bit      = 7 - (col & 7);
            if ((rowp[byte_idx] >> bit) & 1) {
                d.fillRect(x0 + ox + col * scale, y0 + oy + row * scale,
                          scale, scale, colour);
            }
        }
    }
}

bool picon_draw_hotkey(char hotkey, int cx, int cy, int scale,
                       uint16_t colour, int glow)
{
    const uint8_t *bmp = menu_icon_bitmap_for_hotkey(hotkey);
    if (!bmp) return false;
    if (scale < 1) scale = 1;

    const int w      = menu_icon_w();
    const int h      = menu_icon_h();
    const int stride = (w + 7) / 8;
    const int sw     = w * scale;
    const int sh     = h * scale;
    const int x0     = cx - sw / 2;
    const int y0     = cy - sh / 2;

    if (glow > 0) {
        int shift = (glow >= 2) ? 2 : 3;
        uint16_t halo = dim565(colour, shift);
        picon_blit(bmp, w, h, stride, x0, y0, scale, -scale, 0,      halo);
        picon_blit(bmp, w, h, stride, x0, y0, scale,  scale, 0,      halo);
        picon_blit(bmp, w, h, stride, x0, y0, scale,  0,     -scale, halo);
        picon_blit(bmp, w, h, stride, x0, y0, scale,  0,      scale, halo);
    }
    picon_blit(bmp, w, h, stride, x0, y0, scale, 0, 0, colour);
    return true;
}
