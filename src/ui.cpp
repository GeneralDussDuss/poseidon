/*
 * ui.cpp — drawing primitives. Nautical cyan-on-black aesthetic
 * with gradient bars, pulse indicator, and accent underlines.
 */
#include "ui.h"
#include "theme.h"
#include "input.h"
#include <stdarg.h>
#include <math.h>
#include <esp_system.h>
#include <esp_random.h>
#include <esp_heap_caps.h>

/* Legacy compat — these now pull from the active theme. */
#define COL_STATUS_BG  (theme().status_bg2)
#define COL_FOOTER_BG  (theme().footer_bg)
#define COL_RULE       (theme().rule)
#define COL_SEL_BG     (theme().sel_bg)

static uint32_t s_pulse_at = 0;
static bool     s_pulse_on = false;

static void fill_row(int y, int h, uint16_t c)
{
    M5Cardputer.Display.fillRect(0, y, SCR_W, h, c);
}

void ui_init(void)
{
    auto &d = M5Cardputer.Display;
    d.fillScreen(T_BG);
    d.setTextWrap(false, false);
    d.setTextSize(1);
}

static uint32_t s_last_clear = 0;

void ui_clear_body(void)
{
    uint32_t now = millis();
    /* Only actually fill on the first call or after a >300ms gap
     * (screen transition). Rapid redraws (<300ms) skip the fill
     * entirely — callers use ui_text() or setTextColor(fg, bg) to
     * overwrite in place. This kills the strobe globally. */
    if (now - s_last_clear > 300) {
        M5Cardputer.Display.fillRect(0, BODY_Y, SCR_W, BODY_H, T_BG);
    }
    s_last_clear = now;
}

/* Force a real clear — use for screen transitions, menu entry/exit. */
void ui_force_clear_body(void)
{
    M5Cardputer.Display.fillRect(0, BODY_Y, SCR_W, BODY_H, T_BG);
    s_last_clear = millis();
}

void ui_text(int x, int y, uint16_t fg, const char *fmt, ...)
{
    auto &d = M5Cardputer.Display;
    char buf[64];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    /* Overwrite with bg — fills the text bbox so no clear needed. */
    d.setTextColor(fg, T_BG);
    d.setCursor(x, y);
    d.print(buf);
    int tx = d.getCursorX();
    if (tx < SCR_W - 4) d.fillRect(tx, y, SCR_W - 4 - tx, 10, T_BG);
}

void ui_text_w(int x, int y, int w, uint16_t fg, const char *fmt, ...)
{
    auto &d = M5Cardputer.Display;
    char buf[64];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    d.fillRect(x, y, w, 10, T_BG);
    d.setTextColor(fg, T_BG);
    d.setCursor(x, y);
    d.print(buf);
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
    vgradient(0, 0, SCR_W, STATUS_H - 1, theme().status_bg, theme().status_bg2);

    uint32_t now = millis();
    if (now - s_pulse_at > 400) { s_pulse_at = now; s_pulse_on = !s_pulse_on; }
    d.fillCircle(5, 6, 2, s_pulse_on ? T_ACCENT : T_DIM);

    d.setTextColor(T_ACCENT, 0);
    d.setCursor(12, 2);
    d.print("POSEIDON");
    d.setTextColor(T_FG, 0);
    d.printf("  %s", radio ? radio : "idle");

    /* Right-aligned heap + extra status. */
    d.setTextColor(T_DIM, 0);
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
    d.drawFastHLine(0, STATUS_H - 1, SCR_W, T_ACCENT);
}

void ui_draw_footer(const char *hints)
{
    auto &d = M5Cardputer.Display;
    vgradient(0, FOOTER_Y + 1, SCR_W, FOOTER_H - 1, COL_FOOTER_BG, 0x0000);
    d.drawFastHLine(0, FOOTER_Y, SCR_W, COL_RULE);
    d.setTextColor(T_DIM, 0);
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
    d.fillRoundRect(x, y, w, h, 3, T_BG);
    d.drawRoundRect(x, y, w, h, 3, color);
    /* drop shadow */
    d.drawFastHLine(x + 2, y + h, w, 0x18C3);
    d.drawFastVLine(x + w, y + 2, h, 0x18C3);
    d.setTextColor(color, T_BG);
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
    d.fillRect(0, y, SCR_W, 11, T_BG);
    d.setTextColor(color, T_BG);
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
    if (!build_new) return;

    static uint16_t old_body[SCR_W * BODY_H];
    static uint16_t new_body[SCR_W * BODY_H];

    d.readRect(0, BODY_Y, SCR_W, BODY_H, old_body);
    ui_clear_body();
    build_new();
    d.readRect(0, BODY_Y, SCR_W, BODY_H, new_body);

    const int steps = 8;
    for (int s = 1; s <= steps; ++s) {
        int off = s * SCR_W / steps;
        if (direction > 0) {
            d.pushImage(-off,        BODY_Y, SCR_W, BODY_H, old_body);
            d.pushImage(SCR_W - off, BODY_Y, SCR_W, BODY_H, new_body);
        } else {
            d.pushImage(off,           BODY_Y, SCR_W, BODY_H, old_body);
            d.pushImage(-(SCR_W - off),BODY_Y, SCR_W, BODY_H, new_body);
        }
        delay(18);
    }
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
            d.setTextColor(T_FG, 0x0000);
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
        d.drawCircle(cx, cy, r, T_BG);
    }
}

/* ---- radial waves + arcs (ported from Evil-Cardputer) ---- */

static inline uint16_t hsv565(uint16_t hue, uint8_t sat, uint8_t val)
{
    /* Hue 0..360 → sector. Quick HSV→RGB565. */
    uint8_t region = (hue / 60) % 6;
    uint16_t f = (hue % 60) * 255 / 60;
    uint8_t p = (val * (255 - sat)) / 255;
    uint8_t q = (val * (255 - (sat * f) / 255)) / 255;
    uint8_t t = (val * (255 - (sat * (255 - f)) / 255)) / 255;
    uint8_t r = 0, g = 0, b = 0;
    switch (region) {
    case 0: r = val; g = t;   b = p;   break;
    case 1: r = q;   g = val; b = p;   break;
    case 2: r = p;   g = val; b = t;   break;
    case 3: r = p;   g = q;   b = val; break;
    case 4: r = t;   g = p;   b = val; break;
    case 5: r = val; g = p;   b = q;   break;
    }
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

void ui_waves(int cx, int cy, int max_radius, uint16_t base_color)
{
    auto &d = M5Cardputer.Display;
    uint32_t t = millis() / 16;  /* logical tick */

    /* Central glow (3 concentric disks). */
    for (int i = 3; i >= 1; --i) {
        uint8_t v = 40 * i;
        d.fillCircle(cx, cy, 10 + i * 3, hsv565(280, 200, v));
    }
    d.fillCircle(cx, cy, 4, 0xFFFF);

    /* 3 expanding ring waves with hue drift. */
    for (int i = 0; i < 3; ++i) {
        float k = ((t * 0.015f) + i * 0.33f);
        k -= (int)k;  /* wrap 0..1 */
        float e = -0.5f * (cosf(3.14159f * k) - 1.0f);  /* ease-in-out */
        int r = (int)(e * max_radius);
        if (r <= 0) continue;
        for (int j = 0; j < 3; ++j) {
            int rr = r - j;
            if (rr <= 0) continue;
            uint16_t hue = ((uint16_t)(280 + 30 * sinf(t * 0.01f + i))) % 360;
            uint8_t val = (uint8_t)max(30, 220 - j * 40);
            d.drawCircle(cx, cy, rr, hsv565(hue, 220, val));
        }
    }
    (void)base_color;

    /* Two sweeping arcs at fixed radii. */
    float sweep = t * 0.05f;
    for (int a = 0; a < 2; ++a) {
        float base = sweep + a * 2.1f;
        int r = 16 + a * 14;
        float start = base;
        float end   = base + 1.4f;
        for (float ang = start; ang <= end; ang += 0.08f) {
            int x = cx + (int)(cosf(ang) * r);
            int y = cy + (int)(sinf(ang) * r);
            uint8_t v = 180 + (int)(50 * sinf(ang - (start+end)*0.5f));
            d.drawPixel(x, y, hsv565(210, 200, v));
        }
    }
}

/* Color blend used by radar + others. */
static uint16_t blend565(uint16_t a, uint16_t b, uint8_t t)
{
    uint8_t ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    uint8_t br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
    uint8_t r = (ar * (255 - t) + br * t) / 255;
    uint8_t g = (ag * (255 - t) + bg * t) / 255;
    uint8_t bl = (ab * (255 - t) + bb * t) / 255;
    return (r << 11) | (g << 5) | bl;
}

/* ---- radar sweep ----
 * Rotating line with phosphor fade, outer grid, and occasional contact
 * blips that fade over time. */
struct radar_blip_t { float a; int r; uint32_t when; };
#define RADAR_BLIPS 6
static radar_blip_t s_blips[RADAR_BLIPS] = {0};

void ui_radar(int cx, int cy, int radius, uint16_t color)
{
    auto &d = M5Cardputer.Display;
    static float s_angle = 0;
    s_angle += 0.12f;
    if (s_angle > 6.28318f) s_angle -= 6.28318f;

    /* Outer ring + cross. */
    d.drawCircle(cx, cy, radius, 0x0420);
    d.drawCircle(cx, cy, radius * 2 / 3, 0x0320);
    d.drawCircle(cx, cy, radius / 3, 0x0220);
    d.drawFastHLine(cx - radius, cy, radius * 2, 0x0420);
    d.drawFastVLine(cx, cy - radius, radius * 2, 0x0420);

    /* Fading afterglow sweep — 30 degrees trailing. */
    for (int i = 0; i < 15; ++i) {
        float a = s_angle - i * 0.04f;
        int brightness = 255 - i * 17;
        uint16_t c = blend565(0x0000, color, (uint8_t)brightness);
        for (int r = 0; r < radius; r += 2) {
            int x = cx + (int)(cosf(a) * r);
            int y = cy + (int)(sinf(a) * r);
            d.drawPixel(x, y, c);
        }
    }

    /* Leading edge — bright white. */
    for (int r = 0; r < radius; ++r) {
        int x = cx + (int)(cosf(s_angle) * r);
        int y = cy + (int)(sinf(s_angle) * r);
        d.drawPixel(x, y, 0xFFFF);
    }

    /* Random blips. Guard against small radius: callers pass r=8 in
     * corner-radar slots, which would make (radius - 8) == 0 and
     * esp_random() % 0 is a hardware divide-by-zero panic. */
    int blip_range = radius - 8;
    if (blip_range < 1) blip_range = 1;
    if ((esp_random() & 0xFF) < 12) {
        int slot = esp_random() % RADAR_BLIPS;
        s_blips[slot].a = s_angle;
        s_blips[slot].r = 6 + (esp_random() % blip_range);
        s_blips[slot].when = millis();
    }
    for (int i = 0; i < RADAR_BLIPS; ++i) {
        uint32_t age = millis() - s_blips[i].when;
        if (age > 3000 || s_blips[i].when == 0) continue;
        uint8_t alpha = 255 - (age * 85 / 1000);
        int bx = cx + (int)(cosf(s_blips[i].a) * s_blips[i].r);
        int by = cy + (int)(sinf(s_blips[i].a) * s_blips[i].r);
        uint16_t bc = blend565(0x0000, 0xFFE0, alpha);
        d.fillCircle(bx, by, 2, bc);
    }
}

/* ---- hex data stream ---- */
void ui_hexstream(int x, int y, int w, int h, uint16_t color)
{
    auto &d = M5Cardputer.Display;
    static uint32_t s_phase = 0;
    s_phase += 2;

    /* Three rows, each scrolls at a different speed. */
    int rows = h / 10;
    if (rows > 5) rows = 5;
    for (int r = 0; r < rows; ++r) {
        int row_y = y + r * 10;
        int speed = 1 + (r & 1);
        uint32_t off = (s_phase * speed) / 3;
        /* Clear this row. */
        d.fillRect(x, row_y, w, 10, 0x0000);
        /* Draw hex pairs scrolling from right. */
        for (int col = 0; col < w / 18 + 2; ++col) {
            uint32_t seed = (r * 7919) ^ (col + off);
            seed = seed * 2654435761u;
            uint8_t hi = (seed >> 8) & 0xFF;
            int xp = x + w - (int)((col * 18 + (s_phase % 18) * speed) % (w + 18));
            if (xp < x - 12 || xp > x + w) continue;
            char buf[3];
            snprintf(buf, sizeof(buf), "%02X", hi);
            /* Occasional "fresh" byte highlights bright. */
            uint16_t col_col = (col == 0) ? 0xFFFF : color;
            d.setTextColor(col_col, 0x0000);
            d.setCursor(xp, row_y + 1);
            d.print(buf);
        }
    }
}

/* ---- glitch blocks ---- */
void ui_glitch(int x, int y, int w, int h)
{
    auto &d = M5Cardputer.Display;
    static uint32_t s_last = 0;
    /* Occasional bursts only. */
    if ((esp_random() & 0xFF) > 40) {
        /* No glitch this frame — clear just in case of residue. */
        if (millis() - s_last > 100) return;
    }
    s_last = millis();
    static const uint16_t glitch_cols[] = {
        0xF81F, 0x07FF, 0xFFE0, 0xF800, 0x07E0, 0x001F, 0xFFFF
    };
    for (int i = 0; i < 3; ++i) {
        int sy = y + (esp_random() % h);
        int sh = 1 + (esp_random() % 4);
        int sx = x + (esp_random() % (w / 2));
        int sw = (esp_random() % (w - (sx - x)));
        uint16_t c = glitch_cols[esp_random() % (sizeof(glitch_cols)/sizeof(*glitch_cols))];
        d.fillRect(sx, sy, sw, sh, c);
    }
}

/* ---- EQ bars ---- */
void ui_eq_bars(int x, int y, int bar_w, int bar_h_max, uint16_t color)
{
    auto &d = M5Cardputer.Display;
    /* 5 bars, each with a smoothed random target. */
    static uint8_t level[5] = { 4, 7, 3, 8, 5 };
    static uint8_t target[5] = { 8, 3, 9, 4, 7 };
    static uint32_t s_last = 0;
    if (millis() - s_last > 60) {
        s_last = millis();
        for (int i = 0; i < 5; ++i) {
            if (level[i] < target[i]) level[i]++;
            else if (level[i] > target[i]) level[i]--;
            if (level[i] == target[i]) target[i] = esp_random() % 10;
        }
    }
    for (int i = 0; i < 5; ++i) {
        int bh = level[i] * bar_h_max / 9;
        int bx = x + i * (bar_w + 2);
        /* Bar trail (faded max mark). */
        d.drawFastHLine(bx, y + bar_h_max - bh - 1, bar_w, 0x3003);
        /* Clear below. */
        d.fillRect(bx, y + bar_h_max - bh, bar_w, bh, color);
        /* Empty above. */
        d.fillRect(bx, y, bar_w, bar_h_max - bh, 0x0000);
    }
}

/* ---- magenta dashboard chrome ---- */

static uint32_t s_dash_flash_start = 0;
static uint32_t s_dash_last_flash  = 0;

/* Blend 5:6:5 color toward black by alpha (0..31). */
static inline uint16_t dim565(uint16_t c, uint8_t alpha)
{
    uint8_t r = (c >> 11) & 0x1F;
    uint8_t g = (c >>  5) & 0x3F;
    uint8_t b =  c        & 0x1F;
    r = (r * alpha) >> 5;
    g = (g * alpha) >> 5;
    b = (b * alpha) >> 5;
    return (r << 11) | (g << 5) | b;
}

void ui_dashboard_chrome(const char *title, bool flash_now)
{
    auto &d = M5Cardputer.Display;
    uint32_t now = millis();

    /* Rate-limit flashes: ignore new triggers within 900ms of the
     * previous one so the border doesn't strobe every frame. */
    if (flash_now && (now - s_dash_last_flash) > 900) {
        s_dash_flash_start = now;
        s_dash_last_flash  = now;
    }

    /* Hex storm backdrop. */
    ui_hexstream(0, BODY_Y + 4, SCR_W, BODY_H - 8, 0x4809);

    /* Smooth fade: peak at t=0, fades to nothing over 500 ms. */
    uint32_t dt = now - s_dash_flash_start;
    if (dt < 500) {
        /* alpha 31 → 0 linearly. */
        uint8_t alpha = 31 - (dt * 31) / 500;
        uint16_t c = dim565(T_ACCENT2, alpha);
        d.drawRect(0, BODY_Y, SCR_W, BODY_H, c);
    }

    /* Title bar. */
    d.setTextColor(T_ACCENT2, T_BG);
    d.setCursor(4, BODY_Y + 2);
    d.print(title);
    d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT2);

    /* Corner radar. */
    ui_radar(SCR_W - 14, BODY_Y + BODY_H - 14, 9, T_ACCENT2);
}

void ui_freq_bars(int x, int y, int bar_w, int bar_h_max)
{
    ui_eq_bars(x, y, bar_w, bar_h_max, T_ACCENT);
}

/* ---- full-screen action overlay ---- */
void ui_action_overlay(const char *headline, const char *subtitle,
                       action_anim_t bg, uint16_t color, uint32_t duration_ms)
{
    auto &d = M5Cardputer.Display;
    uint32_t start = millis();
    uint32_t last = 0;
    while (millis() - start < duration_ms) {
        uint32_t elapsed = millis() - start;
        /* First 150ms: fade-in. Last 300ms: fade-out. Otherwise full. */
        uint8_t alpha = 255;
        if (elapsed < 150) alpha = (uint8_t)(elapsed * 255 / 150);
        else if (duration_ms - elapsed < 300)
            alpha = (uint8_t)((duration_ms - elapsed) * 255 / 300);

        if (millis() - last > 50) {
            last = millis();
            d.fillScreen(0x0000);
            /* Background animation. */
            switch (bg) {
            case ACT_BG_RADAR:
                ui_radar(SCR_W / 2, SCR_H / 2, 50, color);
                break;
            case ACT_BG_WAVES:
                ui_waves(SCR_W / 2, SCR_H / 2, 60, color);
                break;
            case ACT_BG_MATRIX:
                ui_matrix_rain(0, 0, SCR_W, SCR_H, color);
                break;
            case ACT_BG_GLITCH:
                ui_glitch(0, 0, SCR_W, SCR_H);
                /* Diagonal scan lines for extra chaos. */
                for (int y = 0; y < SCR_H; y += 4) {
                    d.drawFastHLine(0, y, SCR_W, 0x0020);
                }
                break;
            }
            /* Headline: big, centered, magenta-glow outlined. */
            d.setTextSize(3);
            int hw = d.textWidth(headline) * 3;
            int hx = (SCR_W - hw) / 2;
            int hy = SCR_H / 2 - 16;
            uint16_t halo = blend565(0x0000, 0xF81F, alpha);
            uint16_t hot  = blend565(0x0000, color == 0xF81F ? 0xFFFF : color, alpha);
            /* 4-direction halo. */
            d.setTextColor(halo, 0);
            d.setCursor(hx - 2, hy); d.print(headline);
            d.setCursor(hx + 2, hy); d.print(headline);
            d.setCursor(hx, hy - 2); d.print(headline);
            d.setCursor(hx, hy + 2); d.print(headline);
            /* Hot core. */
            d.setTextColor(hot, 0);
            d.setCursor(hx, hy); d.print(headline);
            d.setTextSize(1);

            /* Subtitle below. */
            if (subtitle && *subtitle) {
                d.setTextColor(blend565(0x0000, 0xFFFF, alpha), 0);
                int sw = d.textWidth(subtitle);
                d.setCursor((SCR_W - sw) / 2, SCR_H / 2 + 16);
                d.print(subtitle);
            }

            /* Side brackets. */
            int bl = 10 + (int)(sinf(elapsed * 0.01f) * 4);
            d.drawFastHLine(4, SCR_H / 2 - 18, bl, color);
            d.drawFastVLine(4, SCR_H / 2 - 18, 4, color);
            d.drawFastHLine(4, SCR_H / 2 + 20, bl, color);
            d.drawFastVLine(4, SCR_H / 2 + 17, 4, color);
            d.drawFastHLine(SCR_W - 4 - bl, SCR_H / 2 - 18, bl, color);
            d.drawFastVLine(SCR_W - 5, SCR_H / 2 - 18, 4, color);
            d.drawFastHLine(SCR_W - 4 - bl, SCR_H / 2 + 20, bl, color);
            d.drawFastVLine(SCR_W - 5, SCR_H / 2 + 17, 4, color);
        }
        delay(20);
    }
}

/* ---- matrix rain ----
 * Column state: each column has a "head" y-position and speed.
 * Each render tick:
 *   - draws a fresh bright glyph at head
 *   - draws a fading trail above
 *   - advances head down; resets when off-screen
 * Glyph pool: printable katakana-ish via random printable chars.
 */
#define MATRIX_COLS 20
static int8_t  mx_head[MATRIX_COLS];      /* -1 = inactive */
static uint8_t mx_speed[MATRIX_COLS];
static char    mx_glyph[MATRIX_COLS];
static uint32_t mx_last_tick = 0;
static bool     mx_initialized = false;

void ui_matrix_rain(int x, int y, int w, int h, uint16_t color)
{
    auto &d = M5Cardputer.Display;
    /* Font cell: 6×8 default. Column spacing ~6px, row spacing ~8. */
    int col_w = 6;
    int row_h = 8;
    int n_cols = w / col_w;
    if (n_cols > MATRIX_COLS) n_cols = MATRIX_COLS;
    int rows = h / row_h;

    if (!mx_initialized) {
        for (int c = 0; c < MATRIX_COLS; ++c) {
            mx_head[c] = -1;
            mx_speed[c] = 0;
            mx_glyph[c] = '?';
        }
        mx_initialized = true;
    }

    uint32_t now = millis();
    bool advance = (now - mx_last_tick > 80);
    if (advance) mx_last_tick = now;

    for (int c = 0; c < n_cols; ++c) {
        if (mx_head[c] < 0) {
            /* Chance to spawn a new rain drop. */
            if ((esp_random() & 0xFF) < 10) {
                mx_head[c] = 0;
                mx_speed[c] = 1 + (esp_random() & 1);
            }
            continue;
        }
        /* Draw fading trail. */
        for (int t = 0; t < 5; ++t) {
            int ty = mx_head[c] - t;
            if (ty < 0 || ty >= rows) continue;
            uint16_t tcol = (t == 0) ? 0xFFFF : color;
            if (t == 1) tcol = color;
            else if (t == 2) tcol = 0x0440;  /* dim */
            else if (t >= 3) tcol = 0x0220;
            d.setTextColor(tcol, T_BG);
            d.setCursor(x + c * col_w, y + ty * row_h);
            char g = 0x21 + (char)(esp_random() % 0x5D);
            d.printf("%c", g);
        }
        if (advance) {
            /* Erase the tail row below the 5-char trail. */
            int erase_y = mx_head[c] - 5;
            if (erase_y >= 0 && erase_y < rows) {
                d.fillRect(x + c * col_w, y + erase_y * row_h, col_w, row_h, T_BG);
            }
            mx_head[c] += mx_speed[c];
            if (mx_head[c] >= rows + 5) {
                mx_head[c] = -1;  /* done */
                /* Also wipe any leftover pixels in this column. */
                d.fillRect(x + c * col_w, y, col_w, h, T_BG);
            }
        }
    }
}
