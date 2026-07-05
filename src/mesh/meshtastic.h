/*
 * meshtastic — leaf-node Meshtastic participant.
 *
 * POSEIDON speaks the Meshtastic wire protocol well enough to:
 *   - Receive text messages, NodeInfo (User), and Position broadcasts
 *   - Build a live roster of seen nodes
 *   - Send broadcast text messages
 *   - Send direct-to-node text messages ("paging")
 *   - Optionally advertise our own NodeInfo + Position periodically
 *
 * NOT implemented (leaf-only):
 *   - Packet forwarding / rebroadcast — we always treat received packets
 *     as terminal and never retransmit other nodes' frames
 *   - Store-and-forward, routing ACKs, traceroute, telemetry
 *   - PKI encryption (AES-CCM + Curve25519) — only the default channel's
 *     AES-CTR symmetric path is supported
 *   - Multi-channel — hardcoded to the default LongFast primary channel
 *   - MQTT gateway
 *
 * Default channel configuration (cross-verified vs firmware v2.7.23):
 *   Name        "LongFast"
 *   PSK (AES)   D4 F1 BB 3A 20 29 07 59 F0 BC FF AB CF 4E 69 01
 *   Freq US     906.875 MHz (slot 19 from djb2("LongFast") mod 104)
 *   SF          11
 *   BW          250 kHz
 *   CR          4/5
 *   Preamble    16
 *   Sync word   0x2B
 *   CRC         on
 *   Header      explicit
 *   Chan hash   0x08  (xor("LongFast") ^ xor(defaultpsk))
 */
#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>

/* ===== Constants pulled from Meshtastic firmware ===== */

#define MESH_FREQ_MHZ            906.875f
#define MESH_BW_KHZ              250.0f
#define MESH_SF                  11
#define MESH_CR                  5        /* RadioLib "cr" param, 4/5 */
#define MESH_PREAMBLE            16
#define MESH_SYNC_WORD           0x2B
#define MESH_TX_POWER_DBM        17       /* conservative; US max is 30 */

#define MESH_CHANNEL_HASH        0x08     /* default LongFast + defaultpsk */
#define MESH_BROADCAST_NODEID    0xFFFFFFFFu
#define MESH_HOP_RELIABLE        3u
#define MESH_HOP_MAX             7u
#define MESH_DEFAULT_PORTNUM     0
/* Max text-message payload. Budget: 255 LoRa max - 16 header = 239 ciphertext.
 * A Data proto wrapping an N-byte payload adds: 1 (portnum tag) + 1 (portnum varint)
 * + 1 (payload tag) + 1-2 (length varint) = 4-5 bytes. Direct messages add another
 * 10 bytes for dest+source fixed32s. Cap at 200 so DMs also fit comfortably. */
#define MESH_MAX_PAYLOAD         200

/* Packet-header flag layout (byte 12 of the 16-byte header) */
#define MESH_FLAGS_HOP_LIMIT_MASK   0x07
#define MESH_FLAGS_WANT_ACK_MASK    0x08
#define MESH_FLAGS_VIA_MQTT_MASK    0x10
#define MESH_FLAGS_HOP_START_MASK   0xE0
#define MESH_FLAGS_HOP_START_SHIFT  5

/* PortNum values (portnums.proto) */
#define MESH_PORT_TEXT_MESSAGE   1
#define MESH_PORT_POSITION       3
#define MESH_PORT_NODEINFO       4
#define MESH_PORT_ROUTING        5
#define MESH_PORT_TELEMETRY      67
#define MESH_PORT_TRACEROUTE_APP 70

/* ===== Public types ===== */

struct mesh_node_t {
    uint32_t  id;
    char      long_name[40];
    char      short_name[8];
    int8_t    last_snr;
    int16_t   last_rssi;
    uint8_t   hops;
    uint32_t  last_seen_ms;
    bool      has_position;
    int32_t   latitude_i;   /* degrees * 1e7 */
    int32_t   longitude_i;
    int32_t   altitude;     /* meters */
};

struct mesh_message_t {
    uint32_t  from;
    uint32_t  to;
    uint8_t   hops;
    int8_t    snr;
    int16_t   rssi;
    uint32_t  when_ms;
    char      text[MESH_MAX_PAYLOAD + 1];
    uint16_t  text_len;
};

/* ===== Channel config (NVS-backed, set via mesh_channel feature) ===== */

/* The active channel name used for frequency derivation and channel hash.
 * Empty string = "LongFast" default. Read from NVS key "ch_name". */
const char *mesh_active_channel_name(void);

/* The active 16-byte AES PSK. Either loaded from NVS key "ch_psk" or
 * the hardcoded default. */
const uint8_t *mesh_active_psk(void);

/* The computed channel hash (byte 13 in packet headers). */
uint8_t mesh_active_channel_hash(void);

/* The TX/RX frequency in MHz, derived from djb2(channel_name) mod 104. */
float mesh_active_freq_mhz(void);

/* ===== Lifecycle ===== */

/* Bring up the Meshtastic stack on top of lora_hw. Call radio_switch(RADIO_LORA)
 * first. Returns true on success. Starts a background RX task that keeps the
 * radio in receive mode and populates the node + message queues.
 *
 * Reads channel config (name + PSK) from NVS on startup so custom
 * channels are supported. Use feat_mesh_channel() to configure. */
bool mesh_begin(void);
void mesh_end(void);
bool mesh_is_up(void);

/* Our node identity, derived from the WiFi MAC. */
uint32_t mesh_own_node_id(void);
const char *mesh_own_long_name(void);
const char *mesh_own_short_name(void);

/* ===== TX ===== */

/* Broadcast a text message to everyone on the default channel.
 * Returns 0 on failure, or the packet_id on success (truthy in boolean
 * context for backward compatibility with existing callers). */
uint32_t mesh_send_broadcast_text(const char *text);

/* Send a text message to a specific node (paging).
 * Returns 0 on failure, or the packet_id on success. */
uint32_t mesh_send_direct_text(uint32_t dest_node_id, const char *text);

/* Broadcast our NodeInfo (User proto) so others add us to their rosters. */
bool mesh_send_nodeinfo(void);

/* Broadcast our current GPS position. Returns false if no GPS fix. */
bool mesh_send_position(void);

/* ===== RX helpers for UI features ===== */

/* Node roster — array is owned by the mesh layer, don't mutate. `count_out`
 * is set to the number of valid entries. Safe to call from main loop. */
const mesh_node_t *mesh_nodes(int *count_out);

/* Message log — newest-last ring of the last N received text messages.
 * `count_out` is how many are valid. Entries are mesh_message_t.
 *
 * DEPRECATED for UI use: returns a raw pointer + index into an internal
 * ring buffer, which is (a) not chronologically ordered after the ring
 * wraps, and (b) unsafe against concurrent push_message() from the RX
 * task without holding a mutex. Prefer mesh_snapshot_messages() below. */
const mesh_message_t *mesh_messages(int *count_out);

/* Copy the most-recent `max` messages into `out[]` under the ring's
 * mutex. Writes oldest-first ordering so the UI can render top-to-
 * bottom naturally. Returns number actually copied (≤ max, ≤ ring
 * depth). Thread-safe against the RX task. */
int mesh_snapshot_messages(mesh_message_t *out, int max);

/* Drain pending "new message" notification flag — returns true if a new
 * message arrived since the last call. UI features poll this to know
 * when to refresh. */
bool mesh_drain_new_message(void);

/* Clear in-memory message log. Doesn't touch node roster. */
void mesh_clear_messages(void);

/* ===== ACK delivery tracking ===== */

enum mesh_ack_status_t : uint8_t {
    MESH_ACK_PENDING,   /* waiting for ACK, retry in progress */
    MESH_ACK_OK,        /* ACK received — message delivered */
    MESH_ACK_FAILED     /* all retries exhausted — delivery failed */
};

/* Query delivery status of a sent message by its packet_id (the value
 * returned by mesh_send_broadcast_text / mesh_send_direct_text).
 * Returns MESH_ACK_FAILED if the id is unknown or expired. */
mesh_ack_status_t mesh_ack_status(uint32_t packet_id);

/* ===== Position reporting toggle ===== */

/* When enabled, mesh layer broadcasts NodeInfo every ~30min and Position
 * every ~15min (if GPS fix available). Off by default. */
void mesh_set_position_reporting(bool on);
bool mesh_position_reporting(void);

/* Call periodically from a feature's main loop to drive background
 * NodeInfo + Position broadcasts. Safe to call every tick. */
extern "C" void mesh_tick(void);

/* ===== Traceroute ===== */

#define MESH_TRACEROUTE_MAX_HOPS  8

struct mesh_traceroute_result_t {
    uint32_t route[MESH_TRACEROUTE_MAX_HOPS];  /* node IDs along path */
    int8_t   snr[MESH_TRACEROUTE_MAX_HOPS];    /* SNR per hop (dB, rounded) */
    int      hops;                               /* valid entries in route[] */
    bool     complete;                           /* true when result ready */
};

/* Send a traceroute request to dest_node_id. The mesh firmware on
 * intermediate Meshtastic nodes will populate the route as the packet
 * traverses the mesh, and the destination sends back the full path.
 * Returns true if the request packet was transmitted. */
bool mesh_send_traceroute(uint32_t dest_node_id);

/* Check if a traceroute response has arrived. Copies the result into
 * *out and returns true; clears the internal result. Returns false if
 * no result is pending. */
bool mesh_traceroute_result(mesh_traceroute_result_t *out);

/* Clear any pending traceroute result. Call before starting a new
 * traceroute to avoid stale data. */
void mesh_traceroute_clear(void);
