/*
 * ui.cpp — drawing primitives. Nautical cyan-on-black aesthetic
 * with gradient bars, pulse indicator, and accent underlines.
 */
#include "ui.h"
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

/* Tiny trident glyph drawn from primitives — no need for a bitmap. */
static void draw_trident(int cx, int cy, uint16_t color)
{
    auto &d = M5Cardputer.Display;
    /* shaft */
    d.drawFastVLine(cx, cy - 18, 36, color);
    d.drawFastVLine(cx - 1, cy - 18, 36, color);
    /* crown */
    d.drawFastHLine(cx - 12, cy - 18, 25, color);
    d.drawFastVLine(cx - 12, cy - 26, 8, color);
    d.drawFastVLine(cx + 12, cy - 26, 8, color);
    d.drawFastVLine(cx,      cy - 30, 12, color);
    d.drawPixel(cx - 12, cy - 27, color);
    d.drawPixel(cx + 12, cy - 27, color);
    /* grip */
    d.drawFastHLine(cx - 4, cy + 5,  9, color);
    d.drawFastHLine(cx - 4, cy + 10, 9, color);
}

void ui_splash(void)
{
    auto &d = M5Cardputer.Display;
    d.fillScreen(COL_BG);

    /* Horizon gradient background. */
    vgradient(0, 0, SCR_W, SCR_H, 0x0010, 0x0000);

    /* Trident — centered above title. */
    draw_trident(SCR_W / 2, 48, COL_ACCENT);

    /* Title bar. */
    d.setTextColor(COL_ACCENT, 0);
    d.setTextSize(2);
    const char *title = "POSEIDON";
    int tw = d.textWidth(title) * 2;  /* setTextSize affects width */
    d.setCursor((SCR_W - tw) / 2, 78);
    d.print(title);

    d.setTextSize(1);
    d.setTextColor(COL_DIM, 0);
    const char *sub = "commander of the deep";
    int sw = d.textWidth(sub);
    d.setCursor((SCR_W - sw) / 2, 100);
    d.print(sub);

    /* Accent line under subtitle. */
    d.drawFastHLine((SCR_W - 80) / 2, 110, 80, COL_ACCENT);

    /* Version + hint. */
    d.setTextColor(COL_MAGENTA, 0);
    char vbuf[32];
    snprintf(vbuf, sizeof(vbuf), "v%s", POSEIDON_VERSION);
    int vw = d.textWidth(vbuf);
    d.setCursor((SCR_W - vw) / 2, 118);
    d.print(vbuf);

    d.setTextColor(COL_FG, 0);
    const char *hint = "press any key";
    int hw = d.textWidth(hint);
    d.setCursor((SCR_W - hw) / 2, 127);
    d.print(hint);
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
