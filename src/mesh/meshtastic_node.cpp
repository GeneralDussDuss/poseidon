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
#include <string.h>

#define MESH_MAX_NODES     32
#define MESH_MSG_RING      24

static const uint8_t DEFAULT_PSK[16] = {
    0xD4, 0xF1, 0xBB, 0x3A, 0x20, 0x29, 0x07, 0x59,
    0xF0, 0xBC, 0xFF, 0xAB, 0xCF, 0x4E, 0x69, 0x01
};

/* ==================== state ==================== */

static bool              s_up = false;
static uint32_t          s_own_id = 0;
static char              s_own_long[40] = {0};
static char              s_own_short[8] = {0};
static uint32_t          s_packet_counter = 0;

static mesh_node_t       s_nodes[MESH_MAX_NODES];
static int               s_node_count = 0;
static portMUX_TYPE      s_nodes_mux = portMUX_INITIALIZER_UNLOCKED;

static mesh_message_t    s_msgs[MESH_MSG_RING];
static int               s_msg_head = 0;
static int               s_msg_count = 0;
static volatile bool     s_new_msg = false;
static portMUX_TYPE      s_msgs_mux = portMUX_INITIALIZER_UNLOCKED;

static TaskHandle_t      s_rx_task = nullptr;
static volatile bool     s_rx_task_alive = false;
static volatile bool     s_rx_task_stop = false;

static bool              s_position_reporting = false;
static uint32_t          s_last_nodeinfo_ms = 0;
static uint32_t          s_last_position_ms = 0;

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
    if (count_out) *count_out = s_node_count;
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
    if (count_out) *count_out = s_msg_count;
    return s_msgs;
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
    hdr[13] = MESH_CHANNEL_HASH;
    hdr[14] = 0;  /* next_hop = no preference */
    hdr[15] = 0;  /* relay_node = none */
}

static void parse_header(const uint8_t hdr[16],
                         uint32_t *to, uint32_t *from, uint32_t *id,
                         uint8_t *hop_limit, uint8_t *hop_start, uint8_t *channel)
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
}

/* ==================== TX pipeline ==================== */

static SX1262 *s_radio = nullptr;

/* Ciphertext budget: 255 (LoRa max) - 16 (header) = 239 bytes. */
#define MESH_MAX_CIPHERTEXT 239

static bool mesh_tx_data(uint32_t to, const mesh_data_t &data)
{
    uint8_t encoded[MESH_MAX_CIPHERTEXT];
    mesh_buf_t buf = { encoded, sizeof(encoded), 0 };
    if (!mesh_pb_encode_data(&buf, &data)) return false;
    if (buf.len == 0 || buf.len > MESH_MAX_CIPHERTEXT) return false;

    uint32_t id = next_packet_id();

    /* Encrypt the Data proto in place. */
    mesh_crypto_ctr(DEFAULT_PSK, id, s_own_id, encoded, buf.len);

    /* Build the full on-air frame: 16 byte header + ciphertext. */
    uint8_t frame[16 + MESH_MAX_CIPHERTEXT];
    pack_header(frame, to, s_own_id, id, MESH_HOP_RELIABLE, false);
    memcpy(frame + 16, encoded, buf.len);

    size_t total = 16 + buf.len;
    if (total > 255) return false;  /* guard against a future proto change */

    int st = s_radio->transmit(frame, total);
    /* Immediately return to RX so we don't miss other nodes' traffic. */
    s_radio->startReceive();
    return st == RADIOLIB_ERR_NONE;
}

bool mesh_send_broadcast_text(const char *text)
{
    if (!s_up || !text) return false;
    mesh_data_t d = {};
    d.portnum = MESH_PORT_TEXT_MESSAGE;
    size_t n = strlen(text);
    if (n > sizeof(d.payload)) n = sizeof(d.payload);
    memcpy(d.payload, text, n);
    d.payload_len = (uint16_t)n;
    return mesh_tx_data(MESH_BROADCAST_NODEID, d);
}

bool mesh_send_direct_text(uint32_t dest, const char *text)
{
    if (!s_up || !text || dest == 0 || dest == s_own_id) return false;
    mesh_data_t d = {};
    d.portnum = MESH_PORT_TEXT_MESSAGE;
    d.dest = dest;
    d.source = s_own_id;
    size_t n = strlen(text);
    if (n > sizeof(d.payload)) n = sizeof(d.payload);
    memcpy(d.payload, text, n);
    d.payload_len = (uint16_t)n;
    return mesh_tx_data(dest, d);
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
                                const mesh_data_t &d)
{
    /* Always update roster — any packet tells us a node exists. */
    portENTER_CRITICAL(&s_nodes_mux);
    int idx = upsert_node(from);
    s_nodes[idx].last_snr = snr;
    s_nodes[idx].last_rssi = rssi;
    s_nodes[idx].hops = hops;
    s_nodes[idx].last_seen_ms = millis();
    portEXIT_CRITICAL(&s_nodes_mux);

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
    default:
        /* Routing, telemetry, etc. — ignore for now. */
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
        parse_header(buf, &to, &from, &id, &hop_limit, &hop_start, &channel);

        /* Filter: only default-channel traffic; ignore packets from ourselves
         * (shouldn't happen but be safe). */
        if (channel != MESH_CHANNEL_HASH) continue;
        if (from == s_own_id) continue;

        size_t ctext_len = plen - 16;
        uint8_t ctext[260];
        memcpy(ctext, buf + 16, ctext_len);
        mesh_crypto_ctr(DEFAULT_PSK, id, from, ctext, ctext_len);

        mesh_data_t d;
        if (!mesh_pb_decode_data(ctext, ctext_len, &d)) continue;

        uint8_t hops = (hop_start > hop_limit) ? (hop_start - hop_limit) : 0;
        handle_decoded_data(from, to, hops, rssi, snr, d);
    }
    s_rx_task_alive = false;
    vTaskDelete(nullptr);
}

/* ==================== lifecycle ==================== */

bool mesh_is_up(void) { return s_up; }

bool mesh_begin(void)
{
    if (s_up) return true;

    derive_identity();
    s_packet_counter = esp_random() & 0x3FFu;
    s_node_count = 0;
    s_msg_head = 0;
    s_msg_count = 0;
    s_new_msg = false;

    /* Configure LoRa for Meshtastic LongFast US. lora_hw's config struct
     * takes freq_mhz, bw_khz, sf, cr (as int 5..8), sync byte, power. */
    lora_config_t cfg = {
        .freq_mhz = MESH_FREQ_MHZ,
        .bw_khz   = MESH_BW_KHZ,
        .sf       = MESH_SF,
        .cr       = MESH_CR,
        .sync     = MESH_SYNC_WORD,
        .power    = MESH_TX_POWER_DBM,
    };
    int st = lora_begin(cfg);
    if (st != RADIOLIB_ERR_NONE) return false;

    s_radio = &lora_radio();
    s_radio->setPreambleLength(MESH_PREAMBLE);
    s_radio->setCRC(2);           /* CRC enabled */
    s_radio->explicitHeader();
    s_radio->startReceive();

    s_rx_task_stop = false;
    xTaskCreatePinnedToCore(rx_task, "mesh_rx", 4096, nullptr, 3, &s_rx_task, 1);

    s_up = true;
    Serial.printf("[mesh] up id=!%08x long=%s\n",
                  (unsigned int)s_own_id, s_own_long);

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
}
