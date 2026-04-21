/*
 * wifi_scan — reference feature. Shows the POSEIDON pattern:
 *
 *   1. Enter feature: radio_switch(RADIO_WIFI), start async scan task.
 *   2. While running: UI shows live status, keyboard is responsive.
 *   3. Results list: letter hotkeys for actions (D=deauth, C=clone,
 *      I=info, P=portal, O=open-only filter, / = typed filter).
 *   4. ESC: drop out, caller decides radio teardown.
 *
 * This is the template. Every feature should follow this shape —
 * never block the UI, always let the keyboard drive actions.
 */
#include "app.h"
#include "../theme.h"
#include "ui.h"
#include "input.h"
#include "radio.h"
#include "menu.h"
#include "wifi_types.h"
#include "sd_helper.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_heap_caps.h>
#include <SD.h>

#define MAX_APS 64

static ap_t     s_aps[MAX_APS];
static int      s_ap_count = 0;
static volatile bool s_scan_running = false;
static volatile bool s_scan_done = false;
static char     s_filter[24] = "";
static bool     s_filter_open_only = false;

/* Shared with portal, deauth, ap-clone. Declared in wifi_types.h. */
ap_t g_last_selected_ap = {};
bool g_last_selected_valid = false;

static const char *auth_str(uint8_t a)
{
    switch (a) {
    case WIFI_AUTH_OPEN:          return "OPEN";
    case WIFI_AUTH_WEP:           return "WEP";
    case WIFI_AUTH_WPA_PSK:       return "WPA";
    case WIFI_AUTH_WPA2_PSK:      return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:  return "WPA/2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "EAP";
    case WIFI_AUTH_WPA3_PSK:      return "WPA3";
    default:                      return "?";
    }
}

static void scan_task(void *)
{
    s_scan_done = false;
    s_scan_running = true;
    size_t heap_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    Serial.printf("[wifi_scan] heap_free=%u mode=%d\n",
                  (unsigned)heap_free, (int)WiFi.getMode());

    /* WiFi init needs ~50KB. If prior feature left the heap fragmented,
     * retry after a settle delay (idle task cleans up deleted task
     * stacks + internal buffers). */
    wifi_mode_t m = WiFi.getMode();
    if (m != WIFI_STA && m != WIFI_AP_STA) {
        for (int attempt = 0; attempt < 4; attempt++) {
            if (WiFi.mode(WIFI_STA)) break;
            Serial.printf("[wifi_scan] WiFi.mode attempt %d failed, heap=%u\n",
                          attempt, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            /* Force a clean deinit before retry — lets prior-feature state
             * release. */
            esp_wifi_stop();
            esp_wifi_deinit();
            vTaskDelay(pdMS_TO_TICKS(250));
        }
    }
    int n = WiFi.scanNetworks(false, true, false, 120);  /* blocking — we're in a task */
    Serial.printf("[wifi_scan] scanNetworks -> %d (heap=%u)\n",
                  n, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    s_ap_count = 0;
    if (n > 0) {
        for (int i = 0; i < n && s_ap_count < MAX_APS; ++i) {
            ap_t &a = s_aps[s_ap_count++];
            strncpy(a.ssid, WiFi.SSID(i).c_str(), sizeof(a.ssid) - 1);
            a.ssid[sizeof(a.ssid) - 1] = '\0';
            memcpy(a.bssid, WiFi.BSSID(i), 6);
            a.rssi    = WiFi.RSSI(i);
            a.channel = WiFi.channel(i);
            a.auth    = (uint8_t)WiFi.encryptionType(i);
        }
    }
    WiFi.scanDelete();
    s_scan_running = false;
    s_scan_done = true;
    vTaskDelete(nullptr);
}

static bool ap_matches_filter(const ap_t &a)
{
    if (s_filter_open_only && a.auth != WIFI_AUTH_OPEN) return false;
    if (s_filter[0] == '\0') return true;
    /* Case-insensitive substring on SSID. */
    const char *s = a.ssid;
    size_t fl = strlen(s_filter);
    for (; *s; ++s) {
        if (strncasecmp(s, s_filter, fl) == 0) return true;
    }
    return false;
}

/* Render a 10-row window into the filtered list, centered on cursor. */
static void draw_list(int cursor)
{
    ui_clear_body();
    auto &d = M5Cardputer.Display;

    /* Header row with filter status. */
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2);
    d.printf("APs %d", s_ap_count);
    if (s_filter[0] || s_filter_open_only) {
        d.setTextColor(T_WARN, T_BG);
        d.printf("  filter:%s%s", s_filter, s_filter_open_only ? "+open" : "");
    }

    /* Build filtered index list so cursor navigates visible items. */
    int idx[MAX_APS];
    int n = 0;
    for (int i = 0; i < s_ap_count; ++i) {
        if (ap_matches_filter(s_aps[i])) idx[n++] = i;
    }
    if (n == 0) {
        d.setTextColor(T_DIM, T_BG);
        d.setCursor(4, BODY_Y + 18);
        d.print(s_scan_running ? "scanning..." : "no matches");
        return;
    }
    if (cursor >= n) cursor = n - 1;
    if (cursor < 0) cursor = 0;

    int rows = 9;
    int first = cursor - rows / 2;
    if (first < 0) first = 0;
    if (first + rows > n) first = max(0, n - rows);

    for (int r = 0; r < rows && first + r < n; ++r) {
        int ai = idx[first + r];
        const ap_t &a = s_aps[ai];
        int y = BODY_Y + 14 + r * 11;
        bool sel = (first + r == cursor);
        uint16_t bg = sel ? 0x18A3 : T_BG;
        uint16_t fg = sel ? T_ACCENT : T_FG;
        if (sel) d.fillRect(0, y - 1, SCR_W, 11, bg);

        /* ch | rssi | auth | ssid */
        d.setTextColor(T_DIM, bg);
        d.setCursor(2, y);
        d.printf("%2u", a.channel);
        d.setTextColor(fg, bg);
        d.setCursor(18, y);
        d.printf("%4d", a.rssi);
        d.setTextColor(a.auth == WIFI_AUTH_OPEN ? T_BAD : T_GOOD, bg);
        d.setCursor(44, y);
        d.printf("%-5s", auth_str(a.auth));
        d.setTextColor(fg, bg);
        d.setCursor(82, y);
        d.print(a.ssid);
    }
}

/* Other features use g_last_selected_ap. We set it here so the user
 * can jump directly from the AP detail view into an attack. */
extern void feat_wifi_deauth(void);
extern void feat_wifi_deauth_broadcast(void);
extern void feat_wifi_apclone(void);
extern void feat_wifi_portal(void);
extern void feat_wifi_clients(void);

/* Returns a ui_state hint — but we just call the feature directly and
 * return once it finishes. */
static void show_details(const ap_t &a)
{
    ui_clear_body();
    auto &d = M5Cardputer.Display;
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2);  d.print("AP DETAILS");
    d.drawFastHLine(4, BODY_Y + 12, 100, T_ACCENT);
    d.setTextColor(T_FG, T_BG);
    d.setCursor(4, BODY_Y + 18); d.printf("SSID : %.24s", a.ssid);
    d.setCursor(4, BODY_Y + 30); d.printf("BSSID: %02X:%02X:%02X:%02X:%02X:%02X",
        a.bssid[0], a.bssid[1], a.bssid[2], a.bssid[3], a.bssid[4], a.bssid[5]);
    d.setCursor(4, BODY_Y + 42); d.printf("CH   : %u", a.channel);
    d.setCursor(4, BODY_Y + 54); d.printf("RSSI : %d dBm", a.rssi);
    d.setCursor(4, BODY_Y + 66); d.printf("AUTH : %s", auth_str(a.auth));
    ui_draw_footer("D=dth X=bcast L=clnt C=clone P=portal `=back");

    while (true) {
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(10); continue; }
        if (k == PK_ESC) return;

        switch ((char)tolower((int)k)) {
        case 'd': feat_wifi_deauth();           return;
        case 'x': feat_wifi_deauth_broadcast(); return;
        case 'l': feat_wifi_clients();          return;
        case 'c': feat_wifi_apclone();          return;
        case 'p': feat_wifi_portal();           return;
        }
    }
}

void feat_wifi_scan(void)
{
    static int s_saved_cursor = 0;     /* remembered across re-entries */
    static bool s_have_results = false; /* skip re-scan if last scan still fresh */
    radio_switch(RADIO_WIFI);
    s_filter[0] = '\0';
    s_filter_open_only = false;

    ui_draw_status(radio_name(), s_have_results ? "cached" : "scanning...");
    ui_draw_footer("/=flt O=open S=save R=rescan ENTER=info `=back");
    draw_list(s_saved_cursor);

    if (!s_have_results) {
        s_ap_count = 0;
        xTaskCreate(scan_task, "wifi_scan", 4096, nullptr, 4, nullptr);
    }

    int cursor = s_saved_cursor;
    uint32_t last_redraw = 0;
    /* Track the last-painted state so we only redraw the list when
     * something actually changed. Redrawing 9 identical rows every 400ms
     * was the visible flicker on this screen. */
    int  last_count   = -1;
    int  last_cursor  = -1;
    bool last_running = !s_scan_running;      /* force first paint */
    while (true) {
        bool state_changed = (s_ap_count != last_count)
                          || (cursor != last_cursor)
                          || (s_scan_running != last_running);

        /* Status bar updates itself (cached). Redraw the list only on
         * actual state change, or every 2 s as a cheap refresh while
         * the scan task is still discovering new APs. */
        if (state_changed || (s_scan_running && millis() - last_redraw > 2000)) {
            ui_draw_status(radio_name(), s_scan_running ? "..." : "done");
            draw_list(cursor);
            last_redraw = millis();
            last_count   = s_ap_count;
            last_cursor  = cursor;
            last_running = s_scan_running;
        }
        /* Radar sweep in top-right while scanning. */
        if (s_scan_running) ui_radar(SCR_W - 16, BODY_Y + 8, 7, 0x07FF);

        if (s_scan_done) {
            /* Only cache a scan result if it actually found something —
             * otherwise re-entries would be stuck on cached zeros forever
             * without the user knowing to press R. */
            if (s_ap_count > 0) s_have_results = true;
            s_scan_done = false;
        }
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) { s_saved_cursor = cursor; break; }

        switch (k) {
        /* Key handlers only update state — the state-change gate at the
         * top of the loop draws exactly once per change. Direct draw_list
         * calls here would double-paint. */
        case ';': case PK_UP:
            cursor--; if (cursor < 0) cursor = 0; break;
        case '.': case PK_DOWN:
            cursor++; break;
        case 'r': case 'R':
            if (!s_scan_running) {
                s_ap_count = 0;
                s_have_results = false;
                xTaskCreate(scan_task, "wifi_scan", 4096, nullptr, 4, nullptr);
            }
            break;
        case '?':
            ui_show_current_help();
            last_count = -1;    /* force redraw of list after help modal */
            ui_draw_footer("/=flt O=open S=save R=rescan ENTER=info `=back");
            break;
        case 'o': case 'O':
            s_filter_open_only = !s_filter_open_only;
            last_count = -1;    /* filter toggle — force redraw */
            break;
        case '/':
            if (!input_line("Filter SSID contains:", s_filter, sizeof(s_filter))) {
                s_filter[0] = '\0';
            }
            last_count = -1;    /* filter changed — force redraw */
            break;
        case 's': case 'S': {
            if (s_ap_count == 0) { ui_toast("no results", T_WARN, 800); break; }
            char path[64];
            File f = sdlog_open("wifiscan", "ssid,bssid,channel,rssi,auth",
                                path, sizeof(path));
            if (!f) { ui_toast("SD open failed", T_BAD, 1000); break; }
            int wrote = 0;
            for (int i = 0; i < s_ap_count; ++i) {
                if (!ap_matches_filter(s_aps[i])) continue;
                const ap_t &a = s_aps[i];
                f.printf("\"%s\",%02X:%02X:%02X:%02X:%02X:%02X,%u,%d,%s\n",
                         a.ssid,
                         a.bssid[0], a.bssid[1], a.bssid[2],
                         a.bssid[3], a.bssid[4], a.bssid[5],
                         (unsigned)a.channel, (int)a.rssi, auth_str(a.auth));
                wrote++;
            }
            f.close();
            char msg[32];
            snprintf(msg, sizeof(msg), "saved %d APs", wrote);
            ui_toast(msg, T_GOOD, 1000);
            Serial.printf("[wifi_scan] saved %s (%d rows)\n", path, wrote);
            break;
        }
        case PK_ENTER: {
            /* Show details of selected item. */
            int idx[MAX_APS];
            int n = 0;
            for (int i = 0; i < s_ap_count; ++i)
                if (ap_matches_filter(s_aps[i])) idx[n++] = i;
            if (n > 0 && cursor < n) {
                g_last_selected_ap    = s_aps[idx[cursor]];
                g_last_selected_valid = true;
                show_details(g_last_selected_ap);
                draw_list(cursor);
                ui_draw_footer("/=flt O=open S=save R=rescan ENTER=info `=back");
            }
            break;
        }
        default: break;
        }
    }
}
