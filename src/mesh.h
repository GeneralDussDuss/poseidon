/*
 * mesh — ESP-NOW device-to-device sync ("PigSync" inspiration).
 *
 * Purpose: when multiple POSEIDON/DAVEY devices are in range, they
 * discover each other and can relay captured data back to whichever
 * device has the operator. This is the enabler for the KRAKEN C2
 * mesh concept — drop devices around a target, control from one.
 *
 * Protocol v1 (intentionally simple):
 *   every 5s each node broadcasts a HELLO frame (node id, role, heap,
 *   gps fix if any) to the ESP-NOW broadcast address. Nodes maintain
 *   a peer table with last-seen timestamps. Peers not heard from in
 *   30s are evicted.
 *
 * Future: add ENCRYPTED data frames, shared PSK, targeted relay.
 */
#pragma once

#include <Arduino.h>

#define MESH_MAX_PEERS     8
#define MESH_HELLO_MS      5000
#define MESH_TIMEOUT_MS    30000

struct mesh_peer_t {
    uint8_t  mac[6];
    char     name[12];
    uint8_t  role;       /* user-defined, e.g. 0=scout, 1=operator */
    int8_t   rssi;
    uint32_t last_seen;
    uint32_t heap_kb;
    bool     has_gps;
    float    lat;
    float    lon;
};

bool mesh_begin(const char *node_name);
void mesh_stop(void);

/* Copy current peer table. Returns count written. */
int mesh_peers(mesh_peer_t *out, int max);

/* Total HELLOs sent since begin(). */
uint32_t mesh_tx_count(void);
uint32_t mesh_rx_count(void);
