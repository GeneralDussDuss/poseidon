/*
 * main.c — POSEIDON C5 Node.
 *
 * Boot sequence:
 *   1. init NVS + WiFi controller in STA+Null mode (ESP-NOW needs this)
 *   2. register ESP-NOW callbacks
 *   3. start HELLO broadcast task (announces us every 5s)
 *   4. dispatch loop: on CMD_* from S3, execute + stream responses
 *
 * The C5's identity is its factory MAC. The S3 auto-discovers us via
 * HELLO with has_5g=1, has_ieee802154=1 flags → knows we can do 5 GHz
 * + Zigbee.
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"

#include "proto.h"
#include "led_fx.h"

static const char *TAG = "c5_node";
static const uint8_t BROADCAST_MAC[6] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };

extern void wifi_scanner_run(const uint8_t *, uint16_t, uint16_t);
extern void zb_sniffer_start(const uint8_t *, uint8_t, uint16_t);
extern void zb_sniffer_stop(void);
extern void wifi_attacker_deauth(const uint8_t *, const posei_deauth_req_t *, uint16_t);
extern void pmkid_capture_start(const uint8_t *, const posei_pmkid_req_t *, uint16_t);
extern void pmkid_capture_stop(void);

static char s_node_name[12] = "C5-?";

static void send_hello(void)
{
    posei_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.magic   = POSEI_MAGIC;
    msg.version = POSEI_VERSION;
    msg.type    = POSEI_TYPE_HELLO;

    posei_hello_t *h = (posei_hello_t *)msg.payload;
    strncpy(h->name, s_node_name, sizeof(h->name) - 1);
    h->heap_kb        = esp_get_free_heap_size() / 1024;
    h->role           = 1;  /* c5 node */
    h->has_5g         = 1;
    h->has_ieee802154 = 1;
    msg.payload_len = sizeof(posei_hello_t);

    esp_now_send(BROADCAST_MAC, (const uint8_t *)&msg, sizeof(msg));
}

/* Suspended during a scan so the WiFi driver can hop channels +
 * switch bands freely. ESP-NOW transmits while scanning will lock
 * the radio to the current channel and starve the scanner. */
volatile bool g_pause_hello = false;

static void hello_task(void *_)
{
    while (1) {
        if (!g_pause_hello) send_hello();
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

struct scan_req_t { uint8_t mac[6]; uint16_t dur; uint16_t seq; };
static void scan_task(void *arg)
{
    struct scan_req_t *sr = (struct scan_req_t *)arg;
    g_pause_hello = true;       /* freeze ESP-NOW so scan can hop */
    wifi_scanner_run(sr->mac, sr->dur, sr->seq);
    g_pause_hello = false;
    free(sr);
    vTaskDelete(NULL);
}

static void on_recv(const esp_now_recv_info_t *info,
                    const uint8_t *data, int len)
{
    if (len < (int)sizeof(posei_msg_t)) return;
    const posei_msg_t *m = (const posei_msg_t *)data;
    if (m->magic != POSEI_MAGIC || m->version != POSEI_VERSION) return;

    /* Ensure sender is in peer table so we can reply directed. */
    if (!esp_now_is_peer_exist(info->src_addr)) {
        esp_now_peer_info_t pi = { 0 };
        memcpy(pi.peer_addr, info->src_addr, 6);
        pi.channel = 0;
        pi.encrypt = false;
        esp_now_add_peer(&pi);
    }

    ESP_LOGI(TAG, "cmd type=%u seq=%u from %02x:%02x:...",
             m->type, m->seq, info->src_addr[0], info->src_addr[1]);

    switch (m->type) {
    case POSEI_TYPE_CMD_PING: {
        led_fx_set(LED_MODE_PING);
        posei_msg_t pong;
        memset(&pong, 0, sizeof(pong));
        pong.magic   = POSEI_MAGIC;
        pong.version = POSEI_VERSION;
        pong.type    = POSEI_TYPE_RESP_PONG;
        pong.seq     = m->seq;
        esp_now_send(info->src_addr, (const uint8_t *)&pong, sizeof(pong));
        break;
    }
    case POSEI_TYPE_CMD_SCAN_5G:
    case POSEI_TYPE_CMD_SCAN_2G: {
        led_fx_set(LED_MODE_SCAN);
        uint16_t dur = 150;
        if (m->payload_len >= sizeof(posei_scan_req_t)) {
            const posei_scan_req_t *r = (const posei_scan_req_t *)m->payload;
            dur = r->duration_ms;
        }
        struct scan_req_t *sr = malloc(sizeof(*sr));
        if (sr) {
            memcpy(sr->mac, info->src_addr, 6);
            sr->dur = dur;
            sr->seq = m->seq;
            xTaskCreate(scan_task, "scan", 4096, sr, 4, NULL);
        }
        break;
    }
    case POSEI_TYPE_CMD_SCAN_ZB: {
        led_fx_set(LED_MODE_SCAN);
        uint8_t ch = 0xFF;
        if (m->payload_len >= 1) ch = m->payload[0];
        zb_sniffer_start(info->src_addr, ch, m->seq);
        break;
    }
    case POSEI_TYPE_CMD_DEAUTH: {
        if (m->payload_len < (int)sizeof(posei_deauth_req_t)) break;
        led_fx_set(LED_MODE_ATTACK);
        const posei_deauth_req_t *r = (const posei_deauth_req_t *)m->payload;
        wifi_attacker_deauth(info->src_addr, r, m->seq);
        break;
    }
    case POSEI_TYPE_CMD_PMKID: {
        if (m->payload_len < (int)sizeof(posei_pmkid_req_t)) break;
        led_fx_set(LED_MODE_SCAN);
        const posei_pmkid_req_t *r = (const posei_pmkid_req_t *)m->payload;
        pmkid_capture_start(info->src_addr, r, m->seq);
        break;
    }
    case POSEI_TYPE_CMD_STOP:
        led_fx_set(LED_MODE_IDLE);
        zb_sniffer_stop();
        pmkid_capture_stop();
        break;
    }
}

void app_main(void)
{
    /* Set node name from MAC. */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_node_name, sizeof(s_node_name), "C5-%02X%02X", mac[4], mac[5]);

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    /* THE big one: enable dual-band on the C5. Without this the
     * driver only scans 2.4 GHz even on this dual-band chip. */
    esp_err_t bm_err = esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
    ESP_LOGI(TAG, "set_band_mode AUTO: %s", esp_err_to_name(bm_err));
    wifi_band_mode_t bm = WIFI_BAND_MODE_AUTO;
    esp_wifi_get_band_mode(&bm);
    ESP_LOGI(TAG, "band_mode is now: %d (0=AUTO 1=2G 2=5G)", (int)bm);

    /* Country = US opens up the full 5 GHz band (UNII-1/2A/3) for
     * active scanning. Default '01' worldwide restricts most 5 GHz
     * channels to passive-only which makes the dual-band scan miss
     * almost everything above 2.4. */
    /* Use cc='01' (worldwide) with nchan=13 to allow scanning every
     * 2.4 GHz channel a neighbor might be on. US-only nchan=11 was
     * silently dropping channels 12-13 which are common abroad. */
    wifi_country_t country = {
        .cc = "01",
        .schan = 1,
        .nchan = 13,
        .policy = WIFI_COUNTRY_POLICY_MANUAL,
    };
    esp_wifi_set_country(&country);

    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_recv));

    /* Add broadcast as a peer so we can send HELLOs. */
    esp_now_peer_info_t pi = { 0 };
    memcpy(pi.peer_addr, BROADCAST_MAC, 6);
    esp_now_add_peer(&pi);

    led_fx_init();
    led_fx_set(LED_MODE_IDLE);

    ESP_LOGI(TAG, "POSEIDON C5 Node '%s' online", s_node_name);
    xTaskCreate(hello_task, "hello", 3072, NULL, 4, NULL);

    while (1) vTaskDelay(portMAX_DELAY);
}
