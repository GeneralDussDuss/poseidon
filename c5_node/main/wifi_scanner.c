/*
 * wifi_scanner.c — dual-band WiFi scan for the C5.
 *
 * On ESP32-C5 with band_mode=AUTO + country=US + per-STA protocols
 * 11B|G|N|AX (2.4) and 11A|N|AC|AX (5), a single esp_wifi_scan_start
 * with channel=0 sweeps every country-allowed channel across 2.4 GHz
 * AND 5 GHz in one pass. The channel_bitmap fields don't actually
 * restrict which BAND gets scanned — the driver hits both regardless.
 *
 * We previously did a 3-phase split (2G active + 5G active + 5G
 * passive) because early runs returned 5G=0. That turned out to be
 * a duplicate-command race, not a scan-config issue. Single pass
 * is ~2 seconds and catches the same APs the 3-phase version did in
 * ~25 seconds.
 */
#include "proto.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "wifi_scanner";

void wifi_scanner_run(const uint8_t requester[6],
                      uint16_t duration_ms,
                      uint16_t seq)
{
    uint16_t dwell = duration_ms ? duration_ms : 400;
    if (dwell < 300) dwell = 300;

    wifi_scan_config_t cfg = {
        .ssid = NULL, .bssid = NULL,
        .channel = 0,                    /* 0 = all country-allowed */
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = { .active = { .min = 120, .max = dwell } },
    };
    ESP_LOGI(TAG, "dual-band scan, dwell=%ums", (unsigned)dwell);
    esp_err_t err = esp_wifi_scan_start(&cfg, true);

    /* Scanner leaves the radio on whatever channel it last dwelled on
     * (usually 5 GHz). ESP-NOW results MUST go out on ch 1 or POSEIDON
     * (pinned to ch 1 for RX) never hears them. Restore before any
     * posei_msg_t proto_send_to(). */
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scan_start: %s", esp_err_to_name(err));
        return;
    }

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n == 0) { ESP_LOGI(TAG, "scan done, 0 APs"); return; }

    wifi_ap_record_t *records = malloc(sizeof(wifi_ap_record_t) * n);
    if (!records) return;
    esp_wifi_scan_get_ap_records(&n, records);

    int n_2g = 0, n_5g = 0;
    for (int i = 0; i < n; ++i) {
        if (records[i].primary > 14) n_5g++;
        else                         n_2g++;
    }
    ESP_LOGI(TAG, "scan done, %u APs (%u 2G + %u 5G)",
             (unsigned)n, (unsigned)n_2g, (unsigned)n_5g);

    posei_msg_t msg;
    for (uint16_t i = 0; i < n; ) {
        proto_init_msg(&msg, POSEI_TYPE_RESP_AP);
        msg.seq = seq;
        int fit = sizeof(msg.payload) / sizeof(posei_ap_t);
        int batch = (n - i < fit) ? (n - i) : fit;
        posei_ap_t *out = (posei_ap_t *)msg.payload;
        for (int k = 0; k < batch; ++k) {
            memcpy(out[k].bssid, records[i + k].bssid, 6);
            out[k].channel = records[i + k].primary;
            out[k].rssi    = records[i + k].rssi;
            out[k].auth    = records[i + k].authmode;
            out[k].is_5g   = (records[i + k].primary > 14) ? 1 : 0;
            strncpy(out[k].ssid, (char *)records[i + k].ssid, 32);
            out[k].ssid[32] = '\0';
        }
        msg.payload_len = batch * sizeof(posei_ap_t);
        proto_send_to(requester, &msg);
        i += batch;
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    free(records);
}
