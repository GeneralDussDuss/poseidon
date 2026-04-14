/*
 * ui.cpp — drawing primitives.
 */
#include "ui.h"
#include <stdarg.h>
#include <esp_system.h>

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

void ui_draw_status(const char *radio, const char *extra)
{
    auto &d = M5Cardputer.Display;
    fill_row(0, STATUS_H, 0x1082);  /* very dark blue */
    d.setTextColor(COL_ACCENT, 0x1082);
    d.setCursor(4, 2);
    d.printf("POSEIDON  %s", radio ? radio : "idle");

    d.setTextColor(COL_DIM, 0x1082);
    char buf[32];
    uint32_t heap_kb = esp_get_free_heap_size() / 1024;
    snprintf(buf, sizeof(buf), "%luK %s", (unsigned long)heap_kb,
             extra ? extra : "");
    int w = d.textWidth(buf);
    d.setCursor(SCR_W - w - 4, 2);
    d.print(buf);

    d.drawFastHLine(0, STATUS_H - 1, SCR_W, COL_DIM);
}

void ui_draw_footer(const char *hints)
{
    auto &d = M5Cardputer.Display;
    fill_row(FOOTER_Y, FOOTER_H, 0x0841);  /* dark grey */
    d.drawFastHLine(0, FOOTER_Y, SCR_W, COL_DIM);
    d.setTextColor(COL_DIM, 0x0841);
    d.setCursor(4, FOOTER_Y + 1);
    if (hints) d.print(hints);
}

void ui_toast(const char *msg, uint16_t color, uint32_t ms)
{
    auto &d = M5Cardputer.Display;
    int w = d.textWidth(msg) + 12;
    int x = (SCR_W - w) / 2;
    int y = SCR_H / 2 - 10;
    d.fillRect(x, y, w, 18, COL_BG);
    d.drawRect(x, y, w, 18, color);
    d.setTextColor(color, COL_BG);
    d.setCursor(x + 6, y + 5);
    d.print(msg);
    delay(ms);
}

void ui_splash(void)
{
    auto &d = M5Cardputer.Display;
    d.fillScreen(COL_BG);
    d.setTextColor(COL_ACCENT, COL_BG);
    d.setCursor(50, 40);
    d.setTextSize(2);
    d.print("POSEIDON");
    d.setTextSize(1);
    d.setTextColor(COL_DIM, COL_BG);
    d.setCursor(40, 70);
    d.print("commander of the deep");
    d.setCursor(80, 100);
    d.setTextColor(COL_MAGENTA, COL_BG);
    d.printf("v%s", POSEIDON_VERSION);
    d.setTextColor(COL_FG, COL_BG);
    d.setCursor(60, 120);
    d.print("press any key");
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
