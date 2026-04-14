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
    /* We only care about role=1 (C5 nodes). */
    if (h->role != 1) return;

    int idx = find_peer(mac);
    if (idx < 0) {
        if (s_peer_n >= MAX_PEERS) return;
        idx = s_peer_n++;
        memcpy(s_peers[idx].mac, mac, 6);
        ensure_peer(mac);
    }
    strncpy(s_peers[idx].name, h->name, sizeof(s_peers[idx].name) - 1);
    s_peers[idx].name[sizeof(s_peers[idx].name) - 1] = '\0';
    s_peers[idx].has_5g          = h->has_5g;
    s_peers[idx].has_ieee802154  = h->has_ieee802154;
    s_peers[idx].last_seen       = millis();
}

static void handle_resp_ap(const c5_msg_t *m)
{
    int count = m->payload_len / sizeof(c5_ap_t);
    const c5_ap_t *src = (const c5_ap_t *)m->payload;
    for (int i = 0; i < count && s_ap_n < MAX_APS; ++i) {
        /* Dedup by BSSID. */
        bool dup = false;
        for (int j = 0; j < s_ap_n; ++j)
            if (memcmp(s_aps[j].bssid, src[i].bssid, 6) == 0) { dup = true; break; }
        if (dup) continue;
        s_aps[s_ap_n++] = src[i];
    }
}

static void handle_resp_zb(const c5_msg_t *m)
{
    if (m->payload_len < (int)sizeof(c5_zb_t)) return;
    if (s_zb_n >= MAX_ZBS) {
        /* Rotate ring. */
        memmove(s_zbs, s_zbs + 1, sizeof(c5_zb_t) * (MAX_ZBS - 1));
        s_zb_n = MAX_ZBS - 1;
    }
    memcpy(&s_zbs[s_zb_n++], m->payload, sizeof(c5_zb_t));
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

/* Evict peers silent for >15 s. */
static void evict(void)
{
    uint32_t now = millis();
    for (int i = s_peer_n - 1; i >= 0; --i)
        if (now - s_peers[i].last_seen > 15000)
            s_peers[i] = s_peers[--s_peer_n];
}

bool c5_any_online(void) { evict(); return s_peer_n > 0; }
int  c5_peer_count(void) { evict(); return s_peer_n; }
uint32_t c5_last_seen_ms(void)
{
    evict();
    uint32_t best = 0;
    for (int i = 0; i < s_peer_n; ++i)
        if (s_peers[i].last_seen > best) best = s_peers[i].last_seen;
    return best ? (millis() - best) : UINT32_MAX;
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
    /* Send to every known peer directly. */
    for (int i = 0; i < s_peer_n; ++i) {
        esp_now_send(s_peers[i].mac, (const uint8_t *)&m, sizeof(m));
    }
    /* Also broadcast so new C5s catch it. */
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

int c5_aps(c5_ap_t *out, int max)
{
    int n = s_ap_n < max ? s_ap_n : max;
    memcpy(out, s_aps, n * sizeof(c5_ap_t));
    return n;
}
int c5_zbs(c5_zb_t *out, int max)
{
    int n = s_zb_n < max ? s_zb_n : max;
    memcpy(out, s_zbs, n * sizeof(c5_zb_t));
    return n;
}
void c5_clear_results(void) { s_ap_n = 0; s_zb_n = 0; }
