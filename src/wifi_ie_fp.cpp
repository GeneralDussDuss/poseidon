/*
 * wifi_ie_fp — see wifi_ie_fp.h for the technique writeup.
 */
#include "wifi_ie_fp.h"
#include <string.h>
#include <stddef.h>

/* Standard 802.11 tag IDs we recognize. Everything else still counts
 * toward ie_count (for the "richer frame wins" refresh rule) but doesn't
 * set a bitmap bit. */
enum {
    TAG_SSID         = 0,
    TAG_RATES        = 1,
    TAG_DSPARAM      = 3,
    TAG_HTCAP        = 45,
    TAG_EXRATES      = 50,
    TAG_INTERWORKING = 107,
    TAG_EXTCAP       = 127,
    TAG_VHTCAP       = 191,
    TAG_VENDOR       = 221,
    TAG_EXT_HE       = 255,
};

void wifi_ie_fp_parse(const uint8_t *frame, uint16_t frame_len, wifi_ie_fp_t *fp)
{
    memset(fp, 0, sizeof(*fp));
    if (!frame || frame_len <= 24) return;

    uint16_t off = 24;  /* tags start right after the fixed MAC header */
    while (off + 2u <= frame_len) {
        uint8_t  id      = frame[off];
        uint8_t  len     = frame[off + 1];
        uint16_t data_off = (uint16_t)(off + 2);

        /* Bounds check: a tag claiming more payload than the frame
         * actually has is truncated/malformed — stop rather than read
         * past the buffer. */
        if ((uint32_t)data_off + len > frame_len) break;

        if (fp->ie_count < 255) fp->ie_count++;

        switch (id) {
        case TAG_SSID:         fp->tag_bitmap |= WIFI_IE_FP_SSID;      break;
        case TAG_RATES:        fp->tag_bitmap |= WIFI_IE_FP_RATES;     break;
        case TAG_DSPARAM:      fp->tag_bitmap |= WIFI_IE_FP_DSPARAM;   break;
        case TAG_HTCAP:
            fp->tag_bitmap |= WIFI_IE_FP_HTCAP;
            if (len >= 1) fp->ht_cap_byte = frame[data_off];
            break;
        case TAG_EXRATES:      fp->tag_bitmap |= WIFI_IE_FP_EXRATES;     break;
        case TAG_INTERWORKING: fp->tag_bitmap |= WIFI_IE_FP_INTERWORK;   break;
        case TAG_EXTCAP:
            fp->tag_bitmap |= WIFI_IE_FP_EXTCAP;
            if (len >= 1) fp->ext_cap_byte = frame[data_off];
            break;
        case TAG_VHTCAP:        fp->tag_bitmap |= WIFI_IE_FP_VHTCAP;     break;
        case TAG_VENDOR:
            fp->tag_bitmap |= WIFI_IE_FP_VENDOR;
            if (len >= 3 && fp->vendor_n < WIFI_IE_FP_MAX_VENDOR_OUI) {
                uint32_t oui = ((uint32_t)frame[data_off]     << 16) |
                               ((uint32_t)frame[data_off + 1] <<  8) |
                                (uint32_t)frame[data_off + 2];
                bool dup = false;
                for (uint8_t i = 0; i < fp->vendor_n; ++i)
                    if (fp->vendor_oui[i] == oui) { dup = true; break; }
                if (!dup) fp->vendor_oui[fp->vendor_n++] = oui;
            }
            break;
        case TAG_EXT_HE:        fp->tag_bitmap |= WIFI_IE_FP_EXT_HE;     break;
        default: break;
        }

        off = (uint16_t)(data_off + len);
    }
}

static inline void fnv1a_byte(uint32_t *h, uint8_t b)
{
    *h ^= b;
    *h *= 16777619u;   /* FNV-1a 32-bit prime */
}

uint32_t wifi_ie_fp_hash(const wifi_ie_fp_t *fp)
{
    uint32_t h = 2166136261u;  /* FNV-1a 32-bit offset basis */
    for (int i = 0; i < 4; ++i)
        fnv1a_byte(&h, (uint8_t)(fp->tag_bitmap >> (i * 8)));
    fnv1a_byte(&h, fp->ht_cap_byte);
    fnv1a_byte(&h, fp->ext_cap_byte);
    fnv1a_byte(&h, fp->ie_count);
    fnv1a_byte(&h, fp->vendor_n);
    for (uint8_t i = 0; i < fp->vendor_n; ++i) {
        uint32_t o = fp->vendor_oui[i];
        fnv1a_byte(&h, (uint8_t)(o >> 16));
        fnv1a_byte(&h, (uint8_t)(o >> 8));
        fnv1a_byte(&h, (uint8_t)o);
    }
    return h;
}

/* Vendor-specific IE OUI table. `generic` marks a standards-body OUI
 * that shows up on nearly every WPA/P2P-capable device and therefore
 * identifies nothing about who built the radio — wifi_ie_fp_vendor()
 * only returns one of these if no real-manufacturer OUI was also seen.
 *
 * Sources (public OUI assignments, IEEE standards-oui.ieee.org registry
 * and well-documented 802.11 vendor-IE formats used by Wireshark's
 * manuf database and common WiFi tooling):
 *   00:17:F2  Apple, Inc.                — Apple vendor-specific IE,
 *                                           seen on iOS/macOS probe requests
 *   00:50:F2  Microsoft Corp.            — WPS / WPA vendor IE (generic:
 *                                           nearly every WPA-capable device)
 *   50:6F:9A  Wi-Fi Alliance             — P2P / WiFi Direct vendor IE
 *                                           (generic: WFA standards body)
 *   00:0F:AC  IEEE 802.11 (RSN OUI)      — RSN/AKM suite selector OUI
 *                                           (generic: standards body,
 *                                           appears on virtually every
 *                                           WPA2/WPA3 device)
 *   00:10:18  Broadcom Corp.             — Broadcom WiFi chipset vendor IE
 *   00:03:7F  Atheros Communications     — Atheros/Qualcomm Atheros (QCA)
 *                                           WiFi chipset vendor IE
 */
struct oui_entry_t {
    uint32_t    oui;      /* 3-byte OUI packed into the low 24 bits */
    const char *name;
    bool        generic;  /* standards body, not a distinguishing manufacturer */
};

static const oui_entry_t OUI_TABLE[] = {
    { 0x0017F2u, "Apple",               false },
    { 0x0050F2u, "Microsoft (WPS/WPA)", true  },
    { 0x506F9Au, "Wi-Fi Alliance (P2P)",true  },
    { 0x000FACu, "IEEE 802.11 (RSN)",   true  },
    { 0x001018u, "Broadcom",            false },
    { 0x00037Fu, "Atheros/Qualcomm",    false },
};
#define OUI_TABLE_N (sizeof(OUI_TABLE) / sizeof(OUI_TABLE[0]))

const char *wifi_ie_fp_vendor(const wifi_ie_fp_t *fp)
{
    const char *generic_hit = nullptr;
    for (uint8_t i = 0; i < fp->vendor_n; ++i) {
        for (size_t j = 0; j < OUI_TABLE_N; ++j) {
            if (OUI_TABLE[j].oui != fp->vendor_oui[i]) continue;
            if (!OUI_TABLE[j].generic) return OUI_TABLE[j].name;  /* real mfr wins outright */
            if (!generic_hit) generic_hit = OUI_TABLE[j].name;
            break;
        }
    }
    return generic_hit;  /* may be nullptr — no known OUI seen */
}

const char *wifi_ie_fp_class(const wifi_ie_fp_t *fp)
{
    if (fp->tag_bitmap & WIFI_IE_FP_EXT_HE) return "WiFi 6 device";
    if (fp->tag_bitmap & WIFI_IE_FP_VHTCAP) return "WiFi 5 (phone/laptop)";
    if (fp->tag_bitmap & WIFI_IE_FP_HTCAP)  return "WiFi 4 (IoT/older)";
    return "legacy/minimal (IoT)";
}
