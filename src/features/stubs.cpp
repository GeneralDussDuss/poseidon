/*
 * stubs.cpp — placeholder feature entries.
 *
 * Each feature here shows the "coming soon" screen and waits for ESC.
 * Replace one at a time, following the wifi_scan.cpp pattern:
 *
 *   1. radio_switch() to the right domain
 *   2. Set footer hints so the user knows which keys do what
 *   3. Spawn any blocking work on a FreeRTOS task
 *   4. Main loop: input_poll() + redraw, never block
 *   5. ESC returns, menu resumes
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
    d.setCursor(4, BODY_Y + 4);
    d.print(title);
    d.setTextColor(COL_DIM, COL_BG);
    d.setCursor(4, BODY_Y + 22);
    d.print(detail);
    d.setTextColor(COL_WARN, COL_BG);
    d.setCursor(4, BODY_Y + 44);
    d.print("stub — implement me");
    ui_draw_footer("ESC=back");

    while (true) {
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) return;
    }
}

void feat_wifi_deauth(void)      { radio_switch(RADIO_WIFI); stub_screen("WIFI DEAUTH", "type BSSID or pick from scan"); }
void feat_wifi_portal(void)      { radio_switch(RADIO_WIFI); stub_screen("EVIL PORTAL", "captive portal phishing"); }
void feat_wifi_beacon_spam(void) { radio_switch(RADIO_WIFI); stub_screen("BEACON SPAM", "broadcast fake SSIDs"); }
void feat_ble_scan(void)         { radio_switch(RADIO_BLE);  stub_screen("BLE SCAN", "passive scan + type detect"); }
void feat_ble_spam(void)         { radio_switch(RADIO_BLE);  stub_screen("BLE SPAM", "Apple/Samsung/Windows popups"); }
void feat_ble_hid(void)          { radio_switch(RADIO_BLE);  stub_screen("BAD-KB", "BLE HID keyboard attack"); }
void feat_ir_tvbgone(void)       { stub_screen("TV-B-GONE", "kill nearby TVs"); }
void feat_ir_remote(void)        { stub_screen("IR REMOTE", "virtual Samsung remote"); }
void feat_file_browser(void)     { stub_screen("FILES", "SD card browser"); }
void feat_settings(void)         { stub_screen("SETTINGS", "config + preferences"); }

void feat_about(void)
{
    ui_clear_body();
    auto &d = M5Cardputer.Display;
    d.setTextColor(COL_ACCENT, COL_BG);
    d.setCursor(4, BODY_Y + 4);
    d.print("POSEIDON");
    d.setTextColor(COL_FG, COL_BG);
    d.setCursor(4, BODY_Y + 22);
    d.printf("v%s", POSEIDON_VERSION);
    d.setCursor(4, BODY_Y + 34);
    d.print("keyboard-first pentesting");
    d.setCursor(4, BODY_Y + 46);
    d.print("M5Stack Cardputer firmware");
    d.setTextColor(COL_DIM, COL_BG);
    d.setCursor(4, BODY_Y + 64);
    d.print("github.com/.../poseidon");
    d.setCursor(4, BODY_Y + 76);
    d.print("commander of the deep");
    ui_draw_footer("ESC=back");
    while (true) {
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) return;
    }
}
