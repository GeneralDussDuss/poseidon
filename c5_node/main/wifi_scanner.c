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
    /* Active scan, all channels, generous dwell. Active is what works
     * — passive missed everything in our environment. Default IDF
     * behavior: channel=0 scans every channel the country code allows
     * on every enabled band. */
    uint16_t dwell = duration_ms ? duration_ms : 600;
    if (dwell < 300) dwell = 300;
    wifi_scan_config_t cfg = {
        .ssid = NULL, .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = { .active = { .min = 120, .max = dwell } },
    };
    ESP_LOGI(TAG, "scan active dwell=%ums", (unsigned)dwell);
    esp_err_t err = esp_wifi_scan_start(&cfg, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scan_start: %s", esp_err_to_name(err));
        return;
    }

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    ESP_LOGI(TAG, "scan done, %u APs", (unsigned)n);
    if (n == 0) return;

    wifi_ap_record_t *records = malloc(sizeof(wifi_ap_record_t) * n);
    if (!records) return;
    esp_wifi_scan_get_ap_records(&n, records);
    /* Log every AP found so we can see what the C5 actually heard. */
    for (int i = 0; i < n; ++i) {
        ESP_LOGI(TAG, "AP[%d] ch=%u rssi=%d %s",
                 i, records[i].primary, records[i].rssi,
                 records[i].ssid[0] ? (const char *)records[i].ssid : "<hidden>");
    }

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
