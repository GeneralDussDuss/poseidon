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
    C5_TYPE_CMD_STOP    = 15,
    C5_TYPE_RESP_PONG   = 20,
    C5_TYPE_RESP_AP     = 21,
    C5_TYPE_RESP_ZB     = 22,
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
const char *c5_peer_name(int idx);

/* Send a command to every known C5. Returns the seq id used. */
uint16_t c5_cmd_scan_5g(uint16_t duration_ms);
uint16_t c5_cmd_scan_zb(uint8_t channel);  /* 0xFF = hop */
uint16_t c5_cmd_stop(void);
uint16_t c5_cmd_ping(void);

/* Pull collected results. Returns count written. */
int c5_aps(c5_ap_t *out, int max);
int c5_zbs(c5_zb_t *out, int max);
void c5_clear_results(void);
