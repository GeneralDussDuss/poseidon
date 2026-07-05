/*
 * system_tools — saved WiFi connect, file browser, clock, settings.
 */
#include "app.h"
#include "../theme.h"
#include "ui.h"
#include "input.h"
#include "radio.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <SD.h>
#include "../sd_helper.h"
#include <Preferences.h>
#include <time.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

/* ========== Saved WiFi ========== */

static Preferences s_prefs;

void feat_wifi_connect(void)
{
    radio_switch(RADIO_WIFI);
    s_prefs.begin("poseidon", false);

    ui_clear_body();
    auto &d = M5Cardputer.Display;
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2); d.print("WIFI CONNECT");
    d.drawFastHLine(4, BODY_Y + 12, 100, T_ACCENT);
    d.setTextColor(T_FG, T_BG);
    String ssid = s_prefs.getString("ssid", "");
    String pass = s_prefs.getString("pass", "");

    if (ssid.length() > 0) {
        d.setCursor(4, BODY_Y + 22); d.printf("saved: %s", ssid.c_str());
        d.setCursor(4, BODY_Y + 34); d.print("[C] connect  [N] new  [X] forget");
    } else {
        d.setCursor(4, BODY_Y + 22); d.print("no saved network");
        d.setCursor(4, BODY_Y + 34); d.print("[N] add new");
    }
    ui_draw_footer("letter=go  `=back");

    while (true) {
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) { s_prefs.end(); return; }
        if ((k == 'n' || k == 'N')) {
            char s[33], p[65];
            if (!input_line("ssid:", s, sizeof(s))) continue;
            if (!input_line("password:", p, sizeof(p))) continue;
            s_prefs.putString("ssid", s);
            s_prefs.putString("pass", p);
            ssid = s; pass = p;
            ui_toast("saved", T_GOOD, 600);
            break;
        }
        if ((k == 'x' || k == 'X') && ssid.length() > 0) {
            s_prefs.remove("ssid");
            s_prefs.remove("pass");
            s_prefs.end();
            ui_toast("forgotten", T_WARN, 600);
            return;
        }
        if ((k == 'c' || k == 'C') && ssid.length() > 0) break;
    }

    /* Force-reset WiFi driver state.  A previous connect attempt can
     * leave the STA stuck in an infinite AUTH_EXPIRE retry loop — new
     * WiFi.begin() calls then fail with ESP_ERR_WIFI_STATE (0x3006)
     * because the driver thinks it's "already connecting".  We must
     * stop + restart the driver to break the loop. */
    WiFi.disconnect(true, true);
    esp_wifi_stop();
    delay(100);
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(false, false);
    delay(200);

    uint32_t heap = ESP.getFreeHeap();
    Serial.printf("[wifi] connect: ssid='%s' pass_len=%u\n",
                  ssid.c_str(), (unsigned)pass.length());
    Serial.printf("[wifi] mode=%d status=%d heap=%u\n",
                  WiFi.getMode(), WiFi.status(), (unsigned)heap);

    if (heap < 20000) {
        Serial.printf("[wifi] WARN: heap critically low (%u) — "
                      "connection may fail\n", (unsigned)heap);
        ui_toast("low heap — may fail", T_WARN, 1200);
    }

    WiFi.begin(ssid.c_str(), pass.c_str());
    ui_clear_body();
    d.setTextColor(T_WARN, T_BG);
    d.setCursor(4, BODY_Y + 2); d.printf("connecting to %s", ssid.c_str());

    uint32_t deadline = millis() + 15000;
    while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
        d.fillRect(0, BODY_Y + 22, SCR_W, 12, T_BG);
        d.setCursor(4, BODY_Y + 22);
        d.setTextColor(T_DIM, T_BG);
        d.printf("%ds", (int)((deadline - millis()) / 1000));
        if (input_poll() == PK_ESC) { WiFi.disconnect(); s_prefs.end(); return; }
        delay(250);
    }

    d.fillRect(0, BODY_Y + 22, SCR_W, 60, T_BG);
    Serial.printf("[wifi] result: status=%d (3=CONNECTED)\n", WiFi.status());
    if (WiFi.status() == WL_CONNECTED) {
        d.setTextColor(T_GOOD, T_BG);
        d.setCursor(4, BODY_Y + 22); d.print("CONNECTED");
        d.setTextColor(T_FG, T_BG);
        d.setCursor(4, BODY_Y + 34); d.printf("IP : %s", WiFi.localIP().toString().c_str());
        d.setCursor(4, BODY_Y + 46); d.printf("GW : %s", WiFi.gatewayIP().toString().c_str());
        d.setCursor(4, BODY_Y + 58); d.printf("DNS: %s", WiFi.dnsIP().toString().c_str());
    } else {
        d.setTextColor(T_BAD, T_BG);
        d.setCursor(4, BODY_Y + 22); d.print("FAILED");
    }
    s_prefs.end();
    ui_draw_footer("`=back");
    while (true) {
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) break;
    }
}

/* ========== File browser ========== */

#define FB_MAX 32
struct fb_entry_t { char name[32]; bool is_dir; uint32_t size; };
static fb_entry_t s_fb[FB_MAX];
static int        s_fb_count = 0;

static void fb_list(const char *path)
{
    s_fb_count = 0;
    File root = SD.open(path);
    if (!root || !root.isDirectory()) return;
    File f = root.openNextFile();
    while (f && s_fb_count < FB_MAX) {
        fb_entry_t &e = s_fb[s_fb_count++];
        strncpy(e.name, f.name(), sizeof(e.name) - 1);
        e.name[sizeof(e.name) - 1] = '\0';
        e.is_dir = f.isDirectory();
        e.size   = (uint32_t)f.size();
        f = root.openNextFile();
    }
}

void feat_file_browser(void)
{
    if (!sd_mount()) { ui_toast("SD mount fail", T_BAD, 1500); return; }
    char path[128] = "/poseidon";
    fb_list(path);
    int cursor = 0;

    auto draw = [&]() {
        auto &d = M5Cardputer.Display;
        ui_clear_body();
        d.setTextColor(T_ACCENT, T_BG);
        d.setCursor(4, BODY_Y + 2); d.printf("FILES  %s", path);
        d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT);
        if (s_fb_count == 0) {
            d.setTextColor(T_DIM, T_BG);
            d.setCursor(4, BODY_Y + 24);
            d.print("(empty)");
            return;
        }
        int rows = 9;
        int first = cursor - rows / 2;
        if (first < 0) first = 0;
        if (first + rows > s_fb_count) first = max(0, s_fb_count - rows);
        for (int r = 0; r < rows && first + r < s_fb_count; ++r) {
            const fb_entry_t &e = s_fb[first + r];
            int y = BODY_Y + 16 + r * 11;
            bool sel = (first + r == cursor);
            if (sel) d.fillRect(0, y - 1, SCR_W, 11, 0x18C7);
            d.setTextColor(e.is_dir ? T_ACCENT : T_FG, sel ? 0x18C7 : T_BG);
            d.setCursor(4, y);
            if (e.is_dir) d.printf("[DIR] %.24s", e.name);
            else          d.printf("%.24s  %luB", e.name, (unsigned long)e.size);
        }
    };

    draw();
    ui_draw_footer(";/.=move  ENTER=open  D=delete  `=up");
    while (true) {
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) {
            /* Go up if not at root. */
            char *slash = strrchr(path, '/');
            if (slash && slash != path) { *slash = '\0'; fb_list(path); cursor = 0; draw(); }
            else return;
        } else if (k == ';' || k == PK_UP)    { if (cursor > 0) cursor--; draw(); }
        else if (k == '.' || k == PK_DOWN)    { if (cursor + 1 < s_fb_count) cursor++; draw(); }
        else if (k == PK_ENTER && s_fb_count > 0) {
            const fb_entry_t &e = s_fb[cursor];
            if (e.is_dir) {
                size_t l = strlen(path);
                if (l + 1 + strlen(e.name) + 1 < sizeof(path)) {
                    if (path[l - 1] != '/') { path[l++] = '/'; path[l] = 0; }
                    strcat(path, e.name);
                    fb_list(path); cursor = 0; draw();
                }
            }
        } else if ((k == 'd' || k == 'D') && s_fb_count > 0) {
            const fb_entry_t &e = s_fb[cursor];
            char full[192];
            snprintf(full, sizeof(full), "%s/%s", path, e.name);
            if (SD.remove(full)) { ui_toast("deleted", T_GOOD, 500); fb_list(path); if (cursor >= s_fb_count) cursor = s_fb_count - 1; draw(); }
            else { ui_toast("fail", T_BAD, 500); }
        }
    }
}

/* ========== Clock ========== */

void feat_clock(void)
{
    /* Uses GPS time if the LoRa-GNSS hat is present, else uptime. */
    ui_clear_body();
    ui_draw_footer("`=back");
    auto &d = M5Cardputer.Display;

    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2); d.print("CLOCK");
    d.drawFastHLine(4, BODY_Y + 12, 50, T_ACCENT);
    d.setTextColor(T_DIM, T_BG);
    d.setCursor(SCR_W / 2 - 20, BODY_Y + 70);
    d.print("uptime");

    char shown[16] = "";
    uint32_t last = 0;
    while (true) {
        if (millis() - last > 500) {
            last = millis();

            /* Big uptime. */
            uint32_t s = millis() / 1000;
            uint32_t h = s / 3600, m = (s % 3600) / 60, sec = s % 60;
            char buf[16];
            snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu",
                     (unsigned long)h, (unsigned long)m, (unsigned long)sec);
            if (strcmp(buf, shown) != 0) {
                strcpy(shown, buf);
                d.setTextColor(T_FG, T_BG);
                d.setTextSize(3);
                int w = d.textWidth(buf);
                d.setCursor((SCR_W - w) / 2, BODY_Y + 30);
                d.print(buf);
                d.setTextSize(1);
            }
            ui_draw_status(radio_name(), "clock");
        }
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(40); continue; }
        if (k == PK_ESC) break;
    }
}

/* ========== Settings (basic) ========== */

void feat_settings(void)
{
    ui_clear_body();
    auto &d = M5Cardputer.Display;
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2); d.print("SETTINGS");
    d.drawFastHLine(4, BODY_Y + 12, 70, T_ACCENT);
    d.setTextColor(T_FG, T_BG);
    d.setCursor(4, BODY_Y + 22); d.print("[W] saved WiFi");
    d.setCursor(4, BODY_Y + 34); d.print("[C] clear creds log");
    d.setCursor(4, BODY_Y + 46); d.print("[F] format preferences");
    d.setCursor(4, BODY_Y + 58); d.print("[S] format SD card");
    d.setCursor(4, BODY_Y + 70); d.print("[R] reboot");
    d.setCursor(4, BODY_Y + 82); d.print("[L] back to Launcher");
    ui_draw_footer("letter=go  `=back");
    while (true) {
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) return;
        if (k == 'w' || k == 'W') { extern void feat_wifi_connect(); feat_wifi_connect(); return; }
        if (k == 'c' || k == 'C') {
            if (sd_mount() && SD.remove("/poseidon/creds.log")) ui_toast("cleared", T_GOOD, 600);
            else ui_toast("fail", T_BAD, 600);
            return;
        }
        if (k == 'f' || k == 'F') {
            Preferences p; p.begin("poseidon", false); p.clear(); p.end();
            ui_toast("prefs cleared", T_GOOD, 600); return;
        }
        if (k == 's' || k == 'S') {
            ui_toast("formatting...", T_WARN, 300);
            if (sd_format()) ui_toast("SD formatted", T_GOOD, 900);
            else             ui_toast("SD format fail", T_BAD, 1200);
            return;
        }
        if (k == 'r' || k == 'R') { ESP.restart(); }
        if (k == 'l' || k == 'L') {
            /* bmorcelli's Launcher uses a custom bootloader that decides
             * which partition to run from the RTC reset-reason:
             *   POWERON_RESET  -> app0/test (Launcher)
             *   SW_CPU_RESET   -> app1/ota_0 (this app)
             * Per the project's own wiki ("impossible to return to the
             * Launcher unless you change the other application to it"),
             * there is no software-only way to force POWERON_RESET, so
             * we can't boot to Launcher from here. Tell the user what
             * the actual escape is. Only show the hint when running
             * under the Launcher partition scheme — otherwise it's
             * nonsense. */
            const esp_partition_t *p = esp_partition_find_first(
                ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_TEST, NULL);
            if (p) ui_toast("unplug USB to return", T_WARN, 1500);
            else   ui_toast("no launcher slot",    T_BAD,  900);
            return;
        }
    }
}

/* ===== Screensaver toggle =====
 * NVS-persistent. Default on, 120 s idle threshold. Flips with toast. */
#include "../screensaver.h"

void feat_screensaver_toggle(void)
{
    bool now_on = !screensaver_enabled();
    screensaver_enabled_set(now_on);
    ui_toast(now_on ? "Screensaver ON" : "Screensaver OFF",
             now_on ? T_GOOD : T_WARN, 900);
}

/* ===== Layout toggle =====
 * Flips the menu render style between the dense Terminal list and the
 * big-card Carousel layout. Persists to NVS via menu_style_set. The
 * change takes effect the next time we re-enter run_submenu, which
 * happens when this feature returns. */
#include "../menu.h"

void feat_menu_style_toggle(void)
{
    menu_style_t cur  = menu_style_get();
    menu_style_t next = (cur == MENU_STYLE_TERMINAL) ? MENU_STYLE_CAROUSEL
                                                     : MENU_STYLE_TERMINAL;
    menu_style_set(next);
    ui_toast(next == MENU_STYLE_CAROUSEL ? "Carousel layout"
                                         : "Terminal layout",
             T_GOOD, 900);
}

/* ===== Ambient Preview =====
 * Live preview of the active theme's procedural ambient motion. Hit ESC
 * to exit. 'a' / 'A' toggles the NVS-backed enable flag so the user can
 * disable ambient globally if it's distracting under menus. */
extern void ui_ambient_tick(int x, int y, int w, int h);
extern bool ui_ambient_enabled(void);
extern void ui_ambient_enabled_set(bool on);

void feat_ambient_preview(void)
{
    auto &d = M5Cardputer.Display;
    d.fillScreen(T_BG);
    ui_status_invalidate();
    ui_draw_status("AMBIENT", "preview");
    ui_draw_footer("[A] toggle on/off  [ESC] exit");

    while (true) {
        uint16_t k = input_poll();
        if (k == PK_ESC) break;
        if (k == 'a' || k == 'A') {
            ui_ambient_enabled_set(!ui_ambient_enabled());
            ui_toast(ui_ambient_enabled() ? "ambient ON" : "ambient OFF",
                     ui_ambient_enabled() ? T_GOOD : T_WARN, 700);
        }
        d.fillRect(0, BODY_Y, SCR_W, BODY_H, T_BG);
        ui_ambient_tick(0, BODY_Y, SCR_W, BODY_H);
        d.setTextColor(T_ACCENT, T_BG);
        d.setCursor(4, BODY_Y + 2);
        d.print(theme().name);
        d.setTextColor(T_DIM, T_BG);
        d.setCursor(4, BODY_Y + 12);
        d.print(ui_ambient_enabled() ? "ambient: on" : "ambient: off");
        delay(33);
    }
    ui_force_clear_body();
}
