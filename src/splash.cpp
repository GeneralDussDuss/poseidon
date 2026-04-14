/*
 * splash.cpp — POSEIDON splash sequence.
 *
 * Not hand-drawn lines — procedural rendering with:
 *   1. Real sine-interference ocean caustics, animated
 *   2. Rising god rays from the waterline
 *   3. Metallic trident sprite with proper gradient shading (48x72)
 *   4. Title with glow outline + scanline sweep
 *   5. Continuous ambient wave animation while waiting for key
 *
 * Budget: ~3 seconds. Any keypress skips to idle loop immediately.
 */
#include "app.h"
#include "ui.h"
#include "input.h"
#include "sprites/splash_sprite.h"
#include <math.h>

/* ---- helpers ---- */

static uint16_t blend565(uint16_t a, uint16_t b, uint8_t t)
{
    uint8_t ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    uint8_t br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
    uint8_t r = (ar * (255 - t) + br * t) / 255;
    uint8_t g = (ag * (255 - t) + bg * t) / 255;
    uint8_t bl = (ab * (255 - t) + bb * t) / 255;
    return (r << 11) | (g << 5) | bl;
}

/* 64-entry sine LUT, output range [-64, +64]. */
static const int8_t sin64[64] = {
      0,  6, 12, 18, 24, 30, 36, 41, 47, 52, 56, 60, 63, 64, 64, 64,
     63, 60, 56, 52, 47, 41, 36, 30, 24, 18, 12,  6,  0, -6,-12,-18,
    -24,-30,-36,-41,-47,-52,-56,-60,-63,-64,-64,-64,-63,-60,-56,-52,
    -47,-41,-36,-30,-24,-18,-12, -6,  0,  6, 12, 18, 24, 30, 36, 41,
};

/* ---- sprite blitter ----
 * Draws splash_data (56x80 pixel art) centered at (cx, sprite_top_y).
 * Pixels equal to splash_alpha are skipped (transparency). Pixels are
 * blended toward black by `brightness` so the sprite can fade in.
 * `clip_y` is a y-cutoff (don't draw above this line) — used so the
 * sprite "rises" from below a waterline.
 */
static void draw_sprite(int cx, int sprite_top_y, int clip_y, uint8_t brightness)
{
    auto &d = M5Cardputer.Display;
    int origin_x = cx - splash_w / 2;

    for (int y = 0; y < splash_h; ++y) {
        int dy = sprite_top_y + y;
        if (dy < clip_y) continue;
        if (dy < 0 || dy >= SCR_H) continue;
        for (int x = 0; x < splash_w; ++x) {
            uint16_t c = splash_data[y * splash_w + x];
            if (c == splash_alpha) continue;
            int dx = origin_x + x;
            if (dx < 0 || dx >= SCR_W) continue;
            /* Fade toward black by (255 - brightness). */
            uint16_t out = (brightness == 255) ? c : blend565(0x0000, c, brightness);
            d.drawPixel(dx, dy, out);
        }
    }
}

/* ---- ocean caustics ----
 * Interference pattern from two phase-offset sines, sampled across the
 * water band. Pixel brightness is the absolute sum of two sines; pixels
 * above a threshold get drawn as bright cyan, making moving bright lines
 * that look like refracted light on the sea floor.
 */
static void draw_caustics(int y0, int h, int phase, uint8_t brightness)
{
    auto &d = M5Cardputer.Display;
    uint16_t bright = blend565(0x0000, 0x5FDF, brightness);
    uint16_t dim    = blend565(0x0000, 0x1082, brightness);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < SCR_W; ++x) {
            int a = sin64[(x + phase) & 63];
            int b = sin64[(x * 2 + y * 3 + phase * 3 / 2) & 63];
            int v = (a + b);
            if (v > 80)       d.drawPixel(x, y0 + y, bright);
            else if (v > 40)  d.drawPixel(x, y0 + y, dim);
            else              d.drawPixel(x, y0 + y, 0x0000);
        }
    }
}

/* ---- god rays (light beams from above water) ---- */
static void draw_rays(int from_y, int to_y, int phase, uint8_t alpha)
{
    auto &d = M5Cardputer.Display;
    /* Four rays at fixed x, slight wobble. Drawn with increasing
     * transparency downward by blending toward black. */
    static const int xs[] = { 60, 105, 150, 195 };
    for (int i = 0; i < 4; ++i) {
        int wobble = sin64[(phase + i * 16) & 63] / 12;
        int ray_x = xs[i] + wobble;
        for (int y = from_y; y < to_y; ++y) {
            uint8_t t = (uint8_t)((y - from_y) * 255 / (to_y - from_y));
            uint16_t c = blend565(COL_ACCENT, 0x0000, t);
            c = blend565(0x0000, c, alpha);
            d.drawPixel(ray_x,     y, c);
            if (y & 1) d.drawPixel(ray_x + 1, y, c);
        }
    }
}

/* ---- Gradient background for the whole screen. */
static void draw_bg_gradient(uint8_t brightness)
{
    auto &d = M5Cardputer.Display;
    /* Sky → deep ocean. */
    uint16_t sky = blend565(0x0000, 0x0010, brightness);  /* near-black blue */
    uint16_t mid = blend565(0x0000, 0x1082, brightness);
    for (int y = 0; y < SCR_H; ++y) {
        uint8_t t = (uint8_t)(y * 255 / SCR_H);
        uint16_t c = blend565(sky, mid, t);
        d.drawFastHLine(0, y, SCR_W, c);
    }
}

/* ---- Title with glow + scanline sweep. */
static void draw_title(uint8_t title_alpha, int scanline_x)
{
    auto &d = M5Cardputer.Display;
    const char *title = "POSEIDON";
    d.setTextSize(2);
    int tw = d.textWidth(title) * 2;
    int tx = (SCR_W - tw) / 2;
    int ty = 82;

    /* Glow: draw multiple times with increasing brightness for a bloom effect. */
    uint16_t glow_dim  = blend565(0x0000, 0x0104, title_alpha);
    uint16_t glow_med  = blend565(0x0000, 0x0307, title_alpha);
    uint16_t glow_hot  = blend565(0x0000, COL_ACCENT, title_alpha);

    d.setTextColor(glow_dim, 0);
    d.setCursor(tx - 1, ty); d.print(title);
    d.setCursor(tx + 1, ty); d.print(title);
    d.setCursor(tx, ty - 1); d.print(title);
    d.setCursor(tx, ty + 1); d.print(title);

    d.setTextColor(glow_med, 0);
    d.setCursor(tx, ty); d.print(title);

    d.setTextColor(glow_hot, 0);
    d.setCursor(tx, ty); d.print(title);

    /* Scanline sweep — bright white bar moves across the title. */
    if (scanline_x >= 0 && scanline_x < SCR_W) {
        for (int y = ty; y < ty + 16; ++y) {
            int d_abs = scanline_x - (SCR_W / 2);
            (void)d_abs;
            d.drawPixel(scanline_x, y, 0xFFFF);
            d.drawPixel(scanline_x - 1, y, 0x8410);
            d.drawPixel(scanline_x + 1, y, 0x8410);
        }
    }
    d.setTextSize(1);
}

/* ---- Splash entry point, replaces old ui_splash. ---- */
void ui_splash(void)
{
    auto &d = M5Cardputer.Display;
    d.fillScreen(0x0000);

    const int cx = SCR_W / 2;
    /* Sprite sits at y=0..79, title below, subtitle + rule + version after. */
    const int sprite_final_y = 0;
    /* Waterline sweeps up as the sprite rises — starts at screen bottom,
     * ends at the sprite's baseline (~y=80). */
    const int waterline_start = SCR_H - 4;
    const int waterline_final = splash_h - 2;

    /* ---- Phase 1: sprite rises from below screen (40 frames, ~1.2s) ---- */
    for (int f = 0; f <= 40; ++f) {
        uint32_t frame_start = millis();
        uint8_t bright = (uint8_t)(f * 255 / 40);

        draw_bg_gradient(bright);

        /* Sprite top Y: starts at SCR_H (offscreen), rises to 0. */
        int s_top = SCR_H - f * (SCR_H - sprite_final_y) / 40;
        /* Waterline rises with it. */
        int horizon = waterline_start - f * (waterline_start - waterline_final) / 40;

        /* Draw sprite but clip at the waterline (below horizon = submerged). */
        draw_sprite(cx, s_top, 0, bright);

        /* Water fills below the horizon. */
        d.fillRect(0, horizon, SCR_W, SCR_H - horizon, 0x0000);

        /* Caustics just beneath the surface. */
        draw_caustics(horizon + 1, 6, f * 3, bright);

        /* Surface line — animated wave. */
        for (int x = 0; x < SCR_W; ++x) {
            int w = sin64[(x * 3 + f * 5) & 63] / 16;
            d.drawPixel(x, horizon + w, COL_ACCENT);
            d.drawPixel(x, horizon + w + 1, 0x2124);
        }

        /* Ripple at the sprite's emerging point. */
        if (horizon > sprite_final_y && horizon < SCR_H - 8) {
            int r = (f % 12) * 3 + 6;
            d.drawEllipse(cx, horizon, r, r / 3, COL_DIM);
            d.drawEllipse(cx, horizon, r - 3, (r - 3) / 3, 0x18C7);
        }

        if (input_poll() != PK_NONE) goto idle;

        uint32_t e = millis() - frame_start;
        if (e < 30) delay(30 - e);
    }

    /* ---- Phase 2: god rays sweep (20 frames, ~0.6s) ---- */
    for (int f = 0; f <= 20; ++f) {
        uint32_t frame_start = millis();
        uint8_t alpha = (uint8_t)(f * 255 / 20);

        /* Redraw sprite (on top) + water band underneath it. */
        d.fillRect(0, waterline_final, SCR_W, SCR_H - waterline_final, 0x0000);
        draw_rays(waterline_final - 30, waterline_final, f * 2, alpha);
        draw_caustics(waterline_final + 1, 6, f * 3 + 100, 255);

        for (int x = 0; x < SCR_W; ++x) {
            int w = sin64[(x * 3 + f * 5 + 200) & 63] / 16;
            d.drawPixel(x, waterline_final + w, COL_ACCENT);
        }

        if (input_poll() != PK_NONE) goto idle;
        uint32_t e = millis() - frame_start;
        if (e < 30) delay(30 - e);
    }

    /* ---- Phase 3: title appears below sprite (20 frames, ~0.6s) ---- */
    for (int f = 0; f <= 20; ++f) {
        uint32_t frame_start = millis();
        d.fillRect(0, 85, SCR_W, 14, 0x0000);

        uint8_t alpha = (uint8_t)(min(255, f * 255 / 14));
        int scanline_x = -1;
        if (f >= 6 && f <= 16) {
            scanline_x = (f - 6) * SCR_W / 10;
        }

        /* Draw title at y=85 (moved from 82 to make room for sprite). */
        const char *title = "POSEIDON";
        d.setTextSize(2);
        int tw = d.textWidth(title) * 2;
        int tx = (SCR_W - tw) / 2;
        int ty = 85;

        uint16_t glow_dim = blend565(0x0000, 0x0104, alpha);
        uint16_t glow_hot = blend565(0x0000, COL_ACCENT, alpha);
        d.setTextColor(glow_dim, 0);
        d.setCursor(tx - 1, ty); d.print(title);
        d.setCursor(tx + 1, ty); d.print(title);
        d.setTextColor(glow_hot, 0);
        d.setCursor(tx, ty); d.print(title);
        d.setTextSize(1);

        if (scanline_x >= 0 && scanline_x < SCR_W) {
            for (int y = ty; y < ty + 16; ++y) {
                d.drawPixel(scanline_x, y, 0xFFFF);
                d.drawPixel(scanline_x - 1, y, 0x8410);
                d.drawPixel(scanline_x + 1, y, 0x8410);
            }
        }

        if (input_poll() != PK_NONE) goto idle;
        uint32_t e = millis() - frame_start;
        if (e < 30) delay(30 - e);
    }

    /* Subtitle below title, accent rule, version. All below y=100. */
    {
        const char *sub = "commander of the deep";
        d.setTextColor(COL_DIM, 0);
        int sw = d.textWidth(sub);
        d.setCursor((SCR_W - sw) / 2, 104);
        d.print(sub);
        for (int w = 0; w <= 100; w += 10) {
            d.drawFastHLine((SCR_W - w) / 2, 116, w, COL_ACCENT);
            if (input_poll() != PK_NONE) goto idle;
            delay(10);
        }
        char vbuf[32];
        snprintf(vbuf, sizeof(vbuf), "v%s  any key", POSEIDON_VERSION);
        int vw = d.textWidth(vbuf);
        d.setTextColor(COL_MAGENTA, 0);
        d.setCursor((SCR_W - vw) / 2, 122);
        d.print(vbuf);
    }

idle:
    /* ---- Phase 4: ambient idle — caustics under the sprite until key ---- */
    {
        const int water_y = splash_h - 2;
        int phase = 0;
        while (true) {
            uint32_t frame_start = millis();
            d.fillRect(0, water_y - 1, SCR_W, 8, 0x0000);
            draw_caustics(water_y + 1, 4, phase, 255);
            for (int x = 0; x < SCR_W; ++x) {
                int w = sin64[(x * 3 + phase) & 63] / 16;
                d.drawPixel(x, water_y + w, COL_ACCENT);
            }
            phase += 3;
            if (input_poll() != PK_NONE) return;
            uint32_t e = millis() - frame_start;
            if (e < 60) delay(60 - e);
        }
    }
}
