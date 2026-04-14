/*
 * ui.cpp — drawing primitives. Nautical cyan-on-black aesthetic
 * with gradient bars, pulse indicator, and accent underlines.
 */
#include "ui.h"
#include "input.h"
#include <stdarg.h>
#include <math.h>
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

/* Splash moved to splash.cpp — procedural caustics, metallic trident
 * sprite, title bloom, scanline sweep. See ui_splash() there. */

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

/* ==================== animations ==================== */

/* Slide transition: old body fades-left while new slides in from the
 * right. Uses a pair of full-body captures via pushImage read-back.
 * ~180ms total, 8 frames. */
void ui_slide_transition(ui_draw_fn build_new, int direction)
{
    auto &d = M5Cardputer.Display;
    if (!build_new) { /* nothing to do */ return; }

    /* Capture the current body area into a scratch buffer. */
    static uint16_t old_body[SCR_W * BODY_H];
    d.readRect(0, BODY_Y, SCR_W, BODY_H, old_body);

    /* Let the caller paint the new body into the real framebuffer. */
    ui_clear_body();
    build_new();

    /* Snapshot the new body too so we can blit mixed strips. */
    static uint16_t new_body[SCR_W * BODY_H];
    d.readRect(0, BODY_Y, SCR_W, BODY_H, new_body);

    /* 8-step slide. direction=+1 pushes old left, new in from right. */
    const int steps = 8;
    for (int s = 1; s <= steps; ++s) {
        int off = s * SCR_W / steps;
        if (direction > 0) {
            /* Old slides left by `off`; new comes in at x = SCR_W - off. */
            d.pushImage(-off,        BODY_Y, SCR_W, BODY_H, old_body);
            d.pushImage(SCR_W - off, BODY_Y, SCR_W, BODY_H, new_body);
        } else {
            d.pushImage(off,           BODY_Y, SCR_W, BODY_H, old_body);
            d.pushImage(-(SCR_W - off),BODY_Y, SCR_W, BODY_H, new_body);
        }
        delay(18);
    }
    /* Settle on the final frame. */
    d.pushImage(0, BODY_Y, SCR_W, BODY_H, new_body);
}

/* Spinner: rotating trident silhouette. Drawn as a small 3-tine shape
 * rotated in 45° steps. */
void ui_spinner(int cx, int cy, uint16_t color)
{
    auto &d = M5Cardputer.Display;
    uint32_t t = millis() / 100;
    int phase = (int)(t & 7);

    /* 8 phases — draw lines with one selected tine highlighted. */
    int r = 8;
    for (int i = 0; i < 8; ++i) {
        float a = (i * 3.14159f / 4.0f) + (t * 0.05f);
        int x = cx + (int)(cosf(a) * r);
        int y = cy + (int)(sinf(a) * r);
        uint16_t c = (i == phase) ? 0xFFFF : color;
        /* dim tail trail */
        if (i == ((phase - 1 + 8) & 7)) c = color;
        d.fillCircle(x, y, i == phase ? 2 : 1, c);
    }
    /* Central hub */
    d.fillCircle(cx, cy, 2, color);
}

/* Banner slide-in from the top edge. 8 frames down, hold, 8 frames up. */
void ui_notify_slide(const char *title, const char *sub,
                     uint16_t color, uint32_t hold_ms)
{
    auto &d = M5Cardputer.Display;
    const int bw = SCR_W - 12;
    const int bh = 34;
    const int bx = 6;
    const int final_y = STATUS_H + 4;

    /* Capture behind area once so we can restore. */
    static uint16_t behind[SCR_W * 40];
    d.readRect(0, STATUS_H, SCR_W, 40, behind);

    auto draw_banner = [&](int y) {
        d.fillRoundRect(bx, y, bw, bh, 4, 0x0000);
        d.drawRoundRect(bx, y, bw, bh, 4, color);
        d.drawRoundRect(bx + 1, y + 1, bw - 2, bh - 2, 3, color);
        d.setTextColor(color, 0x0000);
        d.setTextSize(2);
        int tw = d.textWidth(title) * 2;
        d.setCursor(bx + (bw - tw) / 2, y + 3);
        d.print(title);
        d.setTextSize(1);
        if (sub && *sub) {
            int sw = d.textWidth(sub);
            d.setTextColor(COL_FG, 0x0000);
            d.setCursor(bx + (bw - sw) / 2, y + 22);
            d.print(sub);
        }
    };

    /* Slide down. */
    for (int s = 1; s <= 8; ++s) {
        int y = -bh + (final_y + bh) * s / 8;
        d.pushImage(0, STATUS_H, SCR_W, 40, behind);
        draw_banner(y);
        delay(18);
    }
    delay(hold_ms);
    /* Slide up. */
    for (int s = 7; s >= 0; --s) {
        int y = -bh + (final_y + bh) * s / 8;
        d.pushImage(0, STATUS_H, SCR_W, 40, behind);
        draw_banner(y);
        delay(18);
    }
    /* Restore. */
    d.pushImage(0, STATUS_H, SCR_W, 40, behind);
}

/* Ripple: expanding ring, 6 frames at 20ms = 120ms total. */
void ui_ripple(int cx, int cy, uint16_t color)
{
    auto &d = M5Cardputer.Display;
    for (int r = 3; r < 24; r += 3) {
        d.drawCircle(cx, cy, r, color);
        delay(20);
        d.drawCircle(cx, cy, r, COL_BG);
    }
}
