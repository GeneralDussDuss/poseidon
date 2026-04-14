/*
 * stubs.cpp — remaining placeholders for features not yet ported.
 *
 * Follow the wifi_scan.cpp pattern when replacing these:
 *   1. radio_switch() to the right domain
 *   2. Footer hints for visible hotkeys
 *   3. FreeRTOS task for blocking work
 *   4. Main loop: input_poll() + redraw, never block
 *   5. ESC returns
 */
#include "app.h"
#include "ui.h"
#include "input.h"
#include "radio.h"

static void stub_screen(const char *title, const char *detail)
{
    ui_clear_body();
    auto &d = M5Cardputer.Display;
    d.setTextColor(COL_ACCENT, COL_BG);
    d.setCursor(4, BODY_Y + 2);
    d.print(title);
    d.drawFastHLine(4, BODY_Y + 12, 120, COL_ACCENT);
    d.setTextColor(COL_DIM, COL_BG);
    d.setCursor(4, BODY_Y + 22);
    d.print(detail);
    d.setTextColor(COL_WARN, COL_BG);
    d.setCursor(4, BODY_Y + 44);
    d.print("stub -- implement me");
    ui_draw_footer("`=back");

    while (true) {
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) return;
    }
}

void feat_ble_hid(void)          { radio_switch(RADIO_BLE);  stub_screen("BAD-KB", "BLE HID keyboard attack"); }
void feat_ir_remote(void)        { stub_screen("IR REMOTE", "virtual Samsung remote"); }
void feat_file_browser(void)     { stub_screen("FILES", "SD card browser"); }
void feat_settings(void)         { stub_screen("SETTINGS", "config + preferences"); }

void feat_about(void)
{
    ui_clear_body();
    auto &d = M5Cardputer.Display;
    d.setTextColor(COL_ACCENT, COL_BG);
    d.setCursor(4, BODY_Y + 2);
    d.print("POSEIDON");
    d.drawFastHLine(4, BODY_Y + 12, 90, COL_ACCENT);
    d.setTextColor(COL_FG, COL_BG);
    d.setCursor(4, BODY_Y + 22); d.printf("v%s", POSEIDON_VERSION);
    d.setCursor(4, BODY_Y + 34); d.print("keyboard-first pentesting");
    d.setCursor(4, BODY_Y + 46); d.print("M5Stack Cardputer");
    d.setTextColor(COL_DIM, COL_BG);
    d.setCursor(4, BODY_Y + 64); d.print("github.com/../poseidon");
    d.setCursor(4, BODY_Y + 76); d.print("commander of the deep");
    ui_draw_footer("`=back");
    while (true) {
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) return;
    }
}
