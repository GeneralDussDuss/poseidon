/*
 * c5_scan — drive a C5 node to do a dual-band WiFi scan and display
 * the results. Also shows the C5 connection status indicator.
 */
#include "app.h"
#include "ui.h"
#include "input.h"
#include "radio.h"
#include "c5_cmd.h"
#include "ble_db.h"
#include <WiFi.h>

static void draw_status_header(void)
{
    auto &d = M5Cardputer.Display;
    int n = c5_peer_count();
    uint16_t col = n > 0 ? COL_GOOD : COL_BAD;
    d.fillCircle(SCR_W - 10, 6, 3, col);  /* status dot */
    d.setTextColor(col, 0x0841);
    d.setCursor(SCR_W - 60, 2);
    if (n == 0) d.print("no C5");
    else        d.printf("C5 x%d", n);
}

void feat_c5_status(void)
{
    /* Ensure c5 layer is running — go through radio_switch so BLE
     * tears down cleanly before WiFi comes up. */
    radio_switch(RADIO_WIFI);
    c5_begin();

    auto &d = M5Cardputer.Display;
    ui_draw_footer("P=ping  S=stop  `=back");

    uint32_t last = 0;
    while (true) {
        if (millis() - last > 400) {
            last = millis();
            ui_clear_body();
            d.setTextColor(0xF81F, COL_BG);
            d.setCursor(4, BODY_Y + 2); d.print("C5 NODES");
            d.drawFastHLine(4, BODY_Y + 12, 80, 0xF81F);
            int n = c5_peer_count();
            if (n == 0) {
                d.setTextColor(COL_BAD, COL_BG);
                d.setCursor(4, BODY_Y + 24); d.print("NO C5 ONLINE");
                d.setTextColor(COL_DIM, COL_BG);
                d.setCursor(4, BODY_Y + 38); d.print("power on your C5 node.");
                d.setCursor(4, BODY_Y + 48); d.print("it broadcasts HELLO every 5s.");
                d.setCursor(4, BODY_Y + 58); d.print("will auto-connect.");
            } else {
                d.setTextColor(COL_GOOD, COL_BG);
                d.setCursor(4, BODY_Y + 22);
                d.printf("ONLINE  %d peer%s", n, n == 1 ? "" : "s");
                for (int i = 0; i < n && i < 6; ++i) {
                    d.setTextColor(COL_FG, COL_BG);
                    d.setCursor(8, BODY_Y + 36 + i * 10);
                    d.printf("* %s", c5_peer_name(i));
                }
                d.setTextColor(COL_DIM, COL_BG);
                d.setCursor(4, BODY_Y + BODY_H - 10);
                d.printf("last seen: %lus ago",
                         (unsigned long)(c5_last_seen_ms() / 1000));
            }
            draw_status_header();
            ui_radar(SCR_W - 20, BODY_Y + BODY_H - 14, 8, COL_ACCENT);
        }
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(40); continue; }
        if (k == PK_ESC) break;
        if (k == 'p' || k == 'P') { c5_cmd_ping(); ui_toast("ping sent", COL_ACCENT, 400); }
        if (k == 's' || k == 'S') { c5_cmd_stop(); ui_toast("stop sent", COL_WARN, 400); }
    }
}

void feat_c5_scan_5g(void)
{
    radio_switch(RADIO_WIFI);
    c5_begin();

    if (!c5_any_online()) {
        ui_toast("no C5 online", COL_BAD, 1500);
        return;
    }
    c5_clear_results();
    c5_cmd_scan_5g(300);

    auto &d = M5Cardputer.Display;
    ui_draw_footer(";/. move  R=rescan  `=back");
    int cursor = 0;
    int last_n = -1;
    uint32_t last = 0;
    while (true) {
        if (millis() - last > 300) {
            last = millis();
            int n_now = c5_aps(nullptr, 0);
            bool new_result = (n_now != last_n);
            last_n = n_now;
            ui_clear_body();
            char title[32];
            snprintf(title, sizeof(title), "C5 DUAL-BAND (%d)", n_now);
            ui_dashboard_chrome(title, new_result);
            draw_status_header();
            ui_freq_bars(SCR_W - 58, BODY_Y + 2, 3, 10);

            c5_ap_t aps[64];
            int n = c5_aps(aps, 64);
            /* Sort by RSSI descending (bubble — n is small). */
            for (int i = 1; i < n; ++i) {
                for (int j = 0; j < n - i; ++j) {
                    if (aps[j].rssi < aps[j + 1].rssi) {
                        c5_ap_t t = aps[j]; aps[j] = aps[j + 1]; aps[j + 1] = t;
                    }
                }
            }

            if (n == 0) {
                d.setTextColor(COL_DIM, COL_BG);
                d.setCursor(4, BODY_Y + 24);
                d.print("waiting for C5 response...");
                ui_radar(SCR_W / 2, BODY_Y + 60, 25, 0x07FF);
            } else {
                int rows = 7;
                if (cursor < 0) cursor = 0;
                if (cursor >= n) cursor = n - 1;
                int first = cursor - rows / 2;
                if (first < 0) first = 0;
                if (first + rows > n) first = max(0, n - rows);
                for (int r = 0; r < rows && first + r < n; ++r) {
                    int i = first + r;
                    const c5_ap_t &a = aps[i];
                    int y = BODY_Y + 18 + r * 12;
                    bool sel = (i == cursor);
                    if (sel) d.fillRect(0, y - 1, SCR_W, 12, 0x3007);
                    /* Band badge. */
                    d.setTextColor(a.is_5g ? 0xF81F : COL_ACCENT, sel ? 0x3007 : COL_BG);
                    d.setCursor(2, y);
                    d.print(a.is_5g ? "5G" : "2G");
                    d.setTextColor(sel ? 0xFFFF : COL_FG, sel ? 0x3007 : COL_BG);
                    d.setCursor(22, y);
                    d.printf("%4d", a.rssi);
                    d.setTextColor(COL_DIM, sel ? 0x3007 : COL_BG);
                    d.setCursor(54, y);
                    d.printf("ch%u", a.channel);
                    d.setTextColor(sel ? COL_ACCENT : COL_FG, sel ? 0x3007 : COL_BG);
                    d.setCursor(84, y);
                    d.printf("%.22s", a.ssid[0] ? a.ssid : "<hidden>");
                }
            }
        }
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(40); continue; }
        if (k == PK_ESC) break;
        if (k == ';' || k == PK_UP)   { if (cursor > 0) cursor--; }
        if (k == '.' || k == PK_DOWN) { cursor++; }
        if (k == 'r' || k == 'R') { c5_clear_results(); c5_cmd_scan_5g(300); }
    }
}

void feat_c5_scan_zb(void)
{
    radio_switch(RADIO_WIFI);
    c5_begin();
    if (!c5_any_online()) { ui_toast("no C5 online", COL_BAD, 1500); return; }

    c5_clear_results();
    c5_cmd_scan_zb(0xFF);  /* hop all channels 11-26 */

    auto &d = M5Cardputer.Display;
    ui_draw_footer("`=back");
    int last_n = -1;
    uint32_t last = 0;
    while (true) {
        if (millis() - last > 300) {
            last = millis();
            c5_zb_t probe[1];
            int n_now = c5_zbs(probe, 0);
            bool new_frame = (n_now != last_n);
            last_n = n_now;
            ui_clear_body();
            ui_dashboard_chrome("C5 ZIGBEE SNIFF", new_frame);
            draw_status_header();
            ui_freq_bars(SCR_W - 58, BODY_Y + 2, 3, 10);

            c5_zb_t z[32];
            int n = c5_zbs(z, 32);
            if (n == 0) {
                d.setTextColor(COL_DIM, COL_BG);
                d.setCursor(4, BODY_Y + 24);
                d.print("listening on 802.15.4...");
            } else {
                int rows = 7;
                int first = n > rows ? n - rows : 0;
                for (int r = 0; r < rows && first + r < n; ++r) {
                    const c5_zb_t &e = z[first + r];
                    int y = BODY_Y + 18 + r * 12;
                    d.setTextColor(COL_ACCENT, COL_BG);
                    d.setCursor(2, y);  d.printf("ch%u", e.channel);
                    d.setTextColor(COL_FG, COL_BG);
                    d.setCursor(32, y); d.printf("%4d", e.rssi);
                    d.setTextColor(COL_WARN, COL_BG);
                    d.setCursor(64, y);
                    switch (e.frame_type) {
                    case 0: d.print("BCN "); break;
                    case 1: d.print("DATA"); break;
                    case 2: d.print("ACK "); break;
                    case 3: d.print("CMD "); break;
                    default: d.printf("t%d", e.frame_type);
                    }
                    d.setTextColor(COL_FG, COL_BG);
                    d.setCursor(102, y);
                    d.printf("PAN%04X src%04X", e.pan_id, e.src_short);
                }
            }
        }
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(40); continue; }
        if (k == PK_ESC) break;
    }
    c5_cmd_stop();
}
