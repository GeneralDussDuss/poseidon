/*
 * proto.h — POSEIDON ESP-NOW wire protocol v3.
 *
 * Shared between the S3 (POSEIDON) and the C5 (this node).
 *
 * v3 vs v2:
 *   - RESP_* renumbered from 20-25 → 40-45 to free 18-29 for new CMDs
 *   - +10 new CMD types: clients_hunt, clients_ap, beacon_spam,
 *     probe_sniff, deauth_detect, karma, apclone, spectrum, ciw
 *   - +4 new RESP types: sta, probe, deauth_hit, spectrum
 *   - Wardrive intentionally NOT in this cut.
 *
 * FLAG DAY: both sides must run v3 together. Mixed v2/v3 will misroute.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#define POSEI_MAGIC   0x504F5345  /* "POSE" */
#define POSEI_VERSION 4

enum {
    POSEI_TYPE_HELLO              = 1,

    /* Commands: S3 → C5 (10–29) */
    POSEI_TYPE_CMD_PING           = 10,
    POSEI_TYPE_CMD_SCAN_5G        = 11,
    POSEI_TYPE_CMD_SCAN_ZB        = 12,
    POSEI_TYPE_CMD_SCAN_2G        = 13,
    POSEI_TYPE_CMD_DEAUTH         = 14,
    POSEI_TYPE_CMD_STOP           = 15,
    POSEI_TYPE_CMD_PMKID          = 16,
    POSEI_TYPE_CMD_HS_CAPTURE     = 17,
    POSEI_TYPE_CMD_CLIENTS_HUNT   = 18,  /* all-channel STA hunt           */
    POSEI_TYPE_CMD_CLIENTS_AP     = 19,  /* per-AP STA list (channel-lock) */
    POSEI_TYPE_CMD_BEACON_SPAM    = 20,  /* 5G beacon spam                 */
    POSEI_TYPE_CMD_PROBE_SNIFF    = 21,  /* probe-req logger               */
    POSEI_TYPE_CMD_DEAUTH_DETECT  = 22,  /* passive deauth-frame detector  */
    POSEI_TYPE_CMD_KARMA          = 23,  /* sniff probe → SoftAP that SSID */
    POSEI_TYPE_CMD_APCLONE        = 24,  /* manual SoftAP with given SSID  */
    POSEI_TYPE_CMD_SPECTRUM       = 25,  /* per-channel peak RSSI sweep    */
    POSEI_TYPE_CMD_CIW            = 26,  /* SSID-injection beacon spam     */

    /* Responses: C5 → S3 (40–49) */
    POSEI_TYPE_RESP_PONG          = 40,
    POSEI_TYPE_RESP_AP            = 41,
    POSEI_TYPE_RESP_ZB            = 42,
    POSEI_TYPE_RESP_STATUS        = 43,
    POSEI_TYPE_RESP_PMKID         = 44,
    POSEI_TYPE_RESP_HS            = 45,
    POSEI_TYPE_RESP_STA           = 46,  /* one STA-BSSID pair             */
    POSEI_TYPE_RESP_PROBE         = 47,  /* one probe request              */
    POSEI_TYPE_RESP_DEAUTH_HIT    = 48,  /* one observed deauth frame      */
    POSEI_TYPE_RESP_SPECTRUM      = 49,  /* batched per-channel peak RSSI  */
};

#define POSEI_PAYLOAD_MAX 230

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  version;
    uint8_t  type;
    uint16_t seq;
    uint8_t  payload[POSEI_PAYLOAD_MAX];
    uint8_t  payload_len;
} posei_msg_t;

/* ---- existing payloads (unchanged) ---- */

typedef struct __attribute__((packed)) {
    uint8_t  bssid[6];
    uint8_t  channel;
    int8_t   rssi;
    uint8_t  auth;
    uint8_t  is_5g;       /* 0 = 2.4, 1 = 5, 2 = 6 */
    char     ssid[33];
} posei_ap_t;

typedef struct __attribute__((packed)) {
    uint8_t  channel;
    int8_t   rssi;
    uint8_t  frame_type;
    uint16_t pan_id;
    uint16_t src_short;
    uint16_t dst_short;
    uint8_t  seq;
} posei_zb_t;

typedef struct __attribute__((packed)) {
    char     name[12];
    uint32_t heap_kb;
    uint8_t  role;
    uint8_t  has_5g;
    uint8_t  has_ieee802154;
} posei_hello_t;

typedef struct __attribute__((packed)) {
    uint16_t duration_ms;
} posei_scan_req_t;

typedef struct __attribute__((packed)) {
    uint8_t  bssid[6];
    uint8_t  channel;
    uint8_t  bcast_all;
    uint16_t duration_ms;
} posei_deauth_req_t;

typedef struct __attribute__((packed)) {
    uint8_t  bssid[6];
    uint8_t  channel;
    uint16_t duration_ms;
} posei_pmkid_req_t;

typedef struct __attribute__((packed)) {
    uint8_t  bssid[6];
    uint8_t  sta[6];
    uint8_t  pmkid[16];
    uint8_t  ssid_len;
    char     ssid[33];
} posei_pmkid_t;

typedef struct __attribute__((packed)) {
    uint8_t  bssid[6];
    uint8_t  channel;
    uint16_t duration_ms;
} posei_hs_req_t;

/* v4: slimmed to what a hashcat-22000 line actually needs. The MIC, SNonce and
 * replay counter are all recoverable from eapol_m2, and the S3 supplies the
 * ESSID from its scan target — so those fields were pure wire redundancy that
 * pushed the struct past the 230-byte ESP-NOW payload. */
typedef struct __attribute__((packed)) {
    uint8_t  bssid[6];
    uint8_t  sta[6];
    uint8_t  anonce[32];
    uint16_t eapol_m2_len;
    uint8_t  eapol_m2[128];   /* full M2 802.1X frame (from the 802.1X header) */
} posei_hs_t;                 /* 174 bytes */
_Static_assert(sizeof(posei_hs_t) <= POSEI_PAYLOAD_MAX, "posei_hs_t must fit payload");

/* ---- NEW payloads (v3) ---- */

/* Clients hunt request. target_bssid=00..00 → hunt every AP.
 * hop_all=1 → cycle 36..165 on 5 GHz; =0 → channel-lock to `channel`. */
typedef struct __attribute__((packed)) {
    uint8_t  target_bssid[6];
    uint8_t  channel;
    uint16_t duration_ms;
    uint8_t  hop_all;
} posei_clients_req_t;

/* One STA-BSSID observation. */
typedef struct __attribute__((packed)) {
    uint8_t  sta[6];
    uint8_t  bssid[6];
    uint8_t  channel;
    int8_t   rssi;
} posei_sta_t;

/* One probe request. */
typedef struct __attribute__((packed)) {
    uint8_t  src[6];
    uint8_t  channel;
    int8_t   rssi;
    uint8_t  ssid_len;
    char     ssid[33];
} posei_probe_t;

/* One observed deauth/disassoc frame. */
typedef struct __attribute__((packed)) {
    uint8_t  src[6];
    uint8_t  dst[6];
    uint8_t  channel;
    uint8_t  reason;
} posei_deauth_hit_t;

/* Beacon spam request.
 *   mode 0 = built-in meme SSID list (firmware-side strings)
 *   mode 1 = single rickroll loop
 *   mode 2 = caller-supplied SSIDs from ssids[] array
 *   ssid_n up to 5 caller SSIDs (further can be sent across multiple reqs) */
typedef struct __attribute__((packed)) {
    uint8_t  channel;          /* 36..165 5G, or 1..13 2G */
    uint16_t duration_ms;      /* 0 = run until STOP */
    uint8_t  mode;
    uint8_t  ssid_n;
    char     ssids[5][33];
} posei_beacon_spam_req_t;

typedef struct __attribute__((packed)) {
    uint8_t  channel;          /* 0 = hop, else lock */
    uint16_t duration_ms;
} posei_probe_sniff_req_t;

typedef struct __attribute__((packed)) {
    uint8_t  channel;
    uint16_t duration_ms;
} posei_deauth_detect_req_t;

/* Karma: sniff probe, spin up SoftAP on requested SSID. */
typedef struct __attribute__((packed)) {
    uint8_t  channel;          /* SoftAP channel */
    uint16_t duration_ms;      /* 0 = until STOP (5-min safety) */
} posei_karma_req_t;

/* Manual AP clone — SoftAP with user-supplied SSID. */
typedef struct __attribute__((packed)) {
    uint8_t  channel;
    char     ssid[33];
    uint8_t  open;             /* 1 = open auth, 0 = WPA2 (psk in pass) */
    char     pass[33];
} posei_apclone_req_t;

/* Per-channel peak RSSI batch. Spectrum sweep streams these every
 * ~200 ms. ch_count up to 24 channels per batch (UNII-1+2A+3 = 24). */
typedef struct __attribute__((packed)) {
    uint8_t  ch_count;
    struct __attribute__((packed)) {
        uint8_t ch;
        int8_t  peak_rssi;
    } ch[24];
} posei_spectrum_t;

typedef struct __attribute__((packed)) {
    uint16_t duration_ms;      /* 0 = until STOP */
} posei_spectrum_req_t;

/* CIW (SSID-injection beacon spam). Built-in payload categories
 * selected by `category` index, firmware iterates payloads. */
typedef struct __attribute__((packed)) {
    uint8_t  channel;
    uint16_t duration_ms;
    uint8_t  category;         /* 0=cmd-inject, 1=fmt-string, 2=log4j, 3=xss, 4=path-trav, ... */
    uint16_t interval_ms;      /* SSID rotation cadence */
} posei_ciw_req_t;

/* ---- proto.c API ---- */
void proto_init_msg(posei_msg_t *m, uint8_t type);
void proto_send_broadcast(const posei_msg_t *m);
void proto_send_to(const uint8_t mac[6], const posei_msg_t *m);
int  proto_validate(const uint8_t *data, int len, posei_msg_t *out);
