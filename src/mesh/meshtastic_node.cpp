/*
 * meshtastic_node — main Meshtastic pipeline: init, header framing, RX task,
 * TX helpers, node roster, message log.
 *
 * Sits on top of lora_hw but configures the SX1262 per Meshtastic spec
 * (SF11 BW250 CR4/5 preamble 16 sync 0x2B CRC on) which differs from
 * POSEIDON's other LoRa uses.
 */
#include "meshtastic.h"
#include "meshtastic_internal.h"
#include "../lora_hw.h"
#include "../gps.h"
#include <Arduino.h>
#include <RadioLib.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_random.h>
#include <esp_idf_version.h>
#if ESP_IDF_VERSION_MAJOR >= 5
  #include <esp_mac.h>   /* esp_read_mac + ESP_MAC_WIFI_STA moved here in IDF 5.x */
#endif
#include <string.h>
#include <Preferences.h>

#define MESH_MAX_NODES     32
#define MESH_MSG_RING      24

static const uint8_t DEFAULT_PSK[16] = {
    0xD4, 0xF1, 0xBB, 0x3A, 0x20, 0x29, 0x07, 0x59,
    0xF0, 0xBC, 0xFF, 0xAB, 0xCF, 0x4E, 0x69, 0x01
};

/* ==================== channel config (NVS-backed) ==================== */

static char     s_channel_name[32] = {0};  /* empty = "LongFast" */
static uint8_t  s_active_psk[16] = {0};
static uint8_t  s_channel_hash = MESH_CHANNEL_HASH;
static float    s_channel_freq = MESH_FREQ_MHZ;
static bool     s_channel_config_loaded = false;

/* djb2 — same algorithm Meshtastic uses for channel name hashing. */
static uint32_t mesh_djb2(const char *s)
{
    uint32_t h = 5381;
    while (*s) h = ((h << 5) + h) + (uint8_t)*s++;
    return h;
}

/* XOR of PSK bytes. */
static uint8_t psk_xor(const uint8_t psk[16])
{
    uint8_t x = 0;
    for (int i = 0; i < 16; i++) x ^= psk[i];
    return x;
}

/* Load channel config from NVS. Called once by mesh_begin() or on first
 * getter access so the channel config screen can display active values
 * even before the mesh is up. */
static void load_channel_config(void)
{
    if (s_channel_config_loaded) return;

    /* Defaults. */
    memcpy(s_active_psk, DEFAULT_PSK, 16);
    s_channel_name[0] = '\0';

    Preferences p;
    if (p.begin("poseidon", true)) {
        String nm = p.getString("ch_name", "");
        String pk = p.getString("ch_psk", "");
        p.end();

        if (nm.length() > 0 && nm.length() < sizeof(s_channel_name)) {
            strlcpy(s_channel_name, nm.c_str(), sizeof(s_channel_name));
        }

        if (pk.length() > 0) {
            /* Try hex decode first (32 hex chars = 16 bytes). */
            const char *hex = pk.c_str();
            int hlen = strlen(hex);
            bool is_hex = (hlen == 32);
            if (is_hex) {
                for (int i = 0; i < hlen; i++) {
                    char c = hex[i];
                    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                          (c >= 'A' && c <= 'F'))) {
                        is_hex = false;
                        break;
                    }
                }
            }
            if (is_hex) {
                for (int i = 0; i < 16; i++) {
                    char buf[3] = { hex[i*2], hex[i*2+1], '\0' };
                    s_active_psk[i] = (uint8_t)strtoul(buf, nullptr, 16);
                }
            } else {
                /* Hash text via djb2, spread across 16 bytes. */
                memset(s_active_psk, 0, 16);
                for (int pass = 0; pass < 4; pass++) {
                    uint32_t h = 5381;
                    for (const char *ch = hex; *ch; ch++)
                        h = ((h << 5) + h) + (uint8_t)*ch + pass;
                    s_active_psk[pass*4]   = (uint8_t)(h);
                    s_active_psk[pass*4+1] = (uint8_t)(h >> 8);
                    s_active_psk[pass*4+2] = (uint8_t)(h >> 16);
                    s_active_psk[pass*4+3] = (uint8_t)(h >> 24);
                }
            }
        }
    }

    /* Derive channel hash: XOR of djb2(name) bytes XOR'd with XOR(psk). */
    const char *eff = s_channel_name[0] ? s_channel_name : "LongFast";
    uint32_t dh = mesh_djb2(eff);
    s_channel_hash = (uint8_t)dh ^ (uint8_t)(dh >> 8)
                   ^ (uint8_t)(dh >> 16) ^ (uint8_t)(dh >> 24);
    s_channel_hash ^= psk_xor(s_active_psk);

    /* Frequency from channel name: djb2 mod 104 slots. */
    s_channel_freq = 903.08f + (mesh_djb2(eff) % 104) * 2.16f;

    s_channel_config_loaded = true;
    Serial.printf("[mesh-ch] name='%s' hash=0x%02X freq=%.3f MHz\n",
                  eff, s_channel_hash, s_channel_freq);
}

const char   *mesh_active_channel_name(void) { load_channel_config(); return s_channel_name; }
const uint8_t *mesh_active_psk(void)         { load_channel_config(); return s_active_psk; }
uint8_t        mesh_active_channel_hash(void) { load_channel_config(); return s_channel_hash; }
float          mesh_active_freq_mhz(void)    { load_channel_config(); return s_channel_freq; }

/* ==================== state ==================== */

static bool              s_up = false;
static uint32_t          s_own_id = 0;
static char              s_own_long[40] = {0};
static char              s_own_short[8] = {0};
static uint32_t          s_packet_counter = 0;

/* Heap-allocated on mesh_begin(), freed on mesh_end(). Was 9.5KB of
 * static BSS, which ate DRAM 24/7 even when mesh wasn't active and
 * contributed to WiFi.scanNetworks hitting ENOMEM under cumulative
 * heap pressure. */
static mesh_node_t       *s_nodes = nullptr;
static int                s_node_count = 0;
static portMUX_TYPE       s_nodes_mux = portMUX_INITIALIZER_UNLOCKED;

static mesh_message_t    *s_msgs = nullptr;
static int                s_msg_head = 0;
static int                s_msg_count = 0;
static volatile bool      s_new_msg = false;
static portMUX_TYPE       s_msgs_mux = portMUX_INITIALIZER_UNLOCKED;

static TaskHandle_t      s_rx_task = nullptr;
static volatile bool     s_rx_task_alive = false;
static volatile bool     s_rx_task_stop = false;

static bool              s_position_reporting = false;
static uint32_t          s_last_nodeinfo_ms = 0;
static uint32_t          s_last_position_ms = 0;

/* ==================== ACK tracking ==================== */

#define MESH_ACK_RING    8   /* max pending ACKs — small for no-PSRAM */

struct mesh_ack_entry_t {
    uint32_t  packet_id;
    uint32_t  dest_node;     /* MESH_BROADCAST_NODEID or specific node */
    uint32_t  tx_time_ms;
    uint8_t   retries;
    uint8_t   status;        /* mesh_ack_status_t */
    uint8_t   text_len;
    char      text[MESH_MAX_PAYLOAD];
};

static mesh_ack_entry_t  s_ack_ring[MESH_ACK_RING];
static int               s_ack_head  = 0;
static int               s_ack_count = 0;
static portMUX_TYPE      s_ack_mux   = portMUX_INITIALIZER_UNLOCKED;

/* Traceroute result — written by the RX task, read by the UI. */
static volatile bool            s_traceroute_ready = false;
static mesh_traceroute_result_t s_traceroute_result = {};

/* ==================== identity ==================== */

static void derive_identity(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    /* Per NodeDB::pickNewNodeNum: (mac[2]<<24)|(mac[3]<<16)|(mac[4]<<8)|mac[5] */
    s_own_id = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16)
             | ((uint32_t)mac[4] << 8)  | (uint32_t)mac[5];
    if (s_own_id == 0 || s_own_id == MESH_BROADCAST_NODEID) {
        s_own_id = 0x50534400u | (mac[5] & 0xFF);  /* "PSD" + last MAC byte */
    }
    snprintf(s_own_long,  sizeof(s_own_long),  "POSEIDON %02X%02X", mac[4], mac[5]);
    snprintf(s_own_short, sizeof(s_own_short), "PS%02X", mac[5]);
}

uint32_t mesh_own_node_id(void)       { return s_own_id; }
const char *mesh_own_long_name(void)  { return s_own_long; }
const char *mesh_own_short_name(void) { return s_own_short; }

/* ==================== packet id ==================== */

static uint32_t next_packet_id(void)
{
    /* Firmware pattern: 10-bit counter (non-zero) | 22-bit random high */
    s_packet_counter = (s_packet_counter + 1) & 0x3FFu;
    if (s_packet_counter == 0) s_packet_counter = 1;
    uint32_t hi = esp_random() & 0x3FFFFFu;
    uint32_t id = (hi << 10) | s_packet_counter;
    if (id == 0) id = 1;
    return id;
}

/* ==================== node roster ==================== */

static int find_node(uint32_t id)
{
    for (int i = 0; i < s_node_count; i++) {
        if (s_nodes[i].id == id) return i;
    }
    return -1;
}

static int upsert_node(uint32_t id)
{
    int idx = find_node(id);
    if (idx >= 0) return idx;
    if (s_node_count < MESH_MAX_NODES) {
        idx = s_node_count++;
    } else {
        /* Evict oldest */
        idx = 0;
        uint32_t oldest = s_nodes[0].last_seen_ms;
        for (int i = 1; i < s_node_count; i++) {
            if (s_nodes[i].last_seen_ms < oldest) {
                oldest = s_nodes[i].last_seen_ms;
                idx = i;
            }
        }
    }
    memset(&s_nodes[idx], 0, sizeof(s_nodes[idx]));
    s_nodes[idx].id = id;
    return idx;
}

const mesh_node_t *mesh_nodes(int *count_out)
{
    if (count_out) *count_out = s_nodes ? s_node_count : 0;
    return s_nodes;
}

/* ==================== message log ==================== */

static void push_message(const mesh_message_t &m)
{
    portENTER_CRITICAL(&s_msgs_mux);
    s_msgs[s_msg_head] = m;
    s_msg_head = (s_msg_head + 1) % MESH_MSG_RING;
    if (s_msg_count < MESH_MSG_RING) s_msg_count++;
    s_new_msg = true;
    portEXIT_CRITICAL(&s_msgs_mux);
}

const mesh_message_t *mesh_messages(int *count_out)
{
    if (count_out) *count_out = s_msgs ? s_msg_count : 0;
    return s_msgs;
}

int mesh_snapshot_messages(mesh_message_t *out, int max)
{
    if (!out || max <= 0) return 0;
    portENTER_CRITICAL(&s_msgs_mux);
    if (!s_msgs) { portEXIT_CRITICAL(&s_msgs_mux); return 0; }
    int n = s_msg_count < max ? s_msg_count : max;
    /* Oldest index is (s_msg_head - s_msg_count) mod ring size. Walking
     * forward `n` steps gives oldest → newest ordering. Once the ring
     * wraps (s_msg_count == RING), s_msg_head is the oldest slot. */
    int start = (s_msg_head - s_msg_count + MESH_MSG_RING) % MESH_MSG_RING;
    if (s_msg_count < n) start = (s_msg_head - s_msg_count + MESH_MSG_RING) % MESH_MSG_RING;
    /* If asked for fewer than we have, grab the newest n by advancing start. */
    if (s_msg_count > n) start = (s_msg_head - n + MESH_MSG_RING) % MESH_MSG_RING;
    for (int i = 0; i < n; ++i) {
        out[i] = s_msgs[(start + i) % MESH_MSG_RING];
    }
    portEXIT_CRITICAL(&s_msgs_mux);
    return n;
}

bool mesh_drain_new_message(void)
{
    bool v;
    portENTER_CRITICAL(&s_msgs_mux);
    v = s_new_msg;
    s_new_msg = false;
    portEXIT_CRITICAL(&s_msgs_mux);
    return v;
}

void mesh_clear_messages(void)
{
    portENTER_CRITICAL(&s_msgs_mux);
    s_msg_count = 0;
    s_msg_head = 0;
    s_new_msg = false;
    portEXIT_CRITICAL(&s_msgs_mux);
}

/* ==================== ACK helpers ==================== */

/* Add an entry to the pending-ACK ring. Called from main loop context. */
static void ack_add(uint32_t packet_id, uint32_t dest_node,
                    const char *text, uint16_t text_len)
{
    portENTER_CRITICAL(&s_ack_mux);
    int idx;
    if (s_ack_count < MESH_ACK_RING) {
        idx = (s_ack_head + s_ack_count) % MESH_ACK_RING;
        s_ack_count++;
    } else {
        /* Evict oldest (head) — no room. */
        idx = s_ack_head;
        s_ack_head = (s_ack_head + 1) % MESH_ACK_RING;
    }
    s_ack_ring[idx].packet_id  = packet_id;
    s_ack_ring[idx].dest_node  = dest_node;
    s_ack_ring[idx].tx_time_ms = millis();
    s_ack_ring[idx].retries    = 0;
    s_ack_ring[idx].status     = MESH_ACK_PENDING;
    uint8_t n = text_len;
    if (n > MESH_MAX_PAYLOAD) n = MESH_MAX_PAYLOAD;
    s_ack_ring[idx].text_len = n;
    memcpy(s_ack_ring[idx].text, text, n);
    portEXIT_CRITICAL(&s_ack_mux);
}

/* Match an incoming ACK request_id against our pending list.
 * Returns true and marks the entry as OK if found. */
static bool ack_match(uint32_t request_id)
{
    portENTER_CRITICAL(&s_ack_mux);
    for (int i = 0; i < s_ack_count; i++) {
        int idx = (s_ack_head + i) % MESH_ACK_RING;
        if (s_ack_ring[idx].packet_id == request_id &&
            s_ack_ring[idx].status == MESH_ACK_PENDING) {
            s_ack_ring[idx].status = MESH_ACK_OK;
            portEXIT_CRITICAL(&s_ack_mux);
            Serial.printf("[mesh-ack] ACKed id=0x%08x\n", (unsigned)request_id);
            return true;
        }
    }
    portEXIT_CRITICAL(&s_ack_mux);
    return false;
}

/* Query the delivery status of a given packet_id. */
mesh_ack_status_t mesh_ack_status(uint32_t packet_id)
{
    mesh_ack_status_t result = MESH_ACK_FAILED;
    portENTER_CRITICAL(&s_ack_mux);
    for (int i = 0; i < s_ack_count; i++) {
        int idx = (s_ack_head + i) % MESH_ACK_RING;
        if (s_ack_ring[idx].packet_id == packet_id) {
            result = (mesh_ack_status_t)s_ack_ring[idx].status;
            break;
        }
    }
    portEXIT_CRITICAL(&s_ack_mux);
    return result;
}

/* Forward declaration — defined later at line ~446. */
static bool mesh_tx_data(uint32_t to, const mesh_data_t &data,
                         bool want_ack = false, uint32_t *out_id = nullptr,
                         uint32_t force_id = 0);

/* Send an ACK (Routing proto) back to a node that requested one. */
static void mesh_send_ack(uint32_t to, uint32_t original_packet_id)
{
    mesh_data_t d = {};
    d.portnum = MESH_PORT_ROUTING;
    d.request_id = original_packet_id;
    /* Routing proto: error_reason = NONE (field 1, varint value 0). */
    d.payload[0] = 0x08;  /* tag: field 1, wire type 0 */
    d.payload[1] = 0x00;  /* value: 0 (NONE) */
    d.payload_len = 2;
    mesh_tx_data(to, d);
}

/* ==================== header packing ==================== */

static void pack_header(uint8_t hdr[16],
                        uint32_t to, uint32_t from, uint32_t id,
                        uint8_t hop_limit, bool want_ack)
{
    /* Little-endian, `to` first (bytes 0..3). */
    hdr[0]  = (uint8_t)(to);
    hdr[1]  = (uint8_t)(to >> 8);
    hdr[2]  = (uint8_t)(to >> 16);
    hdr[3]  = (uint8_t)(to >> 24);
    hdr[4]  = (uint8_t)(from);
    hdr[5]  = (uint8_t)(from >> 8);
    hdr[6]  = (uint8_t)(from >> 16);
    hdr[7]  = (uint8_t)(from >> 24);
    hdr[8]  = (uint8_t)(id);
    hdr[9]  = (uint8_t)(id >> 8);
    hdr[10] = (uint8_t)(id >> 16);
    hdr[11] = (uint8_t)(id >> 24);

    uint8_t hop_start = hop_limit & MESH_FLAGS_HOP_LIMIT_MASK;
    uint8_t flags = (hop_start & MESH_FLAGS_HOP_LIMIT_MASK)
                  | (want_ack ? MESH_FLAGS_WANT_ACK_MASK : 0)
                  | ((hop_start << MESH_FLAGS_HOP_START_SHIFT) & MESH_FLAGS_HOP_START_MASK);
    hdr[12] = flags;
    hdr[13] = s_channel_hash;
    hdr[14] = 0;  /* next_hop = no preference */
    hdr[15] = 0;  /* relay_node = none */
}

static void parse_header(const uint8_t hdr[16],
                         uint32_t *to, uint32_t *from, uint32_t *id,
                         uint8_t *hop_limit, uint8_t *hop_start, uint8_t *channel,
                         bool *want_ack)
{
    *to   = (uint32_t)hdr[0]  | ((uint32_t)hdr[1]  << 8)
          | ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3]  << 24);
    *from = (uint32_t)hdr[4]  | ((uint32_t)hdr[5]  << 8)
          | ((uint32_t)hdr[6] << 16) | ((uint32_t)hdr[7]  << 24);
    *id   = (uint32_t)hdr[8]  | ((uint32_t)hdr[9]  << 8)
          | ((uint32_t)hdr[10]<< 16) | ((uint32_t)hdr[11] << 24);
    uint8_t flags = hdr[12];
    *hop_limit = flags & MESH_FLAGS_HOP_LIMIT_MASK;
    *hop_start = (flags & MESH_FLAGS_HOP_START_MASK) >> MESH_FLAGS_HOP_START_SHIFT;
    *channel = hdr[13];
    *want_ack = (flags & MESH_FLAGS_WANT_ACK_MASK) != 0;
}

/* ==================== TX pipeline ==================== */

static SX1262 *s_radio = nullptr;

/* Ciphertext budget: 255 (LoRa max) - 16 (header) = 239 bytes. */
#define MESH_MAX_CIPHERTEXT 239

static bool mesh_tx_data(uint32_t to, const mesh_data_t &data,
                         bool want_ack, uint32_t *out_id,
                         uint32_t force_id)
{
    uint8_t encoded[MESH_MAX_CIPHERTEXT];
    mesh_buf_t buf = { encoded, sizeof(encoded), 0 };
    if (!mesh_pb_encode_data(&buf, &data)) return false;
    if (buf.len == 0 || buf.len > MESH_MAX_CIPHERTEXT) return false;

    uint32_t id = force_id ? force_id : next_packet_id();

    /* Encrypt the Data proto in place. */
    mesh_crypto_ctr(s_active_psk, id, s_own_id, encoded, buf.len);

    /* Build the full on-air frame: 16 byte header + ciphertext. */
    uint8_t frame[16 + MESH_MAX_CIPHERTEXT];
    pack_header(frame, to, s_own_id, id, MESH_HOP_RELIABLE, want_ack);
    memcpy(frame + 16, encoded, buf.len);

    size_t total = 16 + buf.len;
    if (total > 255) return false;  /* guard against a future proto change */

    int st = s_radio->transmit(frame, total);
    /* Immediately return to RX so we don't miss other nodes' traffic. */
    s_radio->startReceive();
    bool ok = (st == RADIOLIB_ERR_NONE);
    if (ok && out_id) *out_id = id;
    return ok;
}

uint32_t mesh_send_broadcast_text(const char *text)
{
    if (!s_up || !text) return 0;
    mesh_data_t d = {};
    d.portnum = MESH_PORT_TEXT_MESSAGE;
    size_t n = strlen(text);
    if (n > sizeof(d.payload)) n = sizeof(d.payload);
    memcpy(d.payload, text, n);
    d.payload_len = (uint16_t)n;
    uint32_t id = 0;
    if (mesh_tx_data(MESH_BROADCAST_NODEID, d, true, &id)) {
        ack_add(id, MESH_BROADCAST_NODEID, text, (uint16_t)n);
        return id;
    }
    return 0;
}

uint32_t mesh_send_direct_text(uint32_t dest, const char *text)
{
    if (!s_up || !text || dest == 0 || dest == s_own_id) return 0;
    mesh_data_t d = {};
    d.portnum = MESH_PORT_TEXT_MESSAGE;
    d.dest = dest;
    d.source = s_own_id;
    size_t n = strlen(text);
    if (n > sizeof(d.payload)) n = sizeof(d.payload);
    memcpy(d.payload, text, n);
    d.payload_len = (uint16_t)n;
    uint32_t id = 0;
    if (mesh_tx_data(dest, d, true, &id)) {
        ack_add(id, dest, text, (uint16_t)n);
        return id;
    }
    return 0;
}

bool mesh_send_nodeinfo(void)
{
    if (!s_up) return false;
    mesh_user_t u = {};
    snprintf(u.id, sizeof(u.id), "!%08x", (unsigned int)s_own_id);
    strncpy(u.long_name, s_own_long, sizeof(u.long_name) - 1);
    strncpy(u.short_name, s_own_short, sizeof(u.short_name) - 1);
    u.hw_model = 43;  /* PRIVATE_HW from hw_model_v2.proto, reasonable default */
    u.role = 0;       /* CLIENT */

    uint8_t user_buf[MESH_MAX_PAYLOAD];
    mesh_buf_t ub = { user_buf, sizeof(user_buf), 0 };
    if (!mesh_pb_encode_user(&ub, &u)) return false;

    mesh_data_t d = {};
    d.portnum = MESH_PORT_NODEINFO;
    if (ub.len > sizeof(d.payload)) return false;
    memcpy(d.payload, user_buf, ub.len);
    d.payload_len = (uint16_t)ub.len;
    return mesh_tx_data(MESH_BROADCAST_NODEID, d);
}

bool mesh_send_position(void)
{
    if (!s_up) return false;
    const gps_fix_t &fix = gps_get();
    if (!fix.valid) return false;

    mesh_position_t pos = {};
    pos.latitude_i  = (int32_t)(fix.lat_deg * 1e7);
    pos.longitude_i = (int32_t)(fix.lon_deg * 1e7);
    pos.altitude    = (int32_t)fix.alt_m;
    /* Position.time is Unix epoch seconds. We don't have an RTC so
     * leaving time=0 and letting the field go unencoded — other apps
     * will show "time unknown" instead of "1970". */
    pos.time        = 0;
    pos.location_source = 1;  /* LOC_INTERNAL */
    pos.sats_in_view = fix.sats;

    uint8_t pos_buf[MESH_MAX_PAYLOAD];
    mesh_buf_t pb = { pos_buf, sizeof(pos_buf), 0 };
    if (!mesh_pb_encode_position(&pb, &pos)) return false;

    mesh_data_t d = {};
    d.portnum = MESH_PORT_POSITION;
    if (pb.len > sizeof(d.payload)) return false;
    memcpy(d.payload, pos_buf, pb.len);
    d.payload_len = (uint16_t)pb.len;
    return mesh_tx_data(MESH_BROADCAST_NODEID, d);
}

/* ==================== RX pipeline ==================== */

static void handle_decoded_data(uint32_t from, uint32_t to, uint8_t hops,
                                int16_t rssi, int8_t snr,
                                const mesh_data_t &d,
                                uint32_t packet_id, bool want_ack)
{
    /* Always update roster — any packet tells us a node exists. */
    portENTER_CRITICAL(&s_nodes_mux);
    int idx = upsert_node(from);
    s_nodes[idx].last_snr = snr;
    s_nodes[idx].last_rssi = rssi;
    s_nodes[idx].hops = hops;
    s_nodes[idx].last_seen_ms = millis();
    portEXIT_CRITICAL(&s_nodes_mux);

    /* If the sender wants an ACK and the packet is addressed to us
     * (not broadcast — we don't ACK broadcasts to avoid storms), send
     * one back before processing the payload. */
    if (want_ack && to == s_own_id) {
        mesh_send_ack(from, packet_id);
    }

    /* If this is a Routing packet with a request_id, it's likely an
     * ACK for one of our outgoing messages. */
    if (d.portnum == MESH_PORT_ROUTING && d.request_id != 0) {
        ack_match(d.request_id);
        /* Fall through to process any other fields if needed, but
         * for now ACKs are handled entirely above. */
        return;
    }

    switch (d.portnum) {
    case MESH_PORT_TEXT_MESSAGE: {
        mesh_message_t m = {};
        m.from = from;
        m.to   = to;
        m.hops = hops;
        m.snr  = snr;
        m.rssi = rssi;
        m.when_ms = millis();
        uint16_t n = d.payload_len;
        if (n > sizeof(m.text) - 1) n = sizeof(m.text) - 1;
        memcpy(m.text, d.payload, n);
        m.text[n] = '\0';
        m.text_len = n;
        push_message(m);

        /* Command parser — if this text starts with "!poseidon " the
         * message is a remote query. Currently supports "!poseidon ping"
         * which echoes a status reply back. Addressed to us specifically
         * or broadcast; either way we only respond if the suffix matches
         * a known command. Serial-log unknown !poseidon-prefixed msgs
         * so the operator can see activity. */
        if (n >= 10 && memcmp(m.text, "!poseidon ", 10) == 0) {
            const char *cmd = m.text + 10;
            char reply[96];
            reply[0] = '\0';
            if (!strncmp(cmd, "ping", 4)) {
                snprintf(reply, sizeof(reply),
                         "pong from !%08x  rssi=%d snr=%.1f hops=%u",
                         (unsigned)mesh_own_node_id(),
                         (int)rssi, snr, (unsigned)hops);
            } else if (!strncmp(cmd, "status", 6)) {
                snprintf(reply, sizeof(reply),
                         "!%08x  heap=%uK  mesh-ok",
                         (unsigned)mesh_own_node_id(),
                         (unsigned)(ESP.getFreeHeap() / 1024));
            }
            if (reply[0]) {
                Serial.printf("[mesh-cmd] from=!%08x cmd=%s -> %s\n",
                              (unsigned)from, cmd, reply);
                mesh_send_broadcast_text(reply);
            } else {
                Serial.printf("[mesh-cmd] from=!%08x unknown=%s\n",
                              (unsigned)from, cmd);
            }
        }
        break;
    }
    case MESH_PORT_NODEINFO: {
        mesh_user_t u;
        if (mesh_pb_decode_user(d.payload, d.payload_len, &u)) {
            portENTER_CRITICAL(&s_nodes_mux);
            int ni = upsert_node(from);
            strncpy(s_nodes[ni].long_name, u.long_name, sizeof(s_nodes[ni].long_name) - 1);
            strncpy(s_nodes[ni].short_name, u.short_name, sizeof(s_nodes[ni].short_name) - 1);
            portEXIT_CRITICAL(&s_nodes_mux);
        }
        break;
    }
    case MESH_PORT_POSITION: {
        mesh_position_t pos;
        if (mesh_pb_decode_position(d.payload, d.payload_len, &pos)) {
            portENTER_CRITICAL(&s_nodes_mux);
            int ni = upsert_node(from);
            s_nodes[ni].has_position = true;
            s_nodes[ni].latitude_i   = pos.latitude_i;
            s_nodes[ni].longitude_i  = pos.longitude_i;
            s_nodes[ni].altitude     = pos.altitude;
            portEXIT_CRITICAL(&s_nodes_mux);
        }
        break;
    }
    case MESH_PORT_TRACEROUTE_APP: {
        /* Traceroute response — decode RouteDiscovery and store result.
         * We only accept responses addressed to us (direct). */
        if (to == s_own_id) {
            mesh_route_discovery_t rd;
            if (mesh_pb_decode_traceroute(d.payload, d.payload_len, &rd)) {
                /* Determine which path to use. If the response has
                 * route_back entries, those represent the forward path
                 * (dest copies them reversed from the request). If not,
                 * use route + snr_towards. */
                mesh_traceroute_result_t res = {};
                int n = 0;
                if (rd.route_back_count > 0) {
                    n = rd.route_back_count;
                    if (n > MESH_TRACEROUTE_MAX_HOPS) n = MESH_TRACEROUTE_MAX_HOPS;
                    for (int i = 0; i < n; i++) {
                        res.route[i] = rd.route_back[i];
                        /* SNR values in proto are dB*4; convert to integer dB */
                        res.snr[i] = (int8_t)(rd.snr_back[i] / 4);
                    }
                } else if (rd.route_count > 0) {
                    n = rd.route_count;
                    if (n > MESH_TRACEROUTE_MAX_HOPS) n = MESH_TRACEROUTE_MAX_HOPS;
                    for (int i = 0; i < n; i++) {
                        res.route[i] = rd.route[i];
                        res.snr[i] = (int8_t)(rd.snr_towards[i] / 4);
                    }
                }
                /* Even if n==0, we got a response — direct link (0 hops). */
                res.hops = n;
                res.complete = true;
                s_traceroute_result = res;
                s_traceroute_ready = true;
                Serial.printf("[mesh-trace] response from !%08x hops=%d\n",
                              (unsigned)from, n);
            }
        }
        break;
    }
    default:
        break;
    }
}

static void rx_task(void *)
{
    s_rx_task_alive = true;
    uint8_t buf[260];
    while (!s_rx_task_stop) {
        /* Poll for a packet. RadioLib's receive() blocks with a timeout;
         * getPacketLength after startReceive returns non-zero only when
         * a packet has been fully received. Use non-blocking polling. */
        size_t plen = s_radio->getPacketLength();
        if (plen == 0 || plen > sizeof(buf)) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        int st = s_radio->readData(buf, plen);
        int16_t rssi = (int16_t)s_radio->getRSSI();
        int8_t  snr  = (int8_t)s_radio->getSNR();
        s_radio->startReceive();  /* re-arm */
        if (st != RADIOLIB_ERR_NONE || plen < 16) continue;

        uint32_t to, from, id;
        uint8_t hop_limit, hop_start, channel;
        bool want_ack;
        parse_header(buf, &to, &from, &id, &hop_limit, &hop_start, &channel, &want_ack);

        /* Filter: only matching-channel traffic; ignore packets from
         * ourselves (shouldn't happen but be safe). */
        if (channel != s_channel_hash) continue;
        if (from == s_own_id) continue;

        size_t ctext_len = plen - 16;
        uint8_t ctext[260];
        memcpy(ctext, buf + 16, ctext_len);
        mesh_crypto_ctr(s_active_psk, id, from, ctext, ctext_len);

        mesh_data_t d;
        if (!mesh_pb_decode_data(ctext, ctext_len, &d)) continue;

        uint8_t hops = (hop_start > hop_limit) ? (hop_start - hop_limit) : 0;
        handle_decoded_data(from, to, hops, rssi, snr, d, id, want_ack);
    }
    s_rx_task_alive = false;
    vTaskDelete(nullptr);
}

/* ==================== lifecycle ==================== */

bool mesh_is_up(void) { return s_up; }

bool mesh_begin(void)
{
    if (s_up) return true;

    /* Load channel config (name + PSK) from NVS before anything else. */
    load_channel_config();

    derive_identity();
    s_packet_counter = esp_random() & 0x3FFu;
    s_node_count = 0;
    s_msg_head = 0;
    s_msg_count = 0;
    s_new_msg = false;

    Serial.printf("[mesh] begin: free heap = %u bytes\n",
                  (unsigned)ESP.getFreeHeap());

    /* Allocate node + message buffers only when mesh is active. Previously
     * these were static BSS (9.5 KB) eating DRAM 24/7 — contributed to
     * WiFi.scanNetworks hitting ENOMEM under cumulative heap pressure. */
    if (!s_nodes) s_nodes = (mesh_node_t *)calloc(MESH_MAX_NODES, sizeof(mesh_node_t));
    if (!s_msgs)  s_msgs  = (mesh_message_t *)calloc(MESH_MSG_RING, sizeof(mesh_message_t));
    if (!s_nodes || !s_msgs) {
        Serial.printf("[mesh] FAIL: node/msg alloc (nodes=%p msgs=%p) heap=%u\n",
                      s_nodes, s_msgs, (unsigned)ESP.getFreeHeap());
        free(s_nodes); s_nodes = nullptr;
        free(s_msgs);  s_msgs  = nullptr;
        return false;
    }

    /* Configure LoRa for Meshtastic channel. lora_hw's config struct
     * takes freq_mhz, bw_khz, sf, cr (as int 5..8), sync byte, power.
     * Frequency is derived from the channel name via djb2 mod 104. */
    lora_config_t cfg = {
        .freq_mhz = s_channel_freq,
        .bw_khz   = MESH_BW_KHZ,
        .sf       = MESH_SF,
        .cr       = MESH_CR,
        .sync     = MESH_SYNC_WORD,
        .power    = MESH_TX_POWER_DBM,
    };
    int st = lora_begin(cfg);
    if (st != RADIOLIB_ERR_NONE) {
        Serial.printf("[mesh] FAIL: lora_begin returned %d (Radiolib err) heap=%u\n",
                      st, (unsigned)ESP.getFreeHeap());
        return false;
    }

    s_radio = &lora_radio();
    s_radio->setPreambleLength(MESH_PREAMBLE);
    s_radio->setCRC(2);           /* CRC enabled */
    s_radio->explicitHeader();
    s_radio->startReceive();

    s_rx_task_stop = false;
    /* 5KB stack — enough for RadioLib SPI buffers + our protobuf decoder
     * without biting into scarce heap needed for WiFi init. */
    BaseType_t ok = xTaskCreatePinnedToCore(rx_task, "mesh_rx", 5120,
                                            nullptr, 3, &s_rx_task, 1);
    if (ok != pdPASS) {
        Serial.printf("[mesh] FAIL: xTaskCreate heap=%u\n",
                      (unsigned)ESP.getFreeHeap());
        lora_end();
        return false;
    }

    s_up = true;
    const char *ch = s_channel_name[0] ? s_channel_name : "LongFast";
    Serial.printf("[mesh] up id=!%08x long=%s channel='%s' hash=0x%02X freq=%.3f\n",
                  (unsigned int)s_own_id, s_own_long, ch, s_channel_hash, s_channel_freq);

    /* Announce ourselves on startup. */
    mesh_send_nodeinfo();
    s_last_nodeinfo_ms = millis();
    return true;
}

void mesh_end(void)
{
    if (!s_up) return;
    s_rx_task_stop = true;
    /* Wait for task to exit cooperatively. If it's stuck in a RadioLib
     * SPI transaction it won't notice the flag — force-delete after a
     * bounded wait rather than risk tearing down the radio underneath it. */
    for (int i = 0; i < 20 && s_rx_task_alive; i++) delay(25);
    if (s_rx_task_alive && s_rx_task) {
        vTaskDelete(s_rx_task);
        s_rx_task_alive = false;
    }
    s_rx_task = nullptr;
    lora_end();
    s_radio = nullptr;
    /* Free heap-allocated buffers so WiFi has room. */
    free(s_nodes); s_nodes = nullptr;
    free(s_msgs);  s_msgs  = nullptr;
    s_node_count = 0;
    s_msg_count = 0;
    s_msg_head = 0;
    s_ack_head = 0;
    s_ack_count = 0;
    s_up = false;
}

/* ==================== position reporting ==================== */

void mesh_set_position_reporting(bool on) { s_position_reporting = on; }
bool mesh_position_reporting(void)         { return s_position_reporting; }

/*
 * Called from a feature's main loop (chat / nodes / page) every tick so
 * we trickle NodeInfo + Position without needing a dedicated timer task.
 * Exposed via header for those features to call periodically.
 */
extern "C" void mesh_tick(void)
{
    if (!s_up) return;
    uint32_t now = millis();
    /* NodeInfo every 30 min. */
    if (now - s_last_nodeinfo_ms > (uint32_t)(30UL * 60UL * 1000UL)) {
        mesh_send_nodeinfo();
        s_last_nodeinfo_ms = now;
    }
    /* Position every 15 min when reporting is enabled and GPS has a fix. */
    if (s_position_reporting && now - s_last_position_ms > (uint32_t)(15UL * 60UL * 1000UL)) {
        if (mesh_send_position()) {
            s_last_position_ms = now;
        }
    }

    /* ---- ACK retry logic ----
     * Check pending ACKs every 2 seconds.  If an entry is older than 3 s
     * and retries < 3, retransmit the same packet_id.  After 3 retries
     * mark as failed. */
    if (s_ack_count == 0) return;
    static uint32_t s_last_ack_check = 0;
    if (now - s_last_ack_check < 2000) return;
    s_last_ack_check = now;

    for (int i = 0; i < s_ack_count; i++) {
        /* Grab one entry under lock, release immediately so we can TX. */
        portENTER_CRITICAL(&s_ack_mux);
        int idx = (s_ack_head + i) % MESH_ACK_RING;
        mesh_ack_entry_t *e = &s_ack_ring[idx];

        if (e->status != MESH_ACK_PENDING) {
            portEXIT_CRITICAL(&s_ack_mux);
            continue;
        }
        if ((int32_t)(now - e->tx_time_ms) < 3000) {
            portEXIT_CRITICAL(&s_ack_mux);
            continue;
        }
        if (e->retries >= 3) {
            e->status = MESH_ACK_FAILED;
            Serial.printf("[mesh-ack] FAILED id=0x%08x retries=%u\n",
                          (unsigned)e->packet_id, e->retries);
            portEXIT_CRITICAL(&s_ack_mux);
            continue;
        }

        /* Copy fields needed for retransmission. */
        uint32_t dest = e->dest_node;
        uint32_t pid  = e->packet_id;
        uint8_t  tlen = e->text_len;
        char     text_copy[MESH_MAX_PAYLOAD];
        memcpy(text_copy, e->text, tlen);
        e->retries++;
        e->tx_time_ms = now;
        portEXIT_CRITICAL(&s_ack_mux);

        /* Rebuild the Data proto and retransmit with the same packet_id. */
        mesh_data_t d = {};
        d.portnum = MESH_PORT_TEXT_MESSAGE;
        memcpy(d.payload, text_copy, tlen);
        d.payload_len = tlen;
        bool is_bc = (dest == 0 || dest == MESH_BROADCAST_NODEID);
        if (!is_bc) {
            d.dest   = dest;
            d.source = s_own_id;
        }
        mesh_tx_data(is_bc ? MESH_BROADCAST_NODEID : dest,
                     d, true, nullptr, pid);
        Serial.printf("[mesh-ack] retry %u/3 id=0x%08x\n",
                      (unsigned)e->retries, (unsigned)pid);
    }
}

/* ==================== traceroute ==================== */

bool mesh_send_traceroute(uint32_t dest_node_id)
{
    if (!s_up || dest_node_id == 0 || dest_node_id == s_own_id) return false;

    /* Encode an empty RouteDiscovery as the payload. Intermediate
     * Meshtastic nodes will populate route[] and snr_towards[] as the
     * packet traverses the mesh. The destination sends back a response
     * with the full path. */
    mesh_route_discovery_t rd = {};
    uint8_t rd_buf[80];
    mesh_buf_t rb = { rd_buf, sizeof(rd_buf), 0 };
    if (!mesh_pb_encode_traceroute(&rb, &rd)) return false;

    mesh_data_t d = {};
    d.portnum = MESH_PORT_TRACEROUTE_APP;
    d.dest = dest_node_id;
    d.source = s_own_id;
    if (rb.len > sizeof(d.payload)) return false;
    memcpy(d.payload, rd_buf, rb.len);
    d.payload_len = (uint16_t)rb.len;

    s_traceroute_ready = false;
    memset(&s_traceroute_result, 0, sizeof(s_traceroute_result));

    bool ok = mesh_tx_data(dest_node_id, d);
    if (ok) {
        Serial.printf("[mesh-trace] sent to !%08x\n", (unsigned)dest_node_id);
    }
    return ok;
}

bool mesh_traceroute_result(mesh_traceroute_result_t *out)
{
    if (!s_traceroute_ready) return false;
    if (out) *out = s_traceroute_result;
    s_traceroute_ready = false;
    return true;
}

void mesh_traceroute_clear(void)
{
    s_traceroute_ready = false;
    memset(&s_traceroute_result, 0, sizeof(s_traceroute_result));
}
