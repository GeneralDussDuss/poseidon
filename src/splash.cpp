/*
 * splash.cpp — Neptune engraving splash with animated matrix rain.
 *
 * Based on Agostino Carracci's "Neptune" (c. 1588-1595), public
 * domain via Wikimedia Commons, converted to 94x120 RGB565.
 *
 * Animation phases:
 *   1. Fade-in: black screen → engraving ramps brightness
 *   2. Matrix rain spawns around the figure
 *   3. Title "POSEIDON" slides in from below with magenta glow
 *   4. Accent rule grows outward
 *   5. Ambient idle — rain + shimmer until any keypress
 */
#include "app.h"
#include "ui.h"
#include "input.h"
#include "sprites/splash_sprite.h"
#include <math.h>
#include <esp_random.h>

/* Theme colors — magenta accent restored per user request. */
#define COL_MAGENTA_ACCENT 0xF81F
#define COL_MAGENTA_DEEP   0x9013

static uint16_t blend565(uint16_t a, uint16_t b, uint8_t t)
{
    uint8_t ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    uint8_t br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
    uint8_t r = (ar * (255 - t) + br * t) / 255;
    uint8_t g = (ag * (255 - t) + bg * t) / 255;
    uint8_t bl = (ab * (255 - t) + bb * t) / 255;
    return (r << 11) | (g << 5) | bl;
}

/* Blit the sprite with a fade multiplier (0..255). */
static void draw_engraving(int cx, int cy, uint8_t brightness)
{
    auto &d = M5Cardputer.Display;
    int ox = cx - splash_w / 2;
    int oy = cy - splash_h / 2;
    for (int y = 0; y < splash_h; ++y) {
        int dy = oy + y;
        if (dy < 0 || dy >= SCR_H) continue;
        for (int x = 0; x < splash_w; ++x) {
            uint16_t c = splash_data[y * splash_w + x];
            if (c == splash_alpha) continue;
            int dx = ox + x;
            if (dx < 0 || dx >= SCR_W) continue;
            uint16_t out = (brightness == 255) ? c : blend565(0x0000, c, brightness);
            d.drawPixel(dx, dy, out);
        }
    }
}

/* Horizontal scanline sweep — one bright line races across the
 * engraving for a "materializing" effect. */
static void draw_scanline(int y, uint16_t color)
{
    auto &d = M5Cardputer.Display;
    if (y < 0 || y >= SCR_H) return;
    d.drawFastHLine(0, y, SCR_W, color);
}

void ui_splash(void)
{
    auto &d = M5Cardputer.Display;
    d.fillScreen(0x0000);

    int cx = SCR_W / 2;
    int cy = 60;  /* engraving centered upper half */

    /* ---- Phase 1: engraving fade-in with scanline sweep ---- */
    for (int f = 0; f <= 30; ++f) {
        uint32_t frame_start = millis();
        uint8_t bright = (uint8_t)(f * 255 / 30);

        /* Full-screen clear each frame for clean fade. */
        d.fillScreen(0x0000);
        draw_engraving(cx, cy, bright);

        /* Scanline sweeps top→bottom over the sprite range. */
        int sy = (cy - splash_h / 2) + (f * splash_h / 30);
        draw_scanline(sy,     COL_MAGENTA_ACCENT);
        draw_scanline(sy + 1, COL_MAGENTA_DEEP);

        if (input_poll() != PK_NONE) goto idle;
        uint32_t e = millis() - frame_start;
        if (e < 30) delay(30 - e);
    }

    /* ---- Phase 2: title slides in from below with magenta glow ---- */
    {
        const char *title = "POSEIDON";
        d.setTextSize(2);
        int tw = d.textWidth(title) * 2;
        int tx = (SCR_W - tw) / 2;
        int final_y = 120;
        int start_y = SCR_H + 4;
        for (int f = 0; f <= 12; ++f) {
            int y = start_y + (final_y - start_y) * f / 12;
            /* Wipe just the title band. */
            d.fillRect(0, 118, SCR_W, 14, 0x0000);
            /* Glow layers. */
            d.setTextColor(COL_MAGENTA_DEEP, 0);
            d.setCursor(tx - 1, y); d.print(title);
            d.setCursor(tx + 1, y); d.print(title);
            d.setTextColor(COL_MAGENTA_ACCENT, 0);
            d.setCursor(tx, y); d.print(title);

            if (input_poll() != PK_NONE) goto idle;
            delay(22);
        }
        d.setTextSize(1);
    }

    /* ---- Phase 3: accent rule grows outward ---- */
    for (int w = 0; w <= 120; w += 8) {
        d.drawFastHLine((SCR_W - w) / 2, 132, w, COL_MAGENTA_ACCENT);
        if (input_poll() != PK_NONE) goto idle;
        delay(12);
    }

    /* "commander of the deep" subtitle under the title. */
    {
        const char *sub = "commander of the deep";
        d.setTextColor(0x7BEF, 0);
        int sw = d.textWidth(sub);
        d.setCursor((SCR_W - sw) / 2, 125);
        d.print(sub);
    }

idle:
    /* ---- Phase 4: ambient — matrix rain flanks the engraving ---- */
    while (true) {
        /* Re-draw the sprite periodically so rain doesn't paint over it
         * permanently (matrix rain only draws in its own region though). */
        ui_matrix_rain(0, 2, 50, 118, COL_MAGENTA_ACCENT);
        ui_matrix_rain(190, 2, 50, 118, COL_MAGENTA_DEEP);

        /* Subtle twinkle on the accent rule. */
        if ((esp_random() & 0xFF) < 30) {
            int x = 60 + (esp_random() % 120);
            d.drawPixel(x, 132, 0xFFFF);
            delay(30);
            d.drawPixel(x, 132, COL_MAGENTA_ACCENT);
        }

        if (input_poll() != PK_NONE) return;
        delay(60);
    }
}
