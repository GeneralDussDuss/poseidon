/*
 * c5_cmd — POSEIDON-side C5 remote-radio client.
 *
 * Sits on top of the existing ESP-NOW mesh. When a C5 node is seen
 * (HELLO with role=1 / has_5g=1), we auto-add it to the peer table
 * and expose an API for the UI to send commands (SCAN_5G, SCAN_ZB,
 * DEAUTH_5G) and collect streamed responses.
 *
 * Wire protocol is shared with c5_node/main/proto.h.
 */
#pragma once

#include <Arduino.h>

#define C5_MAGIC   0x504F5345
#define C5_VERSION 2

enum {
    C5_TYPE_HELLO       = 1,
    C5_TYPE_CMD_PING    = 10,
    C5_TYPE_CMD_SCAN_5G = 11,
    C5_TYPE_CMD_SCAN_ZB = 12,
    C5_TYPE_CMD_SCAN_2G = 13,
    C5_TYPE_CMD_DEAUTH  = 14,
    C5_TYPE_CMD_STOP    = 15,
    C5_TYPE_CMD_PMKID   = 16,
    C5_TYPE_CMD_HS      = 17,
    C5_TYPE_RESP_PONG   = 20,
    C5_TYPE_RESP_AP     = 21,
    C5_TYPE_RESP_ZB     = 22,
    C5_TYPE_RESP_STATUS = 23,
    C5_TYPE_RESP_PMKID  = 24,
    C5_TYPE_RESP_HS     = 25,
};

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

/* Same req shape as PMKID — BSSID + channel + duration. */
struct __attribute__((packed)) c5_hs_req_t {
    uint8_t  bssid[6];
    uint8_t  channel;
    uint16_t duration_ms;
};

/* Captured (M1, M2) tuple from a 5 GHz 4-way. Enough to emit a
 * hashcat 22000 "WPA*02*" line on the POSEIDON side. */
struct __attribute__((packed)) c5_hs_t {
    uint8_t  bssid[6];
    uint8_t  sta[6];
    uint8_t  anonce[32];        /* M1 */
    uint8_t  snonce[32];        /* M2 */
    uint8_t  mic[16];           /* M2 */
    uint8_t  replay_counter[8];
    uint16_t eapol_m2_len;
    uint8_t  eapol_m2[128];
    uint8_t  ssid_len;
    char     ssid[33];
};

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

/* Start the c5_cmd ESP-NOW layer. Must be called after WiFi is up.
 * Replaces the mesh's recv handler with our dispatcher. */
bool c5_begin(void);
void c5_stop(void);

/* Is a C5 node currently online? (HELLO seen in the last 15s) */
bool     c5_any_online(void);
int      c5_peer_count(void);
uint32_t c5_last_seen_ms(void);
const char *c5_peer_name(int idx);   /* direct, racy — prefer _copy */
void c5_peer_name_copy(int idx, char *out, int max);

/* Send a command to every known C5. Returns the seq id used. */
uint16_t c5_cmd_scan_5g(uint16_t duration_ms);
uint16_t c5_cmd_scan_zb(uint8_t channel);  /* 0xFF = hop */
uint16_t c5_cmd_stop(void);
uint16_t c5_cmd_ping(void);
uint16_t c5_cmd_deauth(const uint8_t bssid[6], uint8_t channel,
                       uint8_t bcast_all, uint16_t duration_ms);
uint16_t c5_cmd_pmkid(const uint8_t bssid[6], uint8_t channel, uint16_t duration_ms);
/* 4-way handshake capture on the C5. Promisc-listens on `channel` for
 * M1/M2 frames addressed to `bssid`, streams RESP_HS tuples back. */
uint16_t c5_cmd_hs(const uint8_t bssid[6], uint8_t channel, uint16_t duration_ms);

/* Latest RESP_STATUS values from the C5 (for live attack dashboards). */
uint32_t c5_status_frames(void);
uint8_t  c5_status_channel(void);

/* Debug counters: how many RESP_AP frames came in, total raw AP
 * records (pre-dedup). Cleared by c5_clear_results(). */
uint32_t c5_dbg_resp_ap_frames(void);
uint32_t c5_dbg_raw_ap_records(void);

/* Pull collected results. Returns count written. */
int c5_aps(c5_ap_t *out, int max);
int c5_zbs(c5_zb_t *out, int max);
int c5_pmkids(c5_pmkid_t *out, int max);
int c5_hss(c5_hs_t *out, int max);
void c5_clear_results(void);
