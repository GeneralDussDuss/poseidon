/*
 * meshtastic_internal — types shared between the meshtastic_*.cpp files.
 * Not for feature-layer consumption.
 */
#pragma once

#include "meshtastic.h"

/* ==================== protobuf codec ==================== */

struct mesh_buf_t {
    uint8_t *data;
    size_t   cap;
    size_t   len;
};

struct mesh_data_t {
    uint32_t  portnum;
    uint8_t   payload[MESH_MAX_PAYLOAD];
    uint16_t  payload_len;
    bool      want_response;
    uint32_t  dest;
    uint32_t  source;
    uint32_t  request_id;
    uint32_t  reply_id;
    uint32_t  bitfield;
};

struct mesh_user_t {
    char      id[16];          /* "!xxxxxxxx" string form of node id */
    char      long_name[40];
    char      short_name[8];
    uint32_t  hw_model;
    bool      is_licensed;
    uint32_t  role;
};

struct mesh_position_t {
    int32_t   latitude_i;      /* degrees * 1e7 */
    int32_t   longitude_i;
    int32_t   altitude;        /* meters */
    uint32_t  time;            /* epoch seconds */
    uint32_t  location_source;
    uint32_t  sats_in_view;
};

bool mesh_pb_encode_data(mesh_buf_t *b, const mesh_data_t *d);
bool mesh_pb_decode_data(const uint8_t *buf, size_t len, mesh_data_t *out);
bool mesh_pb_encode_user(mesh_buf_t *b, const mesh_user_t *u);
bool mesh_pb_decode_user(const uint8_t *buf, size_t len, mesh_user_t *out);
bool mesh_pb_encode_position(mesh_buf_t *b, const mesh_position_t *pos);
bool mesh_pb_decode_position(const uint8_t *buf, size_t len, mesh_position_t *out);

/* ==================== crypto ==================== */

/* AES-CTR-128 encrypt/decrypt in place. Key is 16 bytes. The counter block
 * layout is (per Meshtastic firmware v2.7.23 initNonce + setCounterSize(4)):
 *   bytes  0..3  packet_id low  32 bits (LE)
 *   bytes  4..7  packet_id high 32 bits (LE, zero for us — 32-bit IDs)
 *   bytes  8..11 from_node (32-bit LE)
 *   bytes 12..15 AES-CTR block counter (starts 0, increments per 16-byte block)
 */
void mesh_crypto_ctr(const uint8_t key[16],
                     uint32_t packet_id,
                     uint32_t from_node,
                     uint8_t *data,
                     size_t len);
