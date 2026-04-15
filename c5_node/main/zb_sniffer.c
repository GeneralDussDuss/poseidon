/*
 * zb_sniffer.c — 802.15.4 (Zigbee/Thread) passive sniffer for C5.
 *
 * The C5 has a real 802.15.4 radio. We use esp_ieee802154_receive()
 * to collect raw frames, extract basic header info, and stream each
 * as a RESP_ZB summary over ESP-NOW. The requester can ask for a
 * specific channel (11-26) or 0xFF to channel-hop.
 */
#include "proto.h"
#include "esp_ieee802154.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "led_fx.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "zb_sniffer";

static uint8_t   s_requester[6];
static uint16_t  s_seq;
static volatile bool s_running = false;
static volatile uint8_t s_channel = 15;
static volatile bool  s_hop = false;

/* Receive callback runs in ISR context — must be brief. */
IRAM_ATTR void esp_ieee802154_receive_done(uint8_t *frame,
                                            esp_ieee802154_frame_info_t *info)
{
    if (!s_running || !frame) return;
    uint8_t len = frame[0];
    if (len < 5) { esp_ieee802154_receive_handle_done(frame); return; }

    posei_msg_t msg;
    proto_init_msg(&msg, POSEI_TYPE_RESP_ZB);
    msg.seq = s_seq;
    posei_zb_t *z = (posei_zb_t *)msg.payload;
    memset(z, 0, sizeof(*z));
    z->channel = s_channel;
    z->rssi    = info ? info->rssi : 0;
    uint8_t fc0 = frame[1];
    z->frame_type = fc0 & 0x07;
    /* Dest PAN id at offset 3 (little endian). */
    if (len >= 5) z->pan_id = frame[3] | (frame[4] << 8);
    /* Full address fields require decoding addressing modes; keep it
     * minimal for now. */
    if (len >= 7) z->seq = frame[2];
    msg.payload_len = sizeof(posei_zb_t);
    proto_send_to(s_requester, &msg);
    led_fx_set(LED_MODE_ZB_RX);

    esp_ieee802154_receive_handle_done(frame);
    /* Some IDF builds need explicit re-arm even after handle_done. */
    esp_ieee802154_receive();
}

/* IEEE 802.15.4 MAC beacon request frame. Frame control:
 *   FCF=0x0803  (Cmd, no security, no ack req, dst-addr=short, no src-addr,
 *                src/dst PAN compressed)
 * Payload:
 *   seq | dstPAN(0xFFFF) | dstShort(0xFFFF) | cmd_id=7 (beacon req)
 * IEEE 802.15.4 length byte goes first when handed to esp_ieee802154_transmit. */
static uint8_t s_beacon_req[10] = {
    0x09,                   /* len = 9 (excludes itself) */
    0x03, 0x08,             /* FCF (LE): 0x0803 */
    0x00,                   /* seq (we'll bump per send) */
    0xFF, 0xFF,             /* dst PAN: broadcast */
    0xFF, 0xFF,             /* dst short addr: broadcast */
    0x07,                   /* MAC command: Beacon Request */
    0x00,                   /* placeholder for MIC/FCS (HW appends FCS) */
};

static volatile uint8_t s_zb_seq;

static void hop_task(void *_)
{
    while (s_running && s_hop) {
        uint8_t ch = 11 + (esp_random() % 16);  /* 11..26 */
        s_channel = ch;
        esp_ieee802154_set_channel(ch);
        /* Active probe: send a beacon request so any nearby Zigbee
         * coordinators reveal themselves immediately instead of us
         * waiting for their next periodic broadcast. */
        s_beacon_req[3] = ++s_zb_seq;
        esp_ieee802154_transmit(s_beacon_req, false);
        vTaskDelay(pdMS_TO_TICKS(450));
    }
    vTaskDelete(NULL);
}

void zb_sniffer_start(const uint8_t requester[6], uint8_t channel, uint16_t seq)
{
    memcpy(s_requester, requester, 6);
    s_seq = seq;

    esp_ieee802154_enable();
    if (channel == 0xFF) {
        s_hop = true;
        s_channel = 15;
    } else {
        s_hop = false;
        s_channel = channel;
    }
    esp_ieee802154_set_channel(s_channel);
    esp_ieee802154_set_promiscuous(true);
    esp_ieee802154_receive();

    if (!s_running) {
        s_running = true;
        if (s_hop) xTaskCreate(hop_task, "zb_hop", 3072, NULL, 5, NULL);
    }
    ESP_LOGI(TAG, "zigbee sniff ch=%u hop=%d", s_channel, s_hop);
}

void zb_sniffer_stop(void)
{
    s_running = false;
    s_hop = false;
    esp_ieee802154_disable();
}
