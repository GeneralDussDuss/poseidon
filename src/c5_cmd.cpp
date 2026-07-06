/*
 * c5_cmd.cpp — dispatcher for the POSEI v3 ESP-NOW protocol.
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
#define MAX_PMKIDS 16

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

static c5_pmkid_t s_pmkids[MAX_PMKIDS];
static volatile int s_pmkid_n = 0;

/* Handshake buffer — each entry is a full M1+M2 tuple ready to be
 * converted into a hashcat 22000 line. Sized small on purpose; the
 * usual flow is: capture → hashcat-convert to SD → clear. */
#define MAX_HSS 8
static c5_hs_t s_hss[MAX_HSS];
static volatile int s_hs_n = 0;

/* v3 result buffers. Kept small/bounded like the others; the UI drains
 * these via the c5_stas/c5_probes/c5_deauth_hits accessors. SPECTRUM is
 * a single latest-batch snapshot, not a ring. */
#define MAX_STAS    32
#define MAX_PROBES  32
#define MAX_DHITS   32
static c5_sta_t s_stas[MAX_STAS];
static volatile int s_sta_n = 0;
static c5_probe_t s_probes[MAX_PROBES];
static volatile int s_probe_n = 0;
static c5_deauth_hit_t s_dhits[MAX_DHITS];
static volatile int s_dhit_n = 0;
static c5_spectrum_t s_spectrum = {};
static volatile bool s_spectrum_valid = false;

static volatile uint32_t s_last_status_frames  = 0;
static volatile uint8_t  s_last_status_channel = 0;

/* Debug: count every RESP_AP frame and total raw AP records received,
 * regardless of dedup outcome. Lets us tell apart "C5 only sent 2"
 * from "C5 sent 20 but dedup collapsed to 2" or "S3 dropping frames". */
static volatile uint32_t s_dbg_resp_ap_frames = 0;
static volatile uint32_t s_dbg_raw_ap_records = 0;

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
    s_dbg_resp_ap_frames++;
    s_dbg_raw_ap_records += count;
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

static void handle_resp_pmkid(const c5_msg_t *m)
{
    if (m->payload_len < (int)sizeof(c5_pmkid_t)) return;
    const c5_pmkid_t *p = (const c5_pmkid_t *)m->payload;
    portENTER_CRITICAL(&s_mux);
    /* Dedup by (bssid, sta, pmkid). */
    for (int i = 0; i < s_pmkid_n; ++i) {
        if (memcmp(s_pmkids[i].bssid, p->bssid, 6) == 0 &&
            memcmp(s_pmkids[i].sta,   p->sta,   6) == 0 &&
            memcmp(s_pmkids[i].pmkid, p->pmkid, 16) == 0) {
            portEXIT_CRITICAL(&s_mux);
            return;
        }
    }
    if (s_pmkid_n >= MAX_PMKIDS) {
        memmove(s_pmkids, s_pmkids + 1, sizeof(c5_pmkid_t) * (MAX_PMKIDS - 1));
        s_pmkid_n = MAX_PMKIDS - 1;
    }
    memcpy(&s_pmkids[s_pmkid_n++], p, sizeof(c5_pmkid_t));
    portEXIT_CRITICAL(&s_mux);
}

static void handle_resp_hs(const c5_msg_t *m)
{
    if (m->payload_len < (int)sizeof(c5_hs_t)) return;
    const c5_hs_t *h = (const c5_hs_t *)m->payload;
    portENTER_CRITICAL(&s_mux);
    /* Dedup by (bssid, sta, anonce-tail) — same handshake won't land twice. */
    for (int i = 0; i < s_hs_n; ++i) {
        if (memcmp(s_hss[i].bssid, h->bssid, 6) == 0 &&
            memcmp(s_hss[i].sta,   h->sta,   6) == 0 &&
            memcmp(s_hss[i].anonce, h->anonce, 32) == 0) {
            portEXIT_CRITICAL(&s_mux);
            return;
        }
    }
    if (s_hs_n >= MAX_HSS) {
        memmove(s_hss, s_hss + 1, sizeof(c5_hs_t) * (MAX_HSS - 1));
        s_hs_n = MAX_HSS - 1;
    }
    memcpy(&s_hss[s_hs_n++], h, sizeof(c5_hs_t));
    portEXIT_CRITICAL(&s_mux);
    Serial.printf("[c5] HS captured bssid=%02X%02X%02X%02X%02X%02X sta=%02X%02X%02X%02X%02X%02X m2=%u\n",
                  h->bssid[0],h->bssid[1],h->bssid[2],h->bssid[3],h->bssid[4],h->bssid[5],
                  h->sta[0],h->sta[1],h->sta[2],h->sta[3],h->sta[4],h->sta[5],
                  (unsigned)h->eapol_m2_len);
}

static void handle_resp_sta(const c5_msg_t *m)
{
    if (m->payload_len < (int)sizeof(c5_sta_t)) return;
    const c5_sta_t *s = (const c5_sta_t *)m->payload;
    portENTER_CRITICAL(&s_mux);
    /* Dedup by (sta, bssid). */
    for (int i = 0; i < s_sta_n; ++i) {
        if (memcmp(s_stas[i].sta,   s->sta,   6) == 0 &&
            memcmp(s_stas[i].bssid, s->bssid, 6) == 0) {
            portEXIT_CRITICAL(&s_mux);
            return;
        }
    }
    if (s_sta_n >= MAX_STAS) {
        memmove(s_stas, s_stas + 1, sizeof(c5_sta_t) * (MAX_STAS - 1));
        s_sta_n = MAX_STAS - 1;
    }
    memcpy(&s_stas[s_sta_n++], s, sizeof(c5_sta_t));
    portEXIT_CRITICAL(&s_mux);
}

static void handle_resp_probe(const c5_msg_t *m)
{
    if (m->payload_len < (int)sizeof(c5_probe_t)) return;
    const c5_probe_t *p = (const c5_probe_t *)m->payload;
    /* ssid_len is an attacker-controlled uint8 (<=255) but ssid is char[33];
     * clamp to the max SSID length so it can't drive OOB reads in the memcmp
     * below or in the stored copy used by later dedup passes. */
    uint8_t ssid_len = p->ssid_len > 32 ? 32 : p->ssid_len;
    portENTER_CRITICAL(&s_mux);
    /* Dedup by (src, ssid). */
    for (int i = 0; i < s_probe_n; ++i) {
        if (memcmp(s_probes[i].src, p->src, 6) == 0 &&
            s_probes[i].ssid_len == ssid_len &&
            memcmp(s_probes[i].ssid, p->ssid, ssid_len) == 0) {
            portEXIT_CRITICAL(&s_mux);
            return;
        }
    }
    if (s_probe_n >= MAX_PROBES) {
        memmove(s_probes, s_probes + 1, sizeof(c5_probe_t) * (MAX_PROBES - 1));
        s_probe_n = MAX_PROBES - 1;
    }
    memcpy(&s_probes[s_probe_n], p, sizeof(c5_probe_t));
    s_probes[s_probe_n].ssid_len = ssid_len;
    s_probe_n++;
    portEXIT_CRITICAL(&s_mux);
}

static void handle_resp_deauth_hit(const c5_msg_t *m)
{
    if (m->payload_len < (int)sizeof(c5_deauth_hit_t)) return;
    const c5_deauth_hit_t *d = (const c5_deauth_hit_t *)m->payload;
    portENTER_CRITICAL(&s_mux);
    if (s_dhit_n >= MAX_DHITS) {
        memmove(s_dhits, s_dhits + 1, sizeof(c5_deauth_hit_t) * (MAX_DHITS - 1));
        s_dhit_n = MAX_DHITS - 1;
    }
    memcpy(&s_dhits[s_dhit_n++], d, sizeof(c5_deauth_hit_t));
    portEXIT_CRITICAL(&s_mux);
}

static void handle_resp_spectrum(const c5_msg_t *m)
{
    if (m->payload_len < (int)sizeof(c5_spectrum_t)) return;
    portENTER_CRITICAL(&s_mux);
    /* Latest-batch snapshot, not a ring. */
    memcpy(&s_spectrum, m->payload, sizeof(c5_spectrum_t));
    s_spectrum_valid = true;
    portEXIT_CRITICAL(&s_mux);
}

/* ESP-NOW recv callback signature differs by ESP-IDF major version.
 *   IDF 4.x (stock espressif32@6.7.0): void(const uint8_t *mac, data, len)
 *   IDF 5.x (pioarduino 55.x):         void(const esp_now_recv_info_t*, ...)
 * Guard both so platformio.ini can swap between stable and migration
 * without code churn. */
#include <esp_idf_version.h>

#if ESP_IDF_VERSION_MAJOR >= 5
static void on_recv(const esp_now_recv_info_t *recv_info,
                    const uint8_t *data, int len)
{
    const uint8_t *mac = recv_info->src_addr;
#else
static void on_recv(const uint8_t *mac, const uint8_t *data, int len)
{
#endif
    if (len < (int)sizeof(c5_msg_t)) return;
    const c5_msg_t *m = (const c5_msg_t *)data;
    if (m->magic != C5_MAGIC || m->version != C5_VERSION) return;

    ensure_peer(mac);
    switch (m->type) {
    case C5_TYPE_HELLO:      handle_hello(mac, m); break;
    case C5_TYPE_RESP_AP:    handle_resp_ap(m);    break;
    case C5_TYPE_RESP_ZB:    handle_resp_zb(m);    break;
    case C5_TYPE_RESP_PONG: {
        /* find_peer + last_seen write must be atomic vs evict_locked's
         * array compaction, or idx goes stale and we clobber a neighbour. */
        portENTER_CRITICAL(&s_mux);
        int idx = find_peer(mac);
        if (idx >= 0) s_peers[idx].last_seen = millis();
        portEXIT_CRITICAL(&s_mux);
        break;
    }
    case C5_TYPE_RESP_STATUS: {
        if (m->payload_len >= 5) {
            uint32_t f;
            memcpy(&f, m->payload, 4);
            s_last_status_frames  = f;
            s_last_status_channel = m->payload[4];
            Serial.printf("[c5] RESP_STATUS frames=%lu ch=%u\n",
                          (unsigned long)f, (unsigned)m->payload[4]);
        }
        break;
    }
    case C5_TYPE_RESP_PMKID: handle_resp_pmkid(m); break;
    case C5_TYPE_RESP_HS:    handle_resp_hs(m);    break;
    case C5_TYPE_RESP_STA:        handle_resp_sta(m);        break;
    case C5_TYPE_RESP_PROBE:      handle_resp_probe(m);      break;
    case C5_TYPE_RESP_DEAUTH_HIT: handle_resp_deauth_hit(m); break;
    case C5_TYPE_RESP_SPECTRUM:   handle_resp_spectrum(m);   break;
    }
}

bool c5_begin(void)
{
    /* ESP-NOW requires WiFi up. Check via raw IDF — WiFi.getMode()
     * reads Arduino's tracked state, which is OFF when WiFi was inited
     * via raw esp_wifi_init (e.g. Triton's wifi_lean_sta_init). Calling
     * WiFi.mode(WIFI_STA) in that case double-creates the default STA
     * netif and asserts in esp_netif_create_default_wifi_sta. */
    wifi_mode_t cur = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&cur) != ESP_OK) {
        WiFi.mode(WIFI_STA);
    }

    /* Pin to channel 1 so ESP-NOW RX matches the C5's broadcast channel.
     * A prior WiFi scan / attack can leave the driver on any channel 1-13;
     * without forcing the lock we'd silently miss HELLOs from the C5.
     * C5 also pins to channel 1 on boot. Always re-apply even if already
     * started — entering the C5 menu from another WiFi feature needs this.
     *
     * POS-AUDIT-263 / net-001 INVARIANT: c5_begin() is the only legal
     * entry point into ch1-pinned mode. Concurrent mesh / wardrive /
     * hop_task workers SHOULD NOT be running when this fires — the
     * radio_switch(RADIO_WIFI) at the feature entry above is the
     * agreed serialisation point. If a worker is mid-hop when c5_begin
     * runs, it will miss the next 1-2 channel changes (cosmetic glitch
     * in scan stats) but recover at its next iteration. Anyone adding
     * a parallel channel-hopping path must read this comment first. */
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);

    if (s_started) return true;
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

    /* POS-AUDIT-273 / net-013: production builds skip the per-tick
     * Serial.printf storm. send_simple_cmd is on the hot path of every
     * C5 command — emitting 3-4 USB-CDC lines per call buffer-stalls
     * the WiFi event task on busy CDC consumers and spams the bring-up
     * console. POSEIDON_DEBUG_C5 keeps the lines for development. */
#ifdef POSEIDON_DEBUG_C5
    wifi_mode_t mode_now = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode_now);
    uint8_t prim_ch = 0; wifi_second_chan_t sec;
    esp_wifi_get_channel(&prim_ch, &sec);
    Serial.printf("[c5-tx] type=%u seq=%u peers=%d started=%d mode=%d ch=%u\n",
                  (unsigned)type, (unsigned)seq, count, (int)s_started,
                  (int)mode_now, (unsigned)prim_ch);
#endif

    for (int i = 0; i < count; ++i) {
        esp_err_t r = esp_now_send(macs[i], (const uint8_t *)&m, sizeof(m));
#ifdef POSEIDON_DEBUG_C5
        Serial.printf("[c5-tx] unicast peer%d rc=%d (%s)\n", i, (int)r, esp_err_to_name(r));
#else
        (void)r;
#endif
    }
    esp_err_t r = esp_now_send(BROADCAST_MAC, (const uint8_t *)&m, sizeof(m));
#ifdef POSEIDON_DEBUG_C5
    Serial.printf("[c5-tx] broadcast rc=%d (%s)\n", (int)r, esp_err_to_name(r));
#else
    (void)r;
#endif
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

uint16_t c5_cmd_pmkid(const uint8_t bssid[6], uint8_t channel, uint16_t duration_ms)
{
    c5_pmkid_req_t r;
    memcpy(r.bssid, bssid, 6);
    r.channel     = channel;
    r.duration_ms = duration_ms;
    s_last_status_channel = channel;
    return send_simple_cmd(C5_TYPE_CMD_PMKID, (uint8_t *)&r, sizeof(r));
}

uint16_t c5_cmd_hs(const uint8_t bssid[6], uint8_t channel, uint16_t duration_ms)
{
    c5_hs_req_t r;
    memcpy(r.bssid, bssid, 6);
    r.channel     = channel;
    r.duration_ms = duration_ms;
    s_last_status_channel = channel;
    return send_simple_cmd(C5_TYPE_CMD_HS, (uint8_t *)&r, sizeof(r));
}

uint32_t c5_status_frames(void)  { return s_last_status_frames; }
uint8_t  c5_status_channel(void) { return s_last_status_channel; }

/* ---- v3 command senders ----
 * These build + send the request frame. The C5-side dispatcher in
 * c5_node/main/main.c does NOT yet handle these CMD types — frames
 * will be silently dropped by the satellite until each feature's
 * handler is implemented. Senders exist so POSEIDON-side menus can
 * link cleanly while the C5 firmware catches up. */

uint16_t c5_cmd_clients_hunt(uint16_t duration_ms)
{
    c5_clients_req_t r = {};
    r.channel     = 0;
    r.duration_ms = duration_ms;
    r.hop_all     = 1;
    return send_simple_cmd(C5_TYPE_CMD_CLIENTS_HUNT, (uint8_t *)&r, sizeof(r));
}

uint16_t c5_cmd_clients_ap(const uint8_t bssid[6], uint8_t channel, uint16_t duration_ms)
{
    c5_clients_req_t r = {};
    memcpy(r.target_bssid, bssid, 6);
    r.channel     = channel;
    r.duration_ms = duration_ms;
    r.hop_all     = 0;
    return send_simple_cmd(C5_TYPE_CMD_CLIENTS_AP, (uint8_t *)&r, sizeof(r));
}

uint16_t c5_cmd_beacon_spam(uint8_t channel, uint16_t duration_ms,
                            uint8_t mode, uint8_t ssid_n, const char ssids[][33])
{
    c5_beacon_spam_req_t r = {};
    r.channel     = channel;
    r.duration_ms = duration_ms;
    r.mode        = mode;
    r.ssid_n      = ssid_n > 5 ? 5 : ssid_n;
    if (ssids) {
        for (uint8_t i = 0; i < r.ssid_n; i++) {
            strncpy(r.ssids[i], ssids[i], 32);
            r.ssids[i][32] = '\0';
        }
    }
    return send_simple_cmd(C5_TYPE_CMD_BEACON_SPAM, (uint8_t *)&r, sizeof(r));
}

uint16_t c5_cmd_probe_sniff(uint8_t channel, uint16_t duration_ms)
{
    c5_probe_sniff_req_t r = { channel, duration_ms };
    return send_simple_cmd(C5_TYPE_CMD_PROBE_SNIFF, (uint8_t *)&r, sizeof(r));
}

uint16_t c5_cmd_deauth_detect(uint8_t channel, uint16_t duration_ms)
{
    c5_deauth_detect_req_t r = { channel, duration_ms };
    return send_simple_cmd(C5_TYPE_CMD_DEAUTH_DETECT, (uint8_t *)&r, sizeof(r));
}

uint16_t c5_cmd_karma(uint8_t channel, uint16_t duration_ms)
{
    c5_karma_req_t r = { channel, duration_ms };
    return send_simple_cmd(C5_TYPE_CMD_KARMA, (uint8_t *)&r, sizeof(r));
}

uint16_t c5_cmd_apclone(uint8_t channel, const char *ssid, bool open, const char *pass)
{
    c5_apclone_req_t r = {};
    r.channel = channel;
    r.open    = open ? 1 : 0;
    if (ssid) { strncpy(r.ssid, ssid, 32); r.ssid[32] = '\0'; }
    if (pass) { strncpy(r.pass, pass, 32); r.pass[32] = '\0'; }
    return send_simple_cmd(C5_TYPE_CMD_APCLONE, (uint8_t *)&r, sizeof(r));
}

uint16_t c5_cmd_spectrum(uint16_t duration_ms)
{
    c5_spectrum_req_t r = { duration_ms };
    return send_simple_cmd(C5_TYPE_CMD_SPECTRUM, (uint8_t *)&r, sizeof(r));
}

uint16_t c5_cmd_ciw(uint8_t channel, uint16_t duration_ms,
                    uint8_t category, uint16_t interval_ms)
{
    c5_ciw_req_t r = { channel, duration_ms, category, interval_ms };
    return send_simple_cmd(C5_TYPE_CMD_CIW, (uint8_t *)&r, sizeof(r));
}

/* ---- v3 result accessors ---- */

int c5_stas(c5_sta_t *out, int max)
{
    portENTER_CRITICAL(&s_mux);
    int n = s_sta_n < max ? s_sta_n : max;
    if (out && n > 0) memcpy(out, s_stas, n * sizeof(c5_sta_t));
    int total = s_sta_n;
    portEXIT_CRITICAL(&s_mux);
    return out ? n : total;
}
int c5_probes(c5_probe_t *out, int max)
{
    portENTER_CRITICAL(&s_mux);
    int n = s_probe_n < max ? s_probe_n : max;
    if (out && n > 0) memcpy(out, s_probes, n * sizeof(c5_probe_t));
    int total = s_probe_n;
    portEXIT_CRITICAL(&s_mux);
    return out ? n : total;
}
int c5_deauth_hits(c5_deauth_hit_t *out, int max)
{
    portENTER_CRITICAL(&s_mux);
    int n = s_dhit_n < max ? s_dhit_n : max;
    if (out && n > 0) memcpy(out, s_dhits, n * sizeof(c5_deauth_hit_t));
    int total = s_dhit_n;
    portEXIT_CRITICAL(&s_mux);
    return out ? n : total;
}
bool c5_spectrum_get(c5_spectrum_t *out)
{
    portENTER_CRITICAL(&s_mux);
    bool valid = s_spectrum_valid;
    if (valid && out) *out = s_spectrum;
    portEXIT_CRITICAL(&s_mux);
    return valid;
}

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
int c5_pmkids(c5_pmkid_t *out, int max)
{
    portENTER_CRITICAL(&s_mux);
    int n = s_pmkid_n < max ? s_pmkid_n : max;
    if (out && n > 0) memcpy(out, s_pmkids, n * sizeof(c5_pmkid_t));
    int total = s_pmkid_n;
    portEXIT_CRITICAL(&s_mux);
    return out ? n : total;
}
int c5_hss(c5_hs_t *out, int max)
{
    portENTER_CRITICAL(&s_mux);
    int n = s_hs_n < max ? s_hs_n : max;
    if (out && n > 0) memcpy(out, s_hss, n * sizeof(c5_hs_t));
    int total = s_hs_n;
    portEXIT_CRITICAL(&s_mux);
    return out ? n : total;
}
void c5_clear_results(void)
{
    portENTER_CRITICAL(&s_mux);
    s_ap_n = 0; s_zb_n = 0; s_pmkid_n = 0; s_hs_n = 0;
    s_sta_n = 0; s_probe_n = 0; s_dhit_n = 0;
    s_spectrum_valid = false;
    s_dbg_resp_ap_frames = 0;
    s_dbg_raw_ap_records = 0;
    portEXIT_CRITICAL(&s_mux);
}

uint32_t c5_dbg_resp_ap_frames(void) { return s_dbg_resp_ap_frames; }
uint32_t c5_dbg_raw_ap_records(void) { return s_dbg_raw_ap_records; }
