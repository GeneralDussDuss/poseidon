/*
 * wifi_scanner.c — dual-band WiFi scan for the C5.
 *
 * C5 supports 802.11ax on BOTH 2.4 GHz AND 5 GHz. A standard
 * esp_wifi_scan_start() without band filtering will sweep both.
 */
#include "proto.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "wifi_scanner";

/* Call from any task. Sends one or more RESP_AP frames to the mac
 * that requested the scan. */
void wifi_scanner_run(const uint8_t requester[6],
                      uint16_t duration_ms,
                      uint16_t seq)
{
    wifi_scan_config_t cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,   /* scan all channels on all bands */
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = {
            .active = { .min = 80, .max = duration_ms ? duration_ms : 150 },
        },
    };
    esp_err_t err = esp_wifi_scan_start(&cfg, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scan_start: %s", esp_err_to_name(err));
        return;
    }

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n == 0) return;

    wifi_ap_record_t *records = malloc(sizeof(wifi_ap_record_t) * n);
    if (!records) return;
    esp_wifi_scan_get_ap_records(&n, records);

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
    free(records);
}
