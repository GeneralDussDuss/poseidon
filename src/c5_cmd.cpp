/*
 * c5_cmd.cpp — dispatcher for the POSEI v2 ESP-NOW protocol.
 *
 * Owns the ESP-NOW recv callback. Dispatches:
 *   - HELLO → peer table (our own; mesh.cpp keeps its old table too)
 *   - RESP_AP → append to s_aps ring
 *   - RESP_ZB → append to s_zbs ring
 *   - RESP_PONG → update last_seen
 *
 * Auto-adds any new sender to esp_now peer list so we can reply.
 */
#include "c5_cmd.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <esp_system.h>
#include <string.h>

#define MAX_PEERS 4
#define MAX_APS   64
#define MAX_ZBS   32

/* Spin-lock shared between the ESP-NOW recv callback (runs in the WiFi
 * task) and the UI thread reading peer/result arrays. Without this,
 * printf("%s", c5_peer_name(i)) could race with a mid-write and read
 * past the end of a not-yet-null-terminated name buffer. */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

struct c5_peer_t {
    uint8_t  mac[6];
    char     name[12];
    uint32_t last_seen;
    uint8_t  has_5g;
    uint8_t  has_ieee802154;
};

static c5_peer_t s_peers[MAX_PEERS];
static volatile int s_peer_n = 0;

static c5_ap_t  s_aps[MAX_APS];
static volatile int s_ap_n = 0;

static c5_zb_t  s_zbs[MAX_ZBS];
static volatile int s_zb_n = 0;

static volatile uint32_t s_last_status_frames  = 0;
static volatile uint8_t  s_last_status_channel = 0;

static volatile uint16_t s_next_seq = 1;
static volatile bool     s_started = false;
static const uint8_t BROADCAST_MAC[6] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };

static int find_peer(const uint8_t *mac)
{
    for (int i = 0; i < s_peer_n; ++i)
        if (memcmp(s_peers[i].mac, mac, 6) == 0) return i;
    return -1;
}

static void ensure_peer(const uint8_t *mac)
{
    if (!esp_now_is_peer_exist(mac)) {
        esp_now_peer_info_t pi = {};
        memcpy(pi.peer_addr, mac, 6);
        pi.channel = 0;
        pi.encrypt = false;
        esp_now_add_peer(&pi);
    }
}

static void handle_hello(const uint8_t *mac, const c5_msg_t *m)
{
    if (m->payload_len < (int)sizeof(c5_hello_t)) return;
    const c5_hello_t *h = (const c5_hello_t *)m->payload;
    if (h->role != 1) return;

    portENTER_CRITICAL(&s_mux);
    int idx = find_peer(mac);
    if (idx < 0) {
        if (s_peer_n < MAX_PEERS) {
            idx = s_peer_n;
            memset(&s_peers[idx], 0, sizeof(s_peers[idx]));
            memcpy(s_peers[idx].mac, mac, 6);
            /* Publish the new slot AFTER it's fully initialized so the
             * UI thread never sees a half-written entry. */
            s_peer_n = idx + 1;
        } else {
            idx = -1;
        }
    }
    if (idx >= 0) {
        strncpy(s_peers[idx].name, h->name, sizeof(s_peers[idx].name) - 1);
        s_peers[idx].name[sizeof(s_peers[idx].name) - 1] = '\0';
        s_peers[idx].has_5g          = h->has_5g;
        s_peers[idx].has_ieee802154  = h->has_ieee802154;
        s_peers[idx].last_seen       = millis();
    }
    portEXIT_CRITICAL(&s_mux);

    if (idx >= 0 && !esp_now_is_peer_exist(mac)) ensure_peer(mac);
}

static void handle_resp_ap(const c5_msg_t *m)
{
    int count = m->payload_len / sizeof(c5_ap_t);
    const c5_ap_t *src = (const c5_ap_t *)m->payload;
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < count && s_ap_n < MAX_APS; ++i) {
        bool dup = false;
        for (int j = 0; j < s_ap_n; ++j)
            if (memcmp(s_aps[j].bssid, src[i].bssid, 6) == 0) { dup = true; break; }
        if (dup) continue;
        s_aps[s_ap_n++] = src[i];
    }
    portEXIT_CRITICAL(&s_mux);
}

static void handle_resp_zb(const c5_msg_t *m)
{
    if (m->payload_len < (int)sizeof(c5_zb_t)) return;
    portENTER_CRITICAL(&s_mux);
    if (s_zb_n >= MAX_ZBS) {
        memmove(s_zbs, s_zbs + 1, sizeof(c5_zb_t) * (MAX_ZBS - 1));
        s_zb_n = MAX_ZBS - 1;
    }
    memcpy(&s_zbs[s_zb_n++], m->payload, sizeof(c5_zb_t));
    portEXIT_CRITICAL(&s_mux);
}

static void on_recv(const uint8_t *mac, const uint8_t *data, int len)
{
    if (len < (int)sizeof(c5_msg_t)) return;
    const c5_msg_t *m = (const c5_msg_t *)data;
    if (m->magic != C5_MAGIC || m->version != C5_VERSION) return;

    ensure_peer(mac);
    switch (m->type) {
    case C5_TYPE_HELLO:      handle_hello(mac, m); break;
    case C5_TYPE_RESP_AP:    handle_resp_ap(m);    break;
    case C5_TYPE_RESP_ZB:    handle_resp_zb(m);    break;
    case C5_TYPE_RESP_PONG: {
        int idx = find_peer(mac);
        if (idx >= 0) s_peers[idx].last_seen = millis();
        break;
    }
    case C5_TYPE_RESP_STATUS: {
        if (m->payload_len >= 5) {
            uint32_t f;
            memcpy(&f, m->payload, 4);
            s_last_status_frames  = f;
            s_last_status_channel = m->payload[4];
        }
        break;
    }
    }
}

bool c5_begin(void)
{
    if (s_started) return true;
    /* ESP-NOW requires WiFi up in STA mode. */
    if (WiFi.getMode() == WIFI_OFF) WiFi.mode(WIFI_STA);
    if (esp_now_init() != ESP_OK) return false;
    esp_now_register_recv_cb(on_recv);

    /* Broadcast peer for sending HELLOs if we want. */
    if (!esp_now_is_peer_exist(BROADCAST_MAC)) {
        esp_now_peer_info_t pi = {};
        memcpy(pi.peer_addr, BROADCAST_MAC, 6);
        esp_now_add_peer(&pi);
    }
    s_peer_n = 0;
    s_ap_n = 0;
    s_zb_n = 0;
    s_started = true;
    return true;
}

void c5_stop(void)
{
    if (!s_started) return;
    esp_now_deinit();
    s_started = false;
}

/* Evict peers silent for >15 s. MUST be called under s_mux. */
static void evict_locked(void)
{
    uint32_t now = millis();
    for (int i = s_peer_n - 1; i >= 0; --i) {
        if (now - s_peers[i].last_seen > 15000) {
            s_peers[i] = s_peers[s_peer_n - 1];
            s_peer_n--;
        }
    }
}

bool c5_any_online(void)
{
    portENTER_CRITICAL(&s_mux);
    evict_locked();
    bool r = s_peer_n > 0;
    portEXIT_CRITICAL(&s_mux);
    return r;
}

int c5_peer_count(void)
{
    portENTER_CRITICAL(&s_mux);
    evict_locked();
    int n = s_peer_n;
    portEXIT_CRITICAL(&s_mux);
    return n;
}

uint32_t c5_last_seen_ms(void)
{
    portENTER_CRITICAL(&s_mux);
    evict_locked();
    uint32_t best = 0;
    for (int i = 0; i < s_peer_n; ++i)
        if (s_peers[i].last_seen > best) best = s_peers[i].last_seen;
    portEXIT_CRITICAL(&s_mux);
    return best ? (millis() - best) : UINT32_MAX;
}

/* Copy peer name into caller buffer — always null-terminated. Safer
 * than returning a pointer into the live array because the ISR can
 * mutate entries at any time. */
void c5_peer_name_copy(int idx, char *out, int max)
{
    if (max <= 0) return;
    out[0] = '\0';
    portENTER_CRITICAL(&s_mux);
    if (idx >= 0 && idx < s_peer_n) {
        int lim = max - 1;
        if (lim > (int)sizeof(s_peers[idx].name) - 1) lim = sizeof(s_peers[idx].name) - 1;
        memcpy(out, s_peers[idx].name, lim);
        out[lim] = '\0';
    }
    portEXIT_CRITICAL(&s_mux);
}

const char *c5_peer_name(int idx)
{
    if (idx < 0 || idx >= s_peer_n) return "";
    return s_peers[idx].name;
}

static uint16_t send_simple_cmd(uint8_t type, const uint8_t *extra, int extra_len)
{
    uint16_t seq = s_next_seq++;
    c5_msg_t m = {};
    m.magic   = C5_MAGIC;
    m.version = C5_VERSION;
    m.type    = type;
    m.seq     = seq;
    if (extra && extra_len > 0 && extra_len < (int)sizeof(m.payload)) {
        memcpy(m.payload, extra, extra_len);
        m.payload_len = extra_len;
    }
    /* Snapshot peer MACs under lock so we don't iterate concurrently
     * with handle_hello adding/removing peers. */
    uint8_t macs[MAX_PEERS][6];
    int count;
    portENTER_CRITICAL(&s_mux);
    count = s_peer_n;
    for (int i = 0; i < count; ++i) memcpy(macs[i], s_peers[i].mac, 6);
    portEXIT_CRITICAL(&s_mux);

    for (int i = 0; i < count; ++i) {
        esp_now_send(macs[i], (const uint8_t *)&m, sizeof(m));
    }
    esp_now_send(BROADCAST_MAC, (const uint8_t *)&m, sizeof(m));
    return seq;
}

uint16_t c5_cmd_ping(void)   { return send_simple_cmd(C5_TYPE_CMD_PING, nullptr, 0); }
uint16_t c5_cmd_stop(void)   { return send_simple_cmd(C5_TYPE_CMD_STOP, nullptr, 0); }
uint16_t c5_cmd_scan_5g(uint16_t duration_ms) {
    uint16_t d = duration_ms;
    return send_simple_cmd(C5_TYPE_CMD_SCAN_5G, (uint8_t *)&d, sizeof(d));
}
uint16_t c5_cmd_scan_zb(uint8_t channel) {
    return send_simple_cmd(C5_TYPE_CMD_SCAN_ZB, &channel, 1);
}
uint16_t c5_cmd_deauth(const uint8_t bssid[6], uint8_t channel,
                       uint8_t bcast_all, uint16_t duration_ms)
{
    c5_deauth_req_t r;
    memcpy(r.bssid, bssid, 6);
    r.channel     = channel;
    r.bcast_all   = bcast_all;
    r.duration_ms = duration_ms;
    s_last_status_frames  = 0;
    s_last_status_channel = channel;
    return send_simple_cmd(C5_TYPE_CMD_DEAUTH, (uint8_t *)&r, sizeof(r));
}

uint32_t c5_status_frames(void)  { return s_last_status_frames; }
uint8_t  c5_status_channel(void) { return s_last_status_channel; }

int c5_aps(c5_ap_t *out, int max)
{
    portENTER_CRITICAL(&s_mux);
    int n = s_ap_n < max ? s_ap_n : max;
    if (out && n > 0) memcpy(out, s_aps, n * sizeof(c5_ap_t));
    int total = s_ap_n;
    portEXIT_CRITICAL(&s_mux);
    return out ? n : total;
}
int c5_zbs(c5_zb_t *out, int max)
{
    portENTER_CRITICAL(&s_mux);
    int n = s_zb_n < max ? s_zb_n : max;
    if (out && n > 0) memcpy(out, s_zbs, n * sizeof(c5_zb_t));
    int total = s_zb_n;
    portEXIT_CRITICAL(&s_mux);
    return out ? n : total;
}
void c5_clear_results(void)
{
    portENTER_CRITICAL(&s_mux);
    s_ap_n = 0; s_zb_n = 0;
    portEXIT_CRITICAL(&s_mux);
}
