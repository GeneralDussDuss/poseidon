/*
 * c5_cmd — POSEIDON-side C5 remote-radio client.
 *
 * Sits on top of the existing ESP-NOW mesh. When a C5 node is seen
 * (HELLO with role=1 / has_5g=1), we auto-add it to the peer table
 * and expose an API for the UI to send commands and collect streamed
 * responses.
 *
 * Wire protocol v3 — shared with c5_node/main/proto.h. FLAG DAY:
 * both sides must run v3 together. v2 nodes will desync on response
 * routing because the RESP_* enum values moved 20-25 → 40-45.
 */
#pragma once

#include <Arduino.h>

#define C5_MAGIC   0x504F5345
#define C5_VERSION 4

enum {
    C5_TYPE_HELLO              = 1,

    /* Commands: S3 → C5 (10–29) */
    C5_TYPE_CMD_PING           = 10,
    C5_TYPE_CMD_SCAN_5G        = 11,
    C5_TYPE_CMD_SCAN_ZB        = 12,
    C5_TYPE_CMD_SCAN_2G        = 13,
    C5_TYPE_CMD_DEAUTH         = 14,
    C5_TYPE_CMD_STOP           = 15,
    C5_TYPE_CMD_PMKID          = 16,
    C5_TYPE_CMD_HS             = 17,    /* matches POSEI_TYPE_CMD_HS_CAPTURE (c5_node/main/proto.h) — net-002 */
    C5_TYPE_CMD_CLIENTS_HUNT   = 18,
    C5_TYPE_CMD_CLIENTS_AP     = 19,
    C5_TYPE_CMD_BEACON_SPAM    = 20,
    C5_TYPE_CMD_PROBE_SNIFF    = 21,
    C5_TYPE_CMD_DEAUTH_DETECT  = 22,
    C5_TYPE_CMD_KARMA          = 23,
    C5_TYPE_CMD_APCLONE        = 24,
    C5_TYPE_CMD_SPECTRUM       = 25,
    C5_TYPE_CMD_CIW            = 26,

    /* Responses: C5 → S3 (40–49) */
    C5_TYPE_RESP_PONG          = 40,
    C5_TYPE_RESP_AP            = 41,
    C5_TYPE_RESP_ZB            = 42,
    C5_TYPE_RESP_STATUS        = 43,
    C5_TYPE_RESP_PMKID         = 44,
    C5_TYPE_RESP_HS            = 45,
    C5_TYPE_RESP_STA           = 46,
    C5_TYPE_RESP_PROBE         = 47,
    C5_TYPE_RESP_DEAUTH_HIT    = 48,
    C5_TYPE_RESP_SPECTRUM      = 49,
};

/* ---- existing request/response structs (unchanged shape) ---- */

struct __attribute__((packed)) c5_deauth_req_t {
    uint8_t  bssid[6];
    uint8_t  channel;
    uint8_t  bcast_all;
    uint16_t duration_ms;
};

struct __attribute__((packed)) c5_pmkid_req_t {
    uint8_t  bssid[6];
    uint8_t  channel;
    uint16_t duration_ms;
};

struct __attribute__((packed)) c5_pmkid_t {
    uint8_t  bssid[6];
    uint8_t  sta[6];
    uint8_t  pmkid[16];
    uint8_t  ssid_len;
    char     ssid[33];
};

struct __attribute__((packed)) c5_hs_req_t {
    uint8_t  bssid[6];
    uint8_t  channel;
    uint16_t duration_ms;
};

/* v4: mirrors the slimmed posei_hs_t. MIC/SNonce/replay-counter live inside
 * eapol_m2; the ESSID is supplied by the S3 from its scan target. */
struct __attribute__((packed)) c5_hs_t {
    uint8_t  bssid[6];
    uint8_t  sta[6];
    uint8_t  anonce[32];
    uint16_t eapol_m2_len;
    uint8_t  eapol_m2[128];   /* full M2 802.1X frame (from the 802.1X header) */
};
static_assert(sizeof(c5_hs_t) == 174, "c5_hs_t must match posei_hs_t (174B)");

struct __attribute__((packed)) c5_msg_t {
    uint32_t magic;
    uint8_t  version;
    uint8_t  type;
    uint16_t seq;
    uint8_t  payload[230];
    uint8_t  payload_len;
};

struct __attribute__((packed)) c5_ap_t {
    uint8_t  bssid[6];
    uint8_t  channel;
    int8_t   rssi;
    uint8_t  auth;
    uint8_t  is_5g;
    char     ssid[33];
};

struct __attribute__((packed)) c5_zb_t {
    uint8_t  channel;
    int8_t   rssi;
    uint8_t  frame_type;
    uint16_t pan_id;
    uint16_t src_short;
    uint16_t dst_short;
    uint8_t  seq;
};

struct __attribute__((packed)) c5_hello_t {
    char     name[12];
    uint32_t heap_kb;
    uint8_t  role;
    uint8_t  has_5g;
    uint8_t  has_ieee802154;
};

/* ---- NEW v3 request/response structs ---- */

struct __attribute__((packed)) c5_clients_req_t {
    uint8_t  target_bssid[6];
    uint8_t  channel;
    uint16_t duration_ms;
    uint8_t  hop_all;
};

struct __attribute__((packed)) c5_sta_t {
    uint8_t  sta[6];
    uint8_t  bssid[6];
    uint8_t  channel;
    int8_t   rssi;
};

struct __attribute__((packed)) c5_probe_t {
    uint8_t  src[6];
    uint8_t  channel;
    int8_t   rssi;
    uint8_t  ssid_len;
    char     ssid[33];
};

struct __attribute__((packed)) c5_deauth_hit_t {
    uint8_t  src[6];
    uint8_t  dst[6];
    uint8_t  channel;
    uint8_t  reason;
};

struct __attribute__((packed)) c5_beacon_spam_req_t {
    uint8_t  channel;
    uint16_t duration_ms;
    uint8_t  mode;        /* 0=meme, 1=rickroll, 2=ssid_list */
    uint8_t  ssid_n;
    char     ssids[5][33];
};

struct __attribute__((packed)) c5_probe_sniff_req_t {
    uint8_t  channel;     /* 0 = hop */
    uint16_t duration_ms;
};

struct __attribute__((packed)) c5_deauth_detect_req_t {
    uint8_t  channel;
    uint16_t duration_ms;
};

struct __attribute__((packed)) c5_karma_req_t {
    uint8_t  channel;
    uint16_t duration_ms;
};

struct __attribute__((packed)) c5_apclone_req_t {
    uint8_t  channel;
    char     ssid[33];
    uint8_t  open;
    char     pass[33];
};

struct __attribute__((packed)) c5_spectrum_t {
    uint8_t  ch_count;
    struct __attribute__((packed)) {
        uint8_t ch;
        int8_t  peak_rssi;
    } ch[24];
};

struct __attribute__((packed)) c5_spectrum_req_t {
    uint16_t duration_ms;
};

struct __attribute__((packed)) c5_ciw_req_t {
    uint8_t  channel;
    uint16_t duration_ms;
    uint8_t  category;
    uint16_t interval_ms;
};

/* ---- API ---- */

bool c5_begin(void);
void c5_stop(void);

bool     c5_any_online(void);
int      c5_peer_count(void);
uint32_t c5_last_seen_ms(void);
const char *c5_peer_name(int idx);
void c5_peer_name_copy(int idx, char *out, int max);

/* Existing command senders. */
uint16_t c5_cmd_scan_5g(uint16_t duration_ms);
uint16_t c5_cmd_scan_zb(uint8_t channel);
uint16_t c5_cmd_stop(void);
uint16_t c5_cmd_ping(void);
uint16_t c5_cmd_deauth(const uint8_t bssid[6], uint8_t channel,
                       uint8_t bcast_all, uint16_t duration_ms);
uint16_t c5_cmd_pmkid(const uint8_t bssid[6], uint8_t channel, uint16_t duration_ms);
uint16_t c5_cmd_hs(const uint8_t bssid[6], uint8_t channel, uint16_t duration_ms);

/* New v3 command senders. */
uint16_t c5_cmd_clients_hunt(uint16_t duration_ms);
uint16_t c5_cmd_clients_ap(const uint8_t bssid[6], uint8_t channel, uint16_t duration_ms);
uint16_t c5_cmd_beacon_spam(uint8_t channel, uint16_t duration_ms,
                            uint8_t mode, uint8_t ssid_n, const char ssids[][33]);
uint16_t c5_cmd_probe_sniff(uint8_t channel, uint16_t duration_ms);
uint16_t c5_cmd_deauth_detect(uint8_t channel, uint16_t duration_ms);
uint16_t c5_cmd_karma(uint8_t channel, uint16_t duration_ms);
uint16_t c5_cmd_apclone(uint8_t channel, const char *ssid, bool open, const char *pass);
uint16_t c5_cmd_spectrum(uint16_t duration_ms);
uint16_t c5_cmd_ciw(uint8_t channel, uint16_t duration_ms,
                    uint8_t category, uint16_t interval_ms);

/* Launch the live C5 deauth dashboard — same UX as feat_wifi_deauth
 * but routes TX to the C5 via ESP-NOW. Persists until ESC; SPACE
 * pauses. broadcast=true → FF:FF:FF:FF:FF:FF deauth on the AP's
 * channel. */
struct ap_t;
void c5_deauth_dashboard(const ap_t &a, bool broadcast);

/* Status (RESP_STATUS values). */
uint32_t c5_status_frames(void);
uint8_t  c5_status_channel(void);

/* Debug. */
uint32_t c5_dbg_resp_ap_frames(void);
uint32_t c5_dbg_raw_ap_records(void);

/* Pull collected results. */
int c5_aps(c5_ap_t *out, int max);
int c5_zbs(c5_zb_t *out, int max);
int c5_pmkids(c5_pmkid_t *out, int max);
int c5_hss(c5_hs_t *out, int max);
int c5_stas(c5_sta_t *out, int max);
int c5_probes(c5_probe_t *out, int max);
int c5_deauth_hits(c5_deauth_hit_t *out, int max);
bool c5_spectrum_get(c5_spectrum_t *out);   /* latest single batch */
void c5_clear_results(void);
