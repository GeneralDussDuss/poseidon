/*
 * wifi_ie_fp — probe-request Information Element (IE) fingerprinting.
 *
 * A probe request's tagged-parameter chain (the IEs after the fixed
 * 802.11 management header) is a stable fingerprint of the device's WiFi
 * stack: which IEs are present, and a handful of capability bits, vary by
 * chipset/OS but stay constant across MAC address randomizations for a
 * given device model. Critically, vendor-specific IEs (tag 221) carry a
 * real 3-byte OUI in their payload — a phone that randomizes its header
 * MAC still emits e.g. an Apple vendor IE, leaking the true manufacturer
 * regardless of MAC randomization.
 *
 * Board-neutral: no POSEIDON_BOARD_* guards. Every board with a WiFi
 * promiscuous RX path benefits from this.
 */
#pragma once
#include <stdint.h>

/* Up to this many distinct vendor-specific OUIs are retained per client.
 * In practice a probe rarely carries more than 2-3 (WFA P2P + the chipset
 * vendor + maybe a WPS/MS one), so 4 slots covers real traffic with slack
 * to spare while keeping the struct small. */
#define WIFI_IE_FP_MAX_VENDOR_OUI 4

/* Bits in wifi_ie_fp_t.tag_bitmap — one per well-known tag ID we track.
 * Not a 1:1 map of tag-id -> bit-index; only pulls out the IDs called
 * out in the spec (SSID, rates, DS param, HT/VHT/HE caps, interworking,
 * extended caps, vendor-specific). */
enum {
    WIFI_IE_FP_SSID      = 1u << 0,  /* tag 0   SSID */
    WIFI_IE_FP_RATES     = 1u << 1,  /* tag 1   Supported Rates */
    WIFI_IE_FP_DSPARAM   = 1u << 2,  /* tag 3   DS Parameter Set */
    WIFI_IE_FP_HTCAP     = 1u << 3,  /* tag 45  HT Capabilities (WiFi 4) */
    WIFI_IE_FP_EXRATES   = 1u << 4,  /* tag 50  Extended Supported Rates */
    WIFI_IE_FP_INTERWORK = 1u << 5,  /* tag 107 Interworking (802.11u) */
    WIFI_IE_FP_EXTCAP    = 1u << 6,  /* tag 127 Extended Capabilities */
    WIFI_IE_FP_VHTCAP    = 1u << 7,  /* tag 191 VHT Capabilities (WiFi 5) */
    WIFI_IE_FP_VENDOR    = 1u << 8,  /* tag 221 Vendor Specific */
    WIFI_IE_FP_EXT_HE    = 1u << 9,  /* tag 255 Element ID Extension (HE, WiFi 6) */
};

/* Compact per-client fingerprint. Kept small (24 bytes) — this is stored
 * per tracked client (cli_t / probe_t), so it multiplies by table size. */
typedef struct {
    uint32_t tag_bitmap;                            /* 4  which well-known tags were seen */
    uint32_t vendor_oui[WIFI_IE_FP_MAX_VENDOR_OUI];  /* 16 up to 4 distinct 3-byte OUIs (low 24 bits) */
    uint8_t  vendor_n;                               /* 1  vendor_oui[] entries populated (0-4) */
    uint8_t  ie_count;                               /* 1  total IEs parsed (capped at 255) */
    uint8_t  ht_cap_byte;                            /* 1  first byte of tag 45 payload, 0 if absent */
    uint8_t  ext_cap_byte;                           /* 1  first byte of tag 127 payload, 0 if absent */
} wifi_ie_fp_t;                                      /* = 24 bytes total, no padding */

/* Parse the tagged-parameter chain of an 802.11 probe-request frame.
 *   frame     : full frame buffer, starting at the 24-byte MAC header
 *   frame_len : total bytes available in `frame` (e.g. rx_ctrl.sig_len)
 *   fp        : zeroed and populated on return
 *
 * Every step is bounds-checked against frame_len; a truncated or
 * malformed tag chain simply stops parsing early rather than reading
 * past the buffer. Allocation-free, bounded by frame_len — safe to call
 * from a WiFi RX callback. */
void wifi_ie_fp_parse(const uint8_t *frame, uint16_t frame_len, wifi_ie_fp_t *fp);

/* FNV-1a hash over the fingerprint's identifying fields (tag bitmap,
 * HT/extended-capability bytes, IE count, vendor OUIs). Two probes from
 * the same device model — even across MAC randomizations — hash the
 * same, so an operator can recognize "that's the same phone" after it
 * rotates its MAC. */
uint32_t wifi_ie_fp_hash(const wifi_ie_fp_t *fp);

/* Resolve a manufacturer name from the vendor-specific OUIs captured in
 * `fp`. Works despite MAC randomization since it reads the IE payload,
 * not the frame's source address. Prefers a real manufacturer (Apple,
 * Broadcom, Atheros/Qualcomm) over a generic standards-body OUI
 * (Microsoft WPS, Wi-Fi Alliance P2P, IEEE RSN) when both are present —
 * the generic ones appear on nearly every device and identify nothing.
 * Returns nullptr if no known OUI was seen. */
const char *wifi_ie_fp_vendor(const wifi_ie_fp_t *fp);

/* Coarse device-generation classification from capability IEs:
 *   HE (tag 255) present         -> "WiFi 6 device"
 *   VHT (tag 191) present        -> "WiFi 5 (phone/laptop)"
 *   HT (tag 45) present, no VHT  -> "WiFi 4 (IoT/older)"
 *   none of the above            -> "legacy/minimal (IoT)"
 * Always returns a non-null string; callers should only trust it once
 * `fp->ie_count > 0` (i.e. a probe request was actually parsed). */
const char *wifi_ie_fp_class(const wifi_ie_fp_t *fp);
