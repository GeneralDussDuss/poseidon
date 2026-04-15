/*
 * proto.h — POSEIDON ESP-NOW wire protocol v2.
 *
 * Shared between the S3 (POSEIDON) and the C5 (this node).
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#define POSEI_MAGIC   0x504F5345  /* "POSE" */
#define POSEI_VERSION 2

enum {
    POSEI_TYPE_HELLO        = 1,

    /* Commands: S3 → C5 */
    POSEI_TYPE_CMD_PING     = 10,
    POSEI_TYPE_CMD_SCAN_5G  = 11,
    POSEI_TYPE_CMD_SCAN_ZB  = 12,
    POSEI_TYPE_CMD_SCAN_2G  = 13,
    POSEI_TYPE_CMD_DEAUTH   = 14,
    POSEI_TYPE_CMD_STOP     = 15,

    /* Responses: C5 → S3 */
    POSEI_TYPE_RESP_PONG    = 20,
    POSEI_TYPE_RESP_AP      = 21,
    POSEI_TYPE_RESP_ZB      = 22,
    POSEI_TYPE_RESP_STATUS  = 23,
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

/* Payload for RESP_AP: up to 4 AP records per batch, streamed. */
typedef struct __attribute__((packed)) {
    uint8_t  bssid[6];
    uint8_t  channel;
    int8_t   rssi;
    uint8_t  auth;
    uint8_t  is_5g;       /* 0 = 2.4, 1 = 5, 2 = 6 */
    char     ssid[33];
} posei_ap_t;

/* Payload for RESP_ZB: one 802.15.4 frame summary. */
typedef struct __attribute__((packed)) {
    uint8_t  channel;
    int8_t   rssi;
    uint8_t  frame_type;   /* beacon, data, ack, cmd */
    uint16_t pan_id;
    uint16_t src_short;
    uint16_t dst_short;
    uint8_t  seq;
} posei_zb_t;

/* Payload for HELLO. */
typedef struct __attribute__((packed)) {
    char     name[12];
    uint32_t heap_kb;
    uint8_t  role;         /* 0 = s3/commander, 1 = c5/node */
    uint8_t  has_5g;
    uint8_t  has_ieee802154;
} posei_hello_t;

/* Payload for CMD_SCAN_5G / CMD_SCAN_2G. */
typedef struct __attribute__((packed)) {
    uint16_t duration_ms;
} posei_scan_req_t;

/* Payload for CMD_DEAUTH (works on 2.4 OR 5 GHz channels — only the
 * C5 can do 5 GHz). bssid=00..00 + bcast=1 means a broadcast deauth
 * to every SSID seen on `channel`. */
typedef struct __attribute__((packed)) {
    uint8_t  bssid[6];
    uint8_t  channel;       /* 1..14 (2.4 GHz) or 36..165 (5 GHz) */
    uint8_t  bcast_all;     /* 1 = ignore bssid, deauth every AP on channel */
    uint16_t duration_ms;
} posei_deauth_req_t;

void proto_init_msg(posei_msg_t *m, uint8_t type);
void proto_send_broadcast(const posei_msg_t *m);
void proto_send_to(const uint8_t mac[6], const posei_msg_t *m);
int  proto_validate(const uint8_t *data, int len, posei_msg_t *out);
