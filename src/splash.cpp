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

/* ---- metallic trident sprite ----
 * 48 wide x 72 tall, rendered live each frame so we can shift gradients.
 * Each call draws at (cx, top_y). The sprite rises from below during the
 * intro, so `clip_y` caps drawing above that line for the waterline effect.
 */
static void draw_trident_metal(int cx, int top_y, int clip_y, uint8_t brightness)
{
    auto &d = M5Cardputer.Display;

    /* Palette: shaft gets a vertical gradient from bright top to dim
     * base. `brightness` ramps during the fade-in. */
    uint16_t hi = blend565(0x0000, 0xFFFF, brightness);
    uint16_t md = blend565(0x0000, COL_ACCENT, brightness);
    uint16_t lo = blend565(0x0000, 0x4A49, brightness);  /* dim teal */
    uint16_t sh = blend565(0x0000, 0x18C7, brightness);

    auto put = [&](int x, int y, uint16_t c) {
        if (y < clip_y) return;
        d.drawPixel(x, y, c);
    };

    /* Crown bar (14 wide, 3 tall) with shading. */
    for (int x = -14; x <= 14; ++x) {
        put(cx + x, top_y + 0, md);
        put(cx + x, top_y + 1, hi);
        put(cx + x, top_y + 2, md);
    }
    /* Left / right outer tines — tapered. */
    for (int i = 0; i < 12; ++i) {
        int x = -14 + i / 6;
        int y = top_y - i - 1;
        put(cx + x,     y, md);
        put(cx + x - 1, y, sh);
        put(cx - x,     y, md);
        put(cx - x + 1, y, sh);
    }
    /* Barbs at tine tips. */
    for (int i = 0; i < 4; ++i) {
        put(cx - 14 - i, top_y - 12 + i, md);
        put(cx + 14 + i, top_y - 12 + i, md);
    }
    /* Center tine — bigger. */
    for (int i = 0; i < 18; ++i) {
        int y = top_y - i - 1;
        put(cx - 1, y, hi);
        put(cx,     y, md);
        put(cx + 1, y, md);
        put(cx + 2, y, sh);
    }
    /* Center tine barb (arrowhead). */
    for (int i = 0; i < 5; ++i) {
        int y = top_y - 18 - i;
        for (int x = -2 + i; x <= 2 - i; ++x) put(cx + x, y, hi);
    }
    /* Shaft — 2 wide, gradient down. */
    int shaft_h = 42;
    for (int i = 0; i < shaft_h; ++i) {
        int y = top_y + 3 + i;
        uint8_t t = (uint8_t)(i * 180 / shaft_h);
        uint16_t c = blend565(hi, lo, t);
        put(cx - 1, y, c);
        put(cx,     y, c);
        put(cx + 1, y, blend565(c, sh, 90));
    }
    /* Grip rings (three, evenly spaced). */
    for (int g = 0; g < 3; ++g) {
        int y = top_y + 22 + g * 6;
        for (int x = -3; x <= 3; ++x) put(cx + x, y, hi);
        for (int x = -3; x <= 3; ++x) put(cx + x, y + 1, sh);
    }
    /* Spear tip below shaft. */
    int base_y = top_y + 3 + shaft_h;
    for (int i = 0; i < 5; ++i) {
        for (int x = -(2 - i / 2); x <= (2 - i / 2); ++x) put(cx + x, base_y + i, md);
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

    const int horizon = 95;
    const int cx = SCR_W / 2;
    const int trident_final_top = 46;

    /* ---- Phase 1: rising from the deep (40 frames, ~1.2s) ---- */
    for (int f = 0; f <= 40; ++f) {
        uint32_t frame_start = millis();

        uint8_t bright = (uint8_t)(f * 255 / 40);

        /* Background stays — only redraw animated bands. */
        if (f == 0) draw_bg_gradient(bright);
        else {
            /* Refresh gradient each frame so brightness ramps smoothly. */
            draw_bg_gradient(bright);
        }

        /* Trident top Y: starts below horizon, rises to final_top. */
        int t_top = horizon + 40 - f * (horizon + 40 - trident_final_top) / 40;

        /* Clip: never draw trident pixels below the horizon (submerged). */
        draw_trident_metal(cx, t_top, 0, bright);

        /* Water fills below horizon. */
        d.fillRect(0, horizon, SCR_W, SCR_H - horizon, 0x0000);

        /* Caustics band just below the surface (8 px tall). */
        draw_caustics(horizon + 1, 8, f * 3, bright);

        /* Subtle surface line — two-color wave for extra depth. */
        for (int x = 0; x < SCR_W; ++x) {
            int w = sin64[(x * 3 + f * 5) & 63] / 16;
            d.drawPixel(x, horizon + w, COL_ACCENT);
            d.drawPixel(x, horizon + w + 1, 0x2124);
        }

        /* Ripple around the trident as it pierces the surface. */
        if (t_top > horizon - 30 && t_top < horizon + 20) {
            int r = (f % 10) * 3 + 4;
            d.drawEllipse(cx, horizon, r, r / 3, COL_DIM);
            d.drawEllipse(cx, horizon, r - 2, (r - 2) / 3, 0x18C7);
        }

        if (input_poll() != PK_NONE) goto idle;

        uint32_t e = millis() - frame_start;
        if (e < 30) delay(30 - e);
    }

    /* ---- Phase 2: god rays sweep (25 frames, ~0.75s) ---- */
    for (int f = 0; f <= 25; ++f) {
        uint32_t frame_start = millis();
        uint8_t alpha = (uint8_t)(f * 255 / 25);

        /* Redraw water band with caustics + rays. */
        d.fillRect(0, horizon, SCR_W, SCR_H - horizon, 0x0000);
        draw_rays(horizon - 40, horizon, f * 2, alpha);
        draw_caustics(horizon + 1, 8, f * 3 + 100, 255);

        /* Reinforce surface line. */
        for (int x = 0; x < SCR_W; ++x) {
            int w = sin64[(x * 3 + f * 5 + 200) & 63] / 16;
            d.drawPixel(x, horizon + w, COL_ACCENT);
        }

        if (input_poll() != PK_NONE) goto idle;
        uint32_t e = millis() - frame_start;
        if (e < 30) delay(30 - e);
    }

    /* ---- Phase 3: title bloom + scanline (30 frames, ~0.9s) ---- */
    for (int f = 0; f <= 30; ++f) {
        uint32_t frame_start = millis();
        /* Clear title band each frame. */
        d.fillRect(0, 76, SCR_W, 24, 0x0000);

        uint8_t alpha = (uint8_t)(min(255, f * 255 / 20));
        int scanline_x = -1;
        if (f >= 8 && f <= 24) {
            scanline_x = (f - 8) * SCR_W / 16;
        }
        draw_title(alpha, scanline_x);

        if (input_poll() != PK_NONE) goto idle;
        uint32_t e = millis() - frame_start;
        if (e < 30) delay(30 - e);
    }

    /* Subtitle + version + accent rule after title is fully in. */
    {
        const char *sub = "commander of the deep";
        int sw = d.textWidth(sub);
        d.setTextColor(COL_DIM, 0);
        d.setCursor((SCR_W - sw) / 2, 106);
        d.print(sub);
        for (int w = 0; w <= 100; w += 10) {
            d.drawFastHLine((SCR_W - w) / 2, 118, w, COL_ACCENT);
            if (input_poll() != PK_NONE) goto idle;
            delay(12);
        }
        char vbuf[32];
        snprintf(vbuf, sizeof(vbuf), "v%s  any key", POSEIDON_VERSION);
        int vw = d.textWidth(vbuf);
        d.setTextColor(COL_MAGENTA, 0);
        d.setCursor((SCR_W - vw) / 2, 122);
        d.print(vbuf);
    }

idle:
    /* ---- Phase 4: ambient idle — waves keep rolling until key ---- */
    int phase = 0;
    while (true) {
        uint32_t frame_start = millis();
        d.fillRect(0, horizon - 2, SCR_W, 12, 0x0000);
        draw_caustics(horizon + 1, 8, phase, 255);
        for (int x = 0; x < SCR_W; ++x) {
            int w = sin64[(x * 3 + phase) & 63] / 16;
            d.drawPixel(x, horizon + w, COL_ACCENT);
        }
        phase += 3;
        if (input_poll() != PK_NONE) return;
        uint32_t e = millis() - frame_start;
        if (e < 60) delay(60 - e);
    }
}
