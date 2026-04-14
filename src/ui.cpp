/*
 * ui.cpp — drawing primitives. Nautical cyan-on-black aesthetic
 * with gradient bars, pulse indicator, and accent underlines.
 */
#include "ui.h"
#include "input.h"
#include <stdarg.h>
#include <esp_system.h>

/* Palette refinements — deeper blacks, sharper accents. */
#define COL_STATUS_BG  0x0841  /* very dark blue-green */
#define COL_FOOTER_BG  0x1082  /* deep slate */
#define COL_RULE       0x2124  /* muted cyan rule */
#define COL_SEL_BG     0x18C7  /* selected row dark cyan */

static uint32_t s_pulse_at = 0;
static bool     s_pulse_on = false;

static void fill_row(int y, int h, uint16_t c)
{
    M5Cardputer.Display.fillRect(0, y, SCR_W, h, c);
}

void ui_init(void)
{
    auto &d = M5Cardputer.Display;
    d.fillScreen(COL_BG);
    d.setTextWrap(false, false);
    d.setTextSize(1);
}

void ui_clear_body(void)
{
    M5Cardputer.Display.fillRect(0, BODY_Y, SCR_W, BODY_H, COL_BG);
}

/* Draw a vertical gradient fill between two colors. */
static void vgradient(int x, int y, int w, int h, uint16_t top, uint16_t bot)
{
    auto &d = M5Cardputer.Display;
    /* Decompose to RGB565 components. */
    uint8_t tr = (top >> 11) & 0x1F, tg = (top >> 5) & 0x3F, tb = top & 0x1F;
    uint8_t br = (bot >> 11) & 0x1F, bg = (bot >> 5) & 0x3F, bb = bot & 0x1F;
    for (int i = 0; i < h; ++i) {
        uint8_t r = tr + (br - tr) * i / (h - 1);
        uint8_t g = tg + (bg - tg) * i / (h - 1);
        uint8_t b = tb + (bb - tb) * i / (h - 1);
        d.drawFastHLine(x, y + i, w, (uint16_t)((r << 11) | (g << 5) | b));
    }
}

void ui_draw_status(const char *radio, const char *extra)
{
    auto &d = M5Cardputer.Display;
    vgradient(0, 0, SCR_W, STATUS_H - 1, 0x20A5, COL_STATUS_BG);  /* teal → dark */

    /* Live pulse dot — cycles so users can see the UI is alive. */
    uint32_t now = millis();
    if (now - s_pulse_at > 400) { s_pulse_at = now; s_pulse_on = !s_pulse_on; }
    d.fillCircle(5, 6, 2, s_pulse_on ? COL_ACCENT : COL_DIM);

    /* Title + current radio domain. */
    d.setTextColor(COL_ACCENT, 0);
    d.setCursor(12, 2);
    d.print("POSEIDON");
    d.setTextColor(COL_FG, 0);
    d.printf("  %s", radio ? radio : "idle");

    /* Right-aligned heap + extra status. */
    d.setTextColor(COL_DIM, 0);
    char buf[32];
    uint32_t heap_kb = esp_get_free_heap_size() / 1024;
    snprintf(buf, sizeof(buf), "%luK%s%s",
             (unsigned long)heap_kb,
             (extra && *extra) ? "  " : "",
             (extra && *extra) ? extra : "");
    int w = d.textWidth(buf);
    d.setCursor(SCR_W - w - 4, 2);
    d.print(buf);

    /* Accent rule. */
    d.drawFastHLine(0, STATUS_H - 1, SCR_W, COL_ACCENT);
}

void ui_draw_footer(const char *hints)
{
    auto &d = M5Cardputer.Display;
    vgradient(0, FOOTER_Y + 1, SCR_W, FOOTER_H - 1, COL_FOOTER_BG, 0x0000);
    d.drawFastHLine(0, FOOTER_Y, SCR_W, COL_RULE);
    d.setTextColor(COL_DIM, 0);
    d.setCursor(4, FOOTER_Y + 2);
    if (hints) d.print(hints);
}

void ui_toast(const char *msg, uint16_t color, uint32_t ms)
{
    auto &d = M5Cardputer.Display;
    int tw = d.textWidth(msg);
    int w = tw + 16;
    int h = 20;
    int x = (SCR_W - w) / 2;
    int y = (SCR_H - h) / 2;
    d.fillRoundRect(x, y, w, h, 3, COL_BG);
    d.drawRoundRect(x, y, w, h, 3, color);
    /* drop shadow */
    d.drawFastHLine(x + 2, y + h, w, 0x18C3);
    d.drawFastVLine(x + w, y + 2, h, 0x18C3);
    d.setTextColor(color, COL_BG);
    d.setCursor(x + (w - tw) / 2, y + 6);
    d.print(msg);
    delay(ms);
}

/* ---- splash ---- */

/* Trident glyph — drawn filled & more detailed than before. */
static void draw_trident(int cx, int cy, int scale, uint16_t color)
{
    auto &d = M5Cardputer.Display;
    int s = scale;
    /* shaft — 2px thick */
    d.fillRect(cx - 1, cy - 9 * s, 2, 20 * s, color);
    /* crown bar */
    d.fillRect(cx - 7 * s, cy - 9 * s, 14 * s + 2, 2, color);
    /* outer tines (curved-ish — zig with small steps) */
    for (int i = 0; i < 5 * s; ++i) {
        d.drawPixel(cx - 7 * s + i / 3, cy - 9 * s - i, color);
        d.drawPixel(cx + 7 * s - i / 3, cy - 9 * s - i, color);
    }
    /* center tine */
    d.fillRect(cx - 1, cy - 15 * s, 2, 6 * s, color);
    /* tine tips — little triangles */
    d.fillTriangle(cx - 8 * s + 1, cy - 13 * s, cx - 7 * s + 1, cy - 15 * s, cx - 5 * s, cy - 13 * s, color);
    d.fillTriangle(cx + 5 * s,     cy - 13 * s, cx + 7 * s - 1, cy - 15 * s, cx + 8 * s - 1, cy - 13 * s, color);
    d.fillTriangle(cx - 2,         cy - 15 * s, cx,             cy - 17 * s, cx + 2,         cy - 15 * s, color);
    /* grip rings at base */
    d.fillRect(cx - 3, cy + 7 * s,  6, 2, color);
    d.fillRect(cx - 3, cy + 9 * s,  6, 2, color);
    /* spear tip below grip */
    d.fillTriangle(cx - 2, cy + 11 * s, cx, cy + 13 * s, cx + 2, cy + 11 * s, color);
}

/* Draw a wavy horizon line across the screen. Phase lets us animate. */
static void draw_waves(int y0, uint16_t color, int phase)
{
    auto &d = M5Cardputer.Display;
    for (int x = 0; x < SCR_W; ++x) {
        /* Sum of two sines for a less mechanical wave. */
        int p = (x + phase) * 6;
        int wave = 0;
        /* approx: sin via tiny lookup for speed */
        static const int8_t sin_t[16] = {
            0, 6, 11, 15, 16, 15, 11, 6, 0, -6, -11, -15, -16, -15, -11, -6
        };
        wave += sin_t[(p / 10) & 15] / 6;
        wave += sin_t[(p / 17 + 4) & 15] / 8;
        d.drawPixel(x, y0 + wave, color);
    }
}

/* Interpolate between two RGB565 colors. t in [0..255]. */
static uint16_t blend565(uint16_t a, uint16_t b, uint8_t t)
{
    uint8_t ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    uint8_t br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
    uint8_t r = (ar * (255 - t) + br * t) / 255;
    uint8_t g = (ag * (255 - t) + bg * t) / 255;
    uint8_t bl = (ab * (255 - t) + bb * t) / 255;
    return (r << 11) | (g << 5) | bl;
}

/* Animated splash:
 *   - dark ocean horizon, animated waves
 *   - trident rises from below the waterline
 *   - title fades in with cyan→white pulse
 *   - subtitle slides in from the right
 * Runs for ~2.2 seconds then holds. input_poll() can break early.
 */
void ui_splash(void)
{
    auto &d = M5Cardputer.Display;
    d.fillScreen(COL_BG);

    /* Background: deep vertical gradient — sky down to abyss. */
    vgradient(0, 0, SCR_W, SCR_H, 0x0010, 0x0000);

    const int horizon_y = 95;  /* waterline */
    const int cx = SCR_W / 2;
    const int trident_final_y = 60;

    /* Phase 1: trident rises from below horizon. 0..40 frames. */
    for (int f = 0; f <= 40; ++f) {
        uint32_t frame_start = millis();

        /* Keep the top of the screen static and only redraw the
         * animation band to avoid full-screen flicker. */
        d.fillRect(0, 20, SCR_W, horizon_y - 20, 0x0000);

        /* Trident Y interpolates from below horizon to final position. */
        int trident_y = horizon_y + 30 - (f * (horizon_y + 30 - trident_final_y) / 40);
        /* Clip the portion above the waterline only so it looks like it's rising. */
        if (trident_y < horizon_y) {
            draw_trident(cx, trident_y, 1, COL_ACCENT);
        }

        /* Water: fill everything at/below horizon with dark band. */
        d.fillRect(0, horizon_y, SCR_W, SCR_H - horizon_y, 0x0000);

        /* Animated waves ripple across the horizon. */
        draw_waves(horizon_y,     COL_ACCENT, f * 4);
        draw_waves(horizon_y + 3, COL_RULE,   f * 6);
        draw_waves(horizon_y + 6, 0x1082,     f * 3);

        /* Ripples around the trident when it's partially submerged. */
        if (trident_y > horizon_y - 20 && trident_y < horizon_y + 20) {
            int ripple_r = (f % 10) * 2 + 4;
            d.drawEllipse(cx, horizon_y, ripple_r, ripple_r / 3, COL_DIM);
        }

        /* Starfield at top — flicker some pixels. */
        if ((f & 3) == 0) {
            for (int i = 0; i < 12; ++i) {
                int sx = (i * 997 + f * 17) % SCR_W;
                int sy = (i * 13) % 18;
                d.drawPixel(sx, sy + 2,
                    (i & 1) ? COL_DIM : COL_FG);
            }
        }

        /* Poll for early-out on keypress. */
        if (input_poll() != PK_NONE) return;

        /* Target 30ms/frame. */
        uint32_t e = millis() - frame_start;
        if (e < 30) delay(30 - e);
    }

    /* Phase 2: title fades in with color pulse. */
    for (int f = 0; f <= 20; ++f) {
        uint32_t frame_start = millis();

        /* Clear title band. */
        d.fillRect(0, 82, SCR_W, 30, 0x0000);

        uint8_t t = f * 255 / 20;
        uint16_t title_color = blend565(0x0000, COL_ACCENT, t);

        d.setTextColor(title_color, 0);
        d.setTextSize(2);
        const char *title = "POSEIDON";
        int tw = d.textWidth(title) * 2;
        d.setCursor((SCR_W - tw) / 2, 82);
        d.print(title);
        d.setTextSize(1);

        if (input_poll() != PK_NONE) return;
        uint32_t e = millis() - frame_start;
        if (e < 30) delay(30 - e);
    }

    /* Phase 3: subtitle + version + accent rule slide + hint. */
    const char *sub = "commander of the deep";
    int sw = M5Cardputer.Display.textWidth(sub);
    int final_x = (SCR_W - sw) / 2;
    for (int f = 0; f <= 14; ++f) {
        uint32_t frame_start = millis();
        d.fillRect(0, 106, SCR_W, 12, 0x0000);
        int x = SCR_W - (SCR_W - final_x) * f / 14;
        d.setTextColor(COL_DIM, 0);
        d.setCursor(x, 108);
        d.print(sub);
        if (input_poll() != PK_NONE) return;
        uint32_t e = millis() - frame_start;
        if (e < 25) delay(25 - e);
    }

    /* Accent rule growing outward. */
    for (int w = 0; w <= 80; w += 8) {
        d.drawFastHLine((SCR_W - w) / 2, 120, w, COL_ACCENT);
        if (input_poll() != PK_NONE) return;
        delay(15);
    }

    /* Version + hint. */
    d.setTextColor(COL_MAGENTA, 0);
    char vbuf[32];
    snprintf(vbuf, sizeof(vbuf), "v%s", POSEIDON_VERSION);
    int vw = d.textWidth(vbuf);
    d.setCursor((SCR_W - vw) / 2, 124);
    d.print(vbuf);

    /* Continuous wave animation while waiting for a keypress. */
    int phase = 0;
    while (true) {
        /* Only redraw the water band so the title stays crisp. */
        d.fillRect(0, 95, SCR_W, 10, 0x0000);
        draw_waves(95,  COL_ACCENT, phase);
        draw_waves(98,  COL_RULE,   phase * 2);
        draw_waves(101, 0x1082,     phase * 3);
        phase += 4;

        if (input_poll() != PK_NONE) return;
        delay(60);
    }
}

void ui_body_println(int row, uint16_t color, const char *fmt, ...)
{
    auto &d = M5Cardputer.Display;
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    int y = BODY_Y + 2 + row * 11;
    d.fillRect(0, y, SCR_W, 11, COL_BG);
    d.setTextColor(color, COL_BG);
    d.setCursor(4, y);
    d.print(buf);
}
