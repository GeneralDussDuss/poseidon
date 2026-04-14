/*
 * mesh.cpp — ESP-NOW broadcast presence + peer table.
 */
#include "mesh.h"
#include "gps.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <esp_system.h>

#define MESH_MAGIC   0x504F5345   /* "POSE" */
#define MESH_VERSION 1
#define MESH_TYPE_HELLO 1

static const uint8_t BROADCAST_MAC[6] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };

struct __attribute__((packed)) hello_frame_t {
    uint32_t magic;
    uint8_t  version;
    uint8_t  type;
    uint8_t  role;
    char     name[12];
    uint32_t heap_kb;
    uint8_t  has_gps;
    float    lat;
    float    lon;
};

static bool s_up = false;
static char s_name[12] = "POSEIDON";
static mesh_peer_t s_peers[MESH_MAX_PEERS];
static int      s_peer_count = 0;
static uint32_t s_tx = 0;
static uint32_t s_rx = 0;
static uint32_t s_last_hello = 0;

static int find_peer(const uint8_t *mac)
{
    for (int i = 0; i < s_peer_count; ++i)
        if (memcmp(s_peers[i].mac, mac, 6) == 0) return i;
    return -1;
}

static void on_recv(const uint8_t *mac, const uint8_t *data, int len)
{
    s_rx++;
    if (len < (int)sizeof(hello_frame_t)) return;
    const hello_frame_t *h = (const hello_frame_t *)data;
    if (h->magic != MESH_MAGIC || h->version != MESH_VERSION) return;
    if (h->type != MESH_TYPE_HELLO) return;

    int idx = find_peer(mac);
    if (idx < 0) {
        if (s_peer_count >= MESH_MAX_PEERS) return;
        idx = s_peer_count++;
        memcpy(s_peers[idx].mac, mac, 6);
    }
    mesh_peer_t &p = s_peers[idx];
    strncpy(p.name, h->name, sizeof(p.name) - 1);
    p.name[sizeof(p.name) - 1] = '\0';
    p.role      = h->role;
    p.heap_kb   = h->heap_kb;
    p.has_gps   = h->has_gps;
    p.lat       = h->lat;
    p.lon       = h->lon;
    /* Arduino ESP-NOW callback has no RSSI — leave as 0. Reach is
     * ~30-50m typical indoors; if you need RSSI, use the newer
     * esp_now_recv_info_t API in a future ESP-IDF upgrade. */
    p.rssi      = 0;
    p.last_seen = millis();
}

static void send_hello(void)
{
    hello_frame_t h = {
        .magic   = MESH_MAGIC,
        .version = MESH_VERSION,
        .type    = MESH_TYPE_HELLO,
        .role    = 0,
        .name    = {0},
        .heap_kb = esp_get_free_heap_size() / 1024,
        .has_gps = 0,
        .lat     = 0, .lon = 0,
    };
    strncpy(h.name, s_name, sizeof(h.name) - 1);

    const gps_fix_t &g = gps_get();
    if (g.valid) {
        h.has_gps = 1;
        h.lat = (float)g.lat_deg;
        h.lon = (float)g.lon_deg;
    }

    esp_now_send(BROADCAST_MAC, (const uint8_t *)&h, sizeof(h));
    s_tx++;
}

static void mesh_task(void *)
{
    while (s_up) {
        uint32_t now = millis();
        if (now - s_last_hello >= MESH_HELLO_MS) {
            s_last_hello = now;
            send_hello();
        }
        /* Evict stale peers. */
        for (int i = s_peer_count - 1; i >= 0; --i) {
            if (now - s_peers[i].last_seen > MESH_TIMEOUT_MS) {
                s_peers[i] = s_peers[--s_peer_count];
            }
        }
        delay(500);
    }
    vTaskDelete(nullptr);
}

bool mesh_begin(const char *node_name)
{
    if (s_up) return true;
    if (node_name) {
        strncpy(s_name, node_name, sizeof(s_name) - 1);
        s_name[sizeof(s_name) - 1] = '\0';
    }

    /* ESP-NOW needs WiFi initialized. Station mode, disconnected. */
    if (WiFi.getMode() == WIFI_OFF) WiFi.mode(WIFI_STA);
    if (esp_now_init() != ESP_OK) return false;
    esp_now_register_recv_cb(on_recv);

    esp_now_peer_info_t pi = {};
    memcpy(pi.peer_addr, BROADCAST_MAC, 6);
    pi.channel = 0;
    pi.encrypt = false;
    if (!esp_now_is_peer_exist(BROADCAST_MAC)) esp_now_add_peer(&pi);

    s_up = true;
    s_peer_count = 0;
    s_tx = s_rx = 0;
    static TaskHandle_t s_mesh_task = nullptr;
    xTaskCreate(mesh_task, "mesh", 4096, nullptr, 3, &s_mesh_task);
    return true;
}

void mesh_stop(void)
{
    if (!s_up) return;
    s_up = false;
    /* Let the task observe s_up = false and self-delete. */
    for (int i = 0; i < 30; ++i) {
        if (!s_up) delay(50);
    }
    esp_now_deinit();
    s_peer_count = 0;
}

int mesh_peers(mesh_peer_t *out, int max)
{
    int n = s_peer_count < max ? s_peer_count : max;
    memcpy(out, s_peers, n * sizeof(mesh_peer_t));
    return n;
}

uint32_t mesh_tx_count(void) { return s_tx; }
uint32_t mesh_rx_count(void) { return s_rx; }
