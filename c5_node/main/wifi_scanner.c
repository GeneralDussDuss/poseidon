/*
 * wifi_scanner.c — dual-band WiFi scan for the C5.
 *
 * C5 supports 802.11ax on BOTH 2.4 GHz AND 5 GHz. A standard
 * esp_wifi_scan_start() without band filtering will sweep both.
 */
#include "proto.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "wifi_scanner";

/* Call from any task. Sends one or more RESP_AP frames to the mac
 * that requested the scan. */
void wifi_scanner_run(const uint8_t requester[6],
                      uint16_t duration_ms,
                      uint16_t seq)
{
    /* Two-pass scan: ACTIVE on 2.4 GHz (fast — APs respond to probes),
     * PASSIVE on 5 GHz (DFS rules forbid APs on UNII-2A/2C from
     * answering probes — we MUST listen for beacons). Active-only on
     * the upper band misses 80%+ of real-world 5 GHz APs. */

    /* Pass 1: 2.4 GHz active. */
    uint16_t dwell = duration_ms ? duration_ms : 200;
    wifi_scan_config_t cfg2 = {
        .ssid = NULL, .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = { .active = { .min = 80, .max = dwell } },
        .channel_bitmap = { .ghz_2_channels = 0x3FFE, .ghz_5_channels = 0 },
    };
    ESP_LOGI(TAG, "scan 2.4 active dwell=%ums", (unsigned)dwell);
    esp_wifi_scan_start(&cfg2, true);

    uint16_t n2 = 0;
    esp_wifi_scan_get_ap_num(&n2);
    ESP_LOGI(TAG, "2.4 done, %u APs", (unsigned)n2);

    wifi_ap_record_t *r24 = NULL;
    if (n2 > 0) {
        r24 = malloc(sizeof(wifi_ap_record_t) * n2);
        if (r24) esp_wifi_scan_get_ap_records(&n2, r24);
        else      n2 = 0;
    } else {
        esp_wifi_scan_get_ap_records(&n2, NULL);  /* free internal buffer */
    }

    /* Pass 2: 5 GHz passive. Listen for beacons across UNII-1/2A/2C/3.
     * Dwell longer because beacon period is typically 100ms — need at
     * least 200ms per channel to reliably catch one. */
    uint16_t dwell5 = dwell + 300;
    if (dwell5 < 400) dwell5 = 400;
    wifi_scan_config_t cfg5 = {
        .ssid = NULL, .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_PASSIVE,
        .scan_time = { .passive = dwell5 },
        .channel_bitmap = { .ghz_2_channels = 0, .ghz_5_channels = 0xFFFFFFFF },
    };
    ESP_LOGI(TAG, "scan 5 passive dwell=%ums", (unsigned)dwell5);
    esp_wifi_scan_start(&cfg5, true);

    uint16_t n5 = 0;
    esp_wifi_scan_get_ap_num(&n5);
    ESP_LOGI(TAG, "5 done, %u APs", (unsigned)n5);

    wifi_ap_record_t *r5 = NULL;
    if (n5 > 0) {
        r5 = malloc(sizeof(wifi_ap_record_t) * n5);
        if (r5) esp_wifi_scan_get_ap_records(&n5, r5);
        else     n5 = 0;
    } else {
        esp_wifi_scan_get_ap_records(&n5, NULL);
    }

    /* Merge into one logical record list. */
    uint16_t n = n2 + n5;
    if (n == 0) {
        free(r24); free(r5);
        return;
    }
    wifi_ap_record_t *records = malloc(sizeof(wifi_ap_record_t) * n);
    if (!records) { free(r24); free(r5); return; }
    if (r24) memcpy(records, r24, sizeof(wifi_ap_record_t) * n2);
    if (r5)  memcpy(records + n2, r5, sizeof(wifi_ap_record_t) * n5);
    free(r24); free(r5);

    /* Stream 4 APs per frame. */
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
    ESP_LOGI(TAG, "streamed %u records", (unsigned)n);
    free(records);
}
