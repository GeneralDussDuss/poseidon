/*
 * ui.cpp — drawing primitives. Nautical cyan-on-black aesthetic
 * with gradient bars, pulse indicator, and accent underlines.
 */
#include "ui.h"
#include "ui/plist.h"
#include "input.h"
#include "theme.h"
#include "input.h"
#include "c5_cmd.h"
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

#include <Preferences.h>

static bool s_big_text_loaded = false;
static bool s_big_text = false;

bool ui_big_text(void)
{
    if (!s_big_text_loaded) {
        Preferences p;
        if (p.begin("pui", true)) {
            s_big_text = p.getBool("bigtxt", false);
            p.end();
        }
        s_big_text_loaded = true;
    }
    return s_big_text;
}

void ui_big_text_set(bool on)
{
    s_big_text = on;
    s_big_text_loaded = true;
    Preferences p;
    if (p.begin("pui", false)) {
        p.putBool("bigtxt", on);
        p.end();
    }
}

static uint32_t s_pulse_at = 0;
static bool     s_pulse_on = false;

void ui_init(void)
{
    auto &d = M5Cardputer.Display;
    d.fillScreen(T_BG);
    d.setTextWrap(false, false);
    d.setTextSize(1);
}

static uint32_t s_last_clear = 0;
static uint32_t s_clear_count = 0;

void ui_clear_body(void)
{
    uint32_t now = millis();
    /* Suppress rapid-fire clears that cause strobe: if called more than
     * 3 times within 500ms, start skipping. This allows pickers (which
     * clear on each keypress, ~1-2Hz) to work while blocking 10-30Hz
     * redraw loops in live displays. */
    if (now - s_last_clear < 80) {
        s_clear_count++;
        if (s_clear_count > 4) return;  /* strobe — skip */
    } else {
        s_clear_count = 0;
    }
    M5Cardputer.Display.fillRect(0, BODY_Y, SCR_W, BODY_H, T_BG);
    s_last_clear = now;
}

/* Force a real clear — use for screen transitions, menu entry/exit.
 * Also invalidates the ui_draw_status cache because some screen
 * transitions follow a full-screen fill that wipes the status bar too. */
void ui_force_clear_body(void)
{
    M5Cardputer.Display.fillRect(0, BODY_Y, SCR_W, BODY_H, T_BG);
    s_last_clear = millis();
    ui_status_invalidate();
}

/* Screen entry: the ONLY correct way to begin a new screen.
 *
 * Adopted from Bruce, which drives this panel without any of the artefacts
 * POSEIDON hit. The rule is: exactly one atomic full-screen clear when a
 * screen is entered, and every later repaint paints its own background
 * opaquely. Nothing caches across regions and nothing skips a paint on the
 * belief that the screen already looks right.
 *
 * POSEIDON's previous model failed three separate ways because it violated
 * that rule: ui_clear_body() silently stops clearing after 4 calls in 500ms
 * (so scrolling the carousel smeared), ui_draw_status() skipped repaints
 * after full-screen overlays had wiped the bar (stale top rows), and region
 * arithmetic left bands nobody owned (stale bottom rows). A whole-screen
 * fill costs a few ms once per transition and makes all three impossible. */
void ui_screen_enter(void)
{
    M5Cardputer.Display.fillScreen(T_BG);
    /* Reset the strobe suppressor so the next legitimate body clear is not
     * counted against a burst that happened on the previous screen. */
    s_last_clear  = millis();
    s_clear_count = 0;
    ui_status_invalidate();
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
    /* h<=1 would divide by zero below (guru meditation on Xtensa). */
    if (h <= 0) return;
    if (h == 1) { d.drawFastHLine(x, y, w, top); return; }
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

/* Cache: redraw the status bar only when something visibly changed.
 * Every feature screen calls ui_draw_status() on its render tick (every
 * 250 ms or so), and the vgradient + text repaint was the single biggest
 * source of flicker. Heap is quantised to 4 KB so natural minor churn
 * doesn't force a redraw. The pulse dot still animates by redrawing
 * just its own 5x5 rect when toggled. */
static char     s_st_radio[24] = {0};
static char     s_st_extra[24] = {0};
static uint32_t s_st_heap_bucket = 0xFFFFFFFFu;
static int      s_st_c5_n = -1;
static bool     s_st_valid = false;

void ui_draw_status(const char *radio, const char *extra)
{
    auto &d = M5Cardputer.Display;

    const char *rr = radio ? radio : "idle";
    const char *ee = (extra && *extra) ? extra : "";
    /* 32 KB bucket: BLE/WiFi scan churn swings free heap by several KB
     * continuously; a 4 KB bucket flipped constantly and repainted the
     * status-bar gradient every tick (a slow top-bar flicker on every
     * live-scan screen). Coarser bucket = the bar only repaints on a
     * real, lasting heap shift. */
    uint32_t heap_bucket = (esp_get_free_heap_size() / 1024) / 32;
    int c5_n = c5_any_online() ? c5_peer_count() : 0;

    bool changed = !s_st_valid
                || strncmp(rr, s_st_radio, sizeof(s_st_radio)) != 0
                || strncmp(ee, s_st_extra, sizeof(s_st_extra)) != 0
                || heap_bucket != s_st_heap_bucket
                || c5_n != s_st_c5_n;

    /* Pulse dot animates independently. Only the 5x5 rect repaints. */
    uint32_t now = millis();
    if (now - s_pulse_at > 400) {
        s_pulse_at = now;
        s_pulse_on = !s_pulse_on;
        if (!changed) {
            d.fillCircle(5, 6, 2, s_pulse_on ? T_ACCENT : T_DIM);
        }
    }

    if (!changed) return;

    vgradient(0, 0, SCR_W, STATUS_H - 1, theme().status_bg, theme().status_bg2);
    d.fillCircle(5, 6, 2, s_pulse_on ? T_ACCENT : T_DIM);

    d.setTextColor(T_ACCENT, 0);
    d.setCursor(12, 2);
    d.print("POSEIDON");
    d.setTextColor(T_FG, 0);
    d.printf("  %s", rr);

    d.setTextColor(T_DIM, 0);
    char buf[32];
    uint32_t heap_kb = heap_bucket * 4;   /* quantised label */
    snprintf(buf, sizeof(buf), "%luK%s%s",
             (unsigned long)heap_kb,
             *ee ? "  " : "",
             *ee ? ee : "");
    int w = d.textWidth(buf);
    d.setCursor(SCR_W - w - 4, 2);
    d.print(buf);

    /* C5/TRIDENT satellite indicator — 6x8 "C5 xN" badge + green dot
     * at the far right of the status bar when one or more C5 satellites
     * are paired over ESP-NOW. Sits inside the gradient; re-drawn as
     * part of the cached state so flicker is zero. */
    if (c5_n > 0) {
        char badge[12];
        snprintf(badge, sizeof(badge), "C5 x%d", c5_n);
        int bw = d.textWidth(badge);
        int bx = SCR_W - w - bw - 12;
        if (bx > 80) {
            d.fillCircle(bx - 5, 6, 2, T_GOOD);
            d.setTextColor(T_GOOD, 0);
            d.setCursor(bx, 2);
            d.print(badge);
        }
    }

    /* Magenta divider line at the bottom of the status bar — the
     * second strategic magenta splash (title underline is the first).
     * Two parallel lines together give the cyberpunk edge-pop. */
    d.drawFastHLine(0, STATUS_H - 1, SCR_W, T_ACCENT2);

    strncpy(s_st_radio, rr, sizeof(s_st_radio) - 1); s_st_radio[sizeof(s_st_radio) - 1] = 0;
    strncpy(s_st_extra, ee, sizeof(s_st_extra) - 1); s_st_extra[sizeof(s_st_extra) - 1] = 0;
    s_st_heap_bucket = heap_bucket;
    s_st_c5_n        = c5_n;
    s_st_valid = true;
}

/* Invalidate the ui_draw_status cache — call this when navigating to a
 * new screen or whenever the status bar was overwritten by a full-body
 * redraw, so the next ui_draw_status() forces a fresh paint. */
void ui_status_invalidate(void) { s_st_valid = false; }

void ui_draw_footer(const char *hints)
{
#if defined(POSEIDON_BOARD_TEMBED)
    /* No footer on this board (FOOTER_H is 0). Every caller passes a
     * keyboard key legend, which is noise without a keyboard, and drawing
     * into a zero-height strip would corrupt the last body row. */
    (void)hints;
    return;
#else
    auto &d = M5Cardputer.Display;
    vgradient(0, FOOTER_Y + 1, SCR_W, FOOTER_H - 1, COL_FOOTER_BG, 0x0000);
    d.drawFastHLine(0, FOOTER_Y, SCR_W, COL_RULE);
    d.setTextColor(T_DIM, 0);
    d.setCursor(4, FOOTER_Y + 2);
    if (!hints) return;
    /* Bug 6 / repro 2026-06-06: clip overflow so feature hint strings
     * with 40+ chars (BLE scan, wifi scan, etc) don't run off the right
     * edge. Default font glyph is 6 px wide; usable width is SCR_W - 8
     * (4 px margin each side) ≈ 38 chars. Truncate with an ellipsis so
     * the operator sees there's more rather than thinking the menu
     * stops mid-hint. */
    static const int MAX_CHARS = (SCR_W - 8) / 6;
    size_t len = strlen(hints);
    if ((int)len <= MAX_CHARS) {
        d.print(hints);
    } else {
        char trunc[64];
        int n = MAX_CHARS - 1;
        if (n < 0) n = 0;
        if (n > (int)sizeof(trunc) - 2) n = (int)sizeof(trunc) - 2;
        memcpy(trunc, hints, n);
        trunc[n]     = '>';
        trunc[n + 1] = '\0';
        d.print(trunc);
    }
#endif /* !POSEIDON_BOARD_TEMBED */
}

void ui_toast(const char *msg, uint16_t color, uint32_t ms)
{
    auto &d = M5Cardputer.Display;
    uint8_t scale = ui_big_text() ? 2 : 1;
    /* Glyph base is 6x8 in the default font. At scale 2 it's 12x16.
     * Don't use d.textWidth() here because the size setter after the
     * width call would make the stored width stale. */
    int gw = 6 * scale, gh = 8 * scale;
    int tw = (int)strlen(msg) * gw;
    /* Clamp width so a long toast at 2x still fits the 240-wide screen. */
    int w = tw + 16;
    if (w > SCR_W - 4) w = SCR_W - 4;
    int h = gh + 12;
    int x = (SCR_W - w) / 2;
    int y = (SCR_H - h) / 2;
    d.fillRoundRect(x, y, w, h, 3, T_BG);
    d.drawRoundRect(x, y, w, h, 3, color);
    /* drop shadow */
    d.drawFastHLine(x + 2, y + h, w, 0x18C3);
    d.drawFastVLine(x + w, y + 2, h, 0x18C3);
    d.setTextColor(color, T_BG);
    d.setTextSize(scale);
    d.setCursor(x + (w - tw) / 2, y + (h - gh) / 2);
    d.print(msg);
    d.setTextSize(1);     /* restore — every other path assumes scale 1 */
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

    /* Previously two 54 KB framebuffers were static BSS — 108 KB of
     * permanent DRAM for an animation that runs ~200 ms per menu slide.
     * WiFi init needs ~50 KB and was failing ENOMEM on devices without
     * PSRAM. Malloc here, free after the animation. If alloc fails
     * (really tight heap), skip the animation and just redraw. */
    const size_t bytes = SCR_W * BODY_H * sizeof(uint16_t);
    uint16_t *old_body = (uint16_t *)malloc(bytes);
    uint16_t *new_body = (uint16_t *)malloc(bytes);
    if (!old_body || !new_body) {
        free(old_body);
        free(new_body);
        /* Fallback: no animation, just draw the new body. */
        ui_clear_body();
        build_new();
        return;
    }

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
    free(old_body);
    free(new_body);
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

    auto draw_banner = [&](int y) {
        d.fillRoundRect(bx, y, bw, bh, 4, 0x0000);
        d.drawRoundRect(bx, y, bw, bh, 4, color);
        d.drawRoundRect(bx + 1, y + 1, bw - 2, bh - 2, 3, color);
        d.setTextColor(color, 0x0000);
        d.setTextSize(2);
        int tw = d.textWidth(title);
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

    /* Capture behind area once so we can restore. Heap-alloc'd — a
     * static 19.2KB buffer sat in BSS even when notifications weren't
     * active, which contributed to WiFi.scanNetworks() ENOMEM on cold
     * boot. On OOM we skip the animation (draw + hold, no slide). */
    const size_t behind_sz = (size_t)SCR_W * 40;
    uint16_t *behind = (uint16_t *)malloc(behind_sz * sizeof(uint16_t));
    if (!behind) {
        draw_banner(final_y);
        delay(hold_ms);
        return;
    }
    d.readRect(0, STATUS_H, SCR_W, 40, behind);

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
    free(behind);
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

/* ---- shared scan / connect screens ---- */
void ui_scanning_indicator(const char *label, int found)
{
    auto &d = M5Cardputer.Display;
    /* Corner radar animates every call for smoothness. */
    ui_radar(SCR_W - 11, BODY_Y + 8, 6, 0x07FF);

    /* Throttle the text/dot animation so it does not flicker. */
    static uint32_t s_ms = 0;
    static int s_dots = 0;
    if (millis() - s_ms < 250) return;
    s_ms = millis();
    s_dots = (s_dots + 1) % 4;
    const char *tail = s_dots == 0 ? "   " : s_dots == 1 ? ".  "
                     : s_dots == 2 ? ".. " : "...";
    char buf[40];
    if (found >= 0) snprintf(buf, sizeof(buf), "%s%s %d", label, tail, found);
    else            snprintf(buf, sizeof(buf), "%s%s", label, tail);

    /* Draw in a fixed top-right strip, left of the corner radar. */
    int x = SCR_W - 128;
    d.fillRect(x, BODY_Y + 2, 108, 10, T_BG);
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(x, BODY_Y + 2);
    d.print(buf);
}

void ui_connecting_screen(const char *target)
{
    auto &d = M5Cardputer.Display;
    ui_clear_body();
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2);
    d.print("CONNECTING");
    d.drawFastHLine(4, BODY_Y + 12, 96, T_ACCENT);
    d.setTextColor(T_FG, T_BG);
    d.setCursor(4, BODY_Y + 30);
    d.printf("%.30s", target ? target : "");
    ui_spinner(SCR_W / 2, BODY_Y + 54, T_ACCENT);
    d.setTextColor(T_DIM, T_BG);
    d.setCursor(4, BODY_Y + 74);
    d.print("linking, please wait");
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

    /* Hex storm backdrop disabled — it repainted hex digits across the
     * whole body every frame, which made the dashboard visibly flash
     * every time a feature redrew its status text. Static chrome is
     * easier on the eyes and faster. Re-enable via ui_hexstream() in a
     * specific feature if you want it back in a smaller strip. */

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

    /* Corner radar (static sweep animation, self-clearing). */
    ui_radar(SCR_W - 14, BODY_Y + BODY_H - 14, 9, T_ACCENT2);
}

void ui_freq_bars(int x, int y, int bar_w, int bar_h_max)
{
    ui_eq_bars(x, y, bar_w, bar_h_max, T_ACCENT);
}

/* ---- full-screen action overlay ----
 * Two entry points share one implementation. The plain ui_action_overlay
 * forwards to the with-tick variant with a null callback so neither has
 * to duplicate the (sizeable) animation loop. POS-AUDIT-009 introduced
 * the tick path to keep captive-portal DNS + HTTP responsive while a
 * CRED-CAPTURED overlay is on screen. */
void ui_action_overlay(const char *headline, const char *subtitle,
                       action_anim_t bg, uint16_t color, uint32_t duration_ms)
{
    ui_action_overlay_with_tick(headline, subtitle, bg, color, duration_ms,
                                 nullptr, nullptr);
}

void ui_action_overlay_with_tick(const char *headline, const char *subtitle,
                                  action_anim_t bg, uint16_t color,
                                  uint32_t duration_ms,
                                  void (*tick_cb)(void *), void *cb_ctx)
{
    auto &d = M5Cardputer.Display;
    /* Full-screen canvas — drawing into RAM eliminates the per-frame
     * fillScreen flash that the panel previously made visible. ~64 KB
     * transient alloc; freed at exit. */
    M5Canvas canvas(&d);
    if (!canvas.createSprite(SCR_W, SCR_H)) {
        /* Alloc failed — fall back to direct draw on display (legacy
         * behavior, will flash but at least renders). */
        d.fillScreen(0x0000);
        d.setTextSize(3);
        int hw = d.textWidth(headline);
        d.setCursor((SCR_W - hw) / 2, SCR_H / 2 - 16);
        d.setTextColor(color, 0);
        d.print(headline);
        d.setTextSize(1);
        if (subtitle && *subtitle) {
            int sw = d.textWidth(subtitle);
            d.setCursor((SCR_W - sw) / 2, SCR_H / 2 + 16);
            d.print(subtitle);
        }
        /* POS-AUDIT-009: in the no-canvas fallback we still need to keep
         * the caller's background work (DNS+HTTP for portal) responsive
         * across the duration_ms window. Slice the wait. */
        uint32_t fb_start = millis();
        while (millis() - fb_start < duration_ms) {
            if (tick_cb) tick_cb(cb_ctx);
            delay(10);
        }
        return;
    }

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
            canvas.fillScreen(0x0000);

            /* Inlined canvas-friendly bg effects — simpler than the
             * standalone ui_radar/ui_waves/etc. helpers which draw to
             * the panel directly, but visually equivalent for the
             * action-overlay's brief moment. */
            switch (bg) {
            case ACT_BG_RADAR: {
                int cx = SCR_W / 2, cy = SCR_H / 2, R = 50;
                canvas.drawCircle(cx, cy, R, 0x0420);
                canvas.drawCircle(cx, cy, (R * 2) / 3, 0x0320);
                canvas.drawCircle(cx, cy, R / 3, 0x0220);
                canvas.drawFastHLine(cx - R, cy, 2 * R, 0x0420);
                canvas.drawFastVLine(cx, cy - R, 2 * R, 0x0420);
                float sa = elapsed * 0.008f;
                for (int i = 0; i < 12; ++i) {
                    float a = sa - i * 0.05f;
                    uint8_t br = 255 - i * 21;
                    uint16_t c = blend565(0x0000, color, br);
                    int x = cx + (int)(cosf(a) * R);
                    int y = cy + (int)(sinf(a) * R);
                    canvas.drawLine(cx, cy, x, y, c);
                }
                break;
            }
            case ACT_BG_WAVES: {
                int cx = SCR_W / 2, cy = SCR_H / 2;
                for (int i = 0; i < 3; ++i) {
                    float k = ((elapsed * 0.0015f) + i * 0.33f);
                    k -= (int)k;
                    int r = (int)(k * 60);
                    if (r > 0) canvas.drawCircle(cx, cy, r, blend565(0x0000, color, 255 - (uint8_t)(k * 200)));
                }
                canvas.fillCircle(cx, cy, 4, 0xFFFF);
                break;
            }
            case ACT_BG_MATRIX: {
                /* Quick rain — vertical streaks at random x. */
                for (int i = 0; i < 12; ++i) {
                    int x = (i * 23 + (int)(elapsed / 30)) % SCR_W;
                    int y = ((int)(elapsed / 8) + i * 17) % (SCR_H + 30) - 30;
                    for (int k = 0; k < 6; ++k) {
                        if (y + k < 0 || y + k >= SCR_H) continue;
                        canvas.drawPixel(x, y + k, blend565(0x0000, color, 255 - k * 35));
                    }
                }
                break;
            }
            case ACT_BG_GLITCH:
                /* Stripes + diagonal scan lines. */
                for (int i = 0; i < 5; ++i) {
                    int y = (esp_random() % SCR_H);
                    int h = 2 + (esp_random() % 4);
                    canvas.fillRect(0, y, SCR_W, h, blend565(0x0000, color, 80));
                }
                for (int y = 0; y < SCR_H; y += 4) {
                    canvas.drawFastHLine(0, y, SCR_W, 0x0020);
                }
                break;
            }

            /* Headline: big, centered, magenta-glow outlined. */
            canvas.setTextSize(3);
            int hw = canvas.textWidth(headline);
            int hx = (SCR_W - hw) / 2;
            int hy = SCR_H / 2 - 16;
            uint16_t halo = blend565(0x0000, 0xF81F, alpha);
            uint16_t hot  = blend565(0x0000, color == 0xF81F ? 0xFFFF : color, alpha);
            canvas.setTextColor(halo, 0);
            canvas.setCursor(hx - 2, hy); canvas.print(headline);
            canvas.setCursor(hx + 2, hy); canvas.print(headline);
            canvas.setCursor(hx, hy - 2); canvas.print(headline);
            canvas.setCursor(hx, hy + 2); canvas.print(headline);
            canvas.setTextColor(hot, 0);
            canvas.setCursor(hx, hy); canvas.print(headline);
            canvas.setTextSize(1);

            /* Subtitle. */
            if (subtitle && *subtitle) {
                canvas.setTextColor(blend565(0x0000, 0xFFFF, alpha), 0);
                int sw = canvas.textWidth(subtitle);
                canvas.setCursor((SCR_W - sw) / 2, SCR_H / 2 + 16);
                canvas.print(subtitle);
            }

            /* Side brackets. */
            int bl = 10 + (int)(sinf(elapsed * 0.01f) * 4);
            canvas.drawFastHLine(4, SCR_H / 2 - 18, bl, color);
            canvas.drawFastVLine(4, SCR_H / 2 - 18, 4, color);
            canvas.drawFastHLine(4, SCR_H / 2 + 20, bl, color);
            canvas.drawFastVLine(4, SCR_H / 2 + 17, 4, color);
            canvas.drawFastHLine(SCR_W - 4 - bl, SCR_H / 2 - 18, bl, color);
            canvas.drawFastVLine(SCR_W - 5, SCR_H / 2 - 18, 4, color);
            canvas.drawFastHLine(SCR_W - 4 - bl, SCR_H / 2 + 20, bl, color);
            canvas.drawFastVLine(SCR_W - 5, SCR_H / 2 + 17, 4, color);

            /* Atomic push — one DMA transfer, no visible mid-frame. */
            canvas.pushSprite(0, 0);
        }
        /* POS-AUDIT-009: tick the caller's background service (e.g.
         * captive-portal DNS + HTTP). Fires every ~20 ms — well inside
         * the typical 1-3 s probe-redirect timeout window of Android /
         * iOS captive-portal detection. */
        if (tick_cb) tick_cb(cb_ctx);
        delay(20);
    }

    canvas.deleteSprite();
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

/* ---- encoder-driven action picker (see ui.h) ---- */

int ui_action_menu(const char *title, const char *const *labels, int n)
{
    if (n <= 0) return -1;

    auto &d = M5Cardputer.Display;

    /* Compact popup drawn OVER the current screen rather than replacing it:
     * the operator keeps sight of what they were looking at, which matters
     * when the actions apply to a specific target on the screen behind. */
    const int row_h   = 12;
    const int pad     = 4;
    const int hdr_h   = 12;
    const int max_vis = 5;                       /* scrolls beyond this */
    const int vis     = (n < max_vis) ? n : max_vis;

    int w = SCR_W * 3 / 5;
    if (w < 120) w = 120;
    if (w > SCR_W - 16) w = SCR_W - 16;
    const int h  = hdr_h + vis * row_h + pad * 2;
    const int x  = (SCR_W - w) / 2;
    const int y  = (SCR_H - h) / 2;

    plist_t m;
    plist_init(&m, n, vis);

    bool frame = true;
    for (;;) {
        if (frame) {
            /* Drop shadow then panel, so it reads as floating above. */
            d.fillRoundRect(x + 2, y + 2, w, h, 4, T_BG);
            d.fillRoundRect(x, y, w, h, 4, T_BG);
            d.drawRoundRect(x, y, w, h, 4, T_ACCENT);
            d.fillRect(x + 1, y + 1, w - 2, hdr_h - 2, T_ACCENT);
            d.setTextSize(1);
            d.setTextColor(T_BG, T_ACCENT);
            d.setCursor(x + 5, y + 2);
            d.print(title ? title : "ACTIONS");
            frame = false;
        }

        /* Rows repaint opaquely every pass — nothing cached, nothing stale. */
        for (int r = 0; r < vis; r++) {
            const int idx = m.top + r;
            if (idx >= n) break;
            const int ry  = y + hdr_h + pad + r * row_h;
            const bool on = (idx == m.sel);
            d.fillRect(x + 3, ry, w - 6, row_h, on ? T_SEL_BG : T_BG);
            d.setTextSize(1);
            d.setTextColor(on ? T_ACCENT : T_FG, on ? T_SEL_BG : T_BG);
            d.setCursor(x + 7, ry + 2);
            d.print(labels[idx]);
        }
        /* Scroll ticks on the right edge when the list overflows. */
        if (n > vis) {
            const int bar_h = (h - hdr_h - pad * 2) * vis / n;
            const int bar_y = y + hdr_h + pad +
                              (h - hdr_h - pad * 2) * m.top / n;
            d.fillRect(x + w - 3, y + hdr_h + pad, 2, h - hdr_h - pad * 2, T_BG);
            d.fillRect(x + w - 3, bar_y, 2, bar_h > 3 ? bar_h : 3, T_ACCENT2);
        }

        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(10); continue; }
        if (k == PK_ESC)   return -1;
        if (k == PK_ENTER) return m.sel;
        if (k == PK_UP   || k == ';') { plist_move(&m, -1); }
        if (k == PK_DOWN || k == '.') { plist_move(&m,  1); }
    }
}


/* See ui.h. Board-neutral by design: on a keyboard board the encoder gestures
 * never arrive, so this is a no-op and the letter bindings work as before. */
int ui_encoder_actions(uint16_t &k, const char *title,
                       const char *const *labels, const char *keys, int n,
                       bool want_enter)
{
#if defined(POSEIDON_BOARD_TEMBED)
    const bool gesture = (k == PK_ACTIONS) || (want_enter && k == PK_ENTER);
    if (!gesture) return 0;
    const int pick = ui_action_menu(title, labels, n);
    if (pick < 0 || pick >= n) return -1;
    k = (uint16_t)(uint8_t)keys[pick];
    return 1;
#else
    (void)k; (void)title; (void)labels; (void)keys; (void)n; (void)want_enter;
    return 0;
#endif
}
