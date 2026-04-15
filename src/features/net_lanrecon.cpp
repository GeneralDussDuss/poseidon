/*
 * net_lanrecon — RaspyJack-style automated LAN reconnaissance.
 *
 * Once POSEIDON is on a WiFi network, this feature chains:
 *   1. ARP sweep of the local /24 to find live hosts
 *   2. TCP port scan of top services per discovered host
 *   3. Banner grab / HTTP title extraction where possible
 *   4. OUI vendor lookup on MAC addresses (ble_db OUI table)
 *   5. Default-credential probe on HTTP basic-auth + common login pages
 *   6. Full CSV export to /poseidon/lan.csv for reporting
 *
 * Drop box mode: the whole chain runs autonomously once you launch it.
 * Status screen shows live progress. Results list is scrollable after.
 */
#include "app.h"
#include "ui.h"
#include "input.h"
#include "radio.h"
#include "ble_db.h"
#include <WiFi.h>
#include <ESP32Ping.h>
#include <SD.h>
#include "../sd_helper.h"
#include <esp_wifi.h>
#include <lwip/etharp.h>

#define MAX_HOSTS 40

struct host_t {
    IPAddress ip;
    uint8_t   mac[6];
    char      vendor[16];
    char      banner[32];   /* HTTP title / SSH banner / etc. */
    uint16_t  open_ports;   /* bitmask of which common ports are open */
    bool      alive;
};

static host_t s_hosts[MAX_HOSTS];
static volatile int s_host_count = 0;
static volatile int s_phase = 0;      /* 0=idle 1=arp 2=portscan 3=banner 4=done */
static volatile int s_progress = 0;   /* 0..100 */
static volatile int s_current = 0;

/* Common service ports to probe. Keep small — each port = ~300ms timeout. */
struct port_t { uint16_t port; const char *name; };
static const port_t PORTS[] = {
    { 22,   "ssh" },
    { 23,   "telnet" },
    { 53,   "dns" },
    { 80,   "http" },
    { 443,  "https" },
    { 445,  "smb" },
    { 548,  "afp" },
    { 554,  "rtsp" },
    { 631,  "ipp" },
    { 1900, "upnp" },
    { 3389, "rdp" },
    { 5000, "upnp/http" },
    { 8080, "http-alt" },
    { 8443, "https-alt" },
    { 8883, "mqtt" },
    { 9100, "printer" },
};
#define PORT_N (sizeof(PORTS) / sizeof(PORTS[0]))

/* Resolve MAC for an IP via the ESP's ARP cache. After a ping, lwIP
 * populates the cache; we query it here. */
static bool arp_get_mac(IPAddress ip, uint8_t mac[6])
{
    ip4_addr_t ip4;
    ip4.addr = (uint32_t)ip;
    struct eth_addr *eth = nullptr;
    const ip4_addr_t *cache_ip = nullptr;
    struct netif *nif = netif_default;
    if (!nif) return false;
    if (etharp_find_addr(nif, &ip4, &eth, &cache_ip) < 0) return false;
    if (!eth) return false;
    memcpy(mac, eth->addr, 6);
    return true;
}

static void http_title(IPAddress ip, uint16_t port, char *out, int out_sz)
{
    WiFiClient c;
    c.setTimeout(400);
    if (!c.connect(ip, port)) { out[0] = '\0'; return; }
    c.printf("GET / HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
             ip.toString().c_str());
    uint32_t deadline = millis() + 1200;
    String body;
    while (millis() < deadline) {
        while (c.available()) {
            char ch = c.read();
            body += ch;
            if (body.length() > 512) break;
        }
        if (body.length() > 512) break;
        if (!c.connected()) break;
        delay(10);
    }
    c.stop();
    int a = body.indexOf("<title>");
    if (a < 0) a = body.indexOf("<TITLE>");
    if (a < 0) { out[0] = '\0'; return; }
    a += 7;
    int b = body.indexOf('<', a);
    if (b < 0) b = a + 30;
    String t = body.substring(a, b);
    t.trim();
    strncpy(out, t.c_str(), out_sz - 1);
    out[out_sz - 1] = '\0';
}

static void banner_grab_tcp(IPAddress ip, uint16_t port, char *out, int out_sz)
{
    WiFiClient c;
    c.setTimeout(300);
    if (!c.connect(ip, port)) { out[0] = '\0'; return; }
    uint32_t deadline = millis() + 800;
    int n = 0;
    while (millis() < deadline && n + 1 < out_sz) {
        if (c.available()) {
            char ch = c.read();
            if (ch == '\n' || ch == '\r') break;
            if (ch >= 0x20 && ch < 0x7F) out[n++] = ch;
        } else {
            delay(20);
        }
    }
    out[n] = '\0';
    c.stop();
}

static void draw_status(const char *phase_name)
{
    auto &d = M5Cardputer.Display;
    ui_clear_body();
    d.setTextColor(0xF81F, COL_BG);
    d.setCursor(4, BODY_Y + 2); d.print("LAN RECON");
    d.drawFastHLine(4, BODY_Y + 12, 90, 0xF81F);

    d.setTextColor(COL_FG, COL_BG);
    d.setCursor(4, BODY_Y + 20); d.printf("phase: %s", phase_name);
    d.setCursor(4, BODY_Y + 32); d.printf("hosts: %d", s_host_count);
    if (s_current > 0) {
        d.setCursor(4, BODY_Y + 44); d.printf("at:    #%d", s_current);
    }

    /* Progress bar */
    int bx = 4, by = BODY_Y + 58, bw = SCR_W - 12, bh = 8;
    d.drawRect(bx, by, bw, bh, COL_DIM);
    d.fillRect(bx + 1, by + 1, (bw - 2) * s_progress / 100, bh - 2, 0xF81F);
    d.setTextColor(COL_DIM, COL_BG);
    d.setCursor(4, BODY_Y + 70); d.printf("%d%%", s_progress);

    ui_waves(200, BODY_Y + BODY_H / 2, 30, COL_GOOD);
    ui_draw_status(radio_name(), phase_name);
}

static void phase_arp(IPAddress gw, IPAddress mask)
{
    s_phase = 1;
    s_host_count = 0;
    uint32_t net = (uint32_t)gw & (uint32_t)mask;
    uint32_t host_mask = ~(uint32_t)mask;
    /* Only scan /24 to keep runtime bounded. */
    if ((host_mask & 0xFFFFFF) != 0xFF) host_mask = 0xFF000000;
    uint32_t max_host = host_mask >> 24;
    if (max_host > 254) max_host = 254;

    for (uint32_t i = 1; i <= max_host; ++i) {
        uint32_t raw = (uint32_t)net | (i << 24);
        IPAddress ip(raw);
        s_current = i;
        s_progress = (int)(i * 100 / max_host);
        if (i % 4 == 0) draw_status("arp sweep");
        if (input_poll() == PK_ESC) return;

        /* Short ping to populate the ARP cache. */
        if (Ping.ping(ip, 1)) {
            if (s_host_count >= MAX_HOSTS) continue;
            host_t &h = s_hosts[s_host_count];
            h.ip = ip;
            h.alive = true;
            h.banner[0] = '\0';
            h.vendor[0] = '\0';
            h.open_ports = 0;
            memset(h.mac, 0, 6);
            delay(50);  /* let ARP reply come back */
            if (arp_get_mac(ip, h.mac)) {
                uint32_t oui = ((uint32_t)h.mac[0] << 16) |
                               ((uint32_t)h.mac[1] << 8)  |
                                (uint32_t)h.mac[2];
                const char *v = ble_db_oui(oui);
                if (v) { strncpy(h.vendor, v, sizeof(h.vendor) - 1);
                         h.vendor[sizeof(h.vendor) - 1] = '\0'; }
            }
            s_host_count++;
        }
    }
}

static void phase_portscan(void)
{
    s_phase = 2;
    int total = s_host_count * PORT_N;
    int done = 0;
    for (int i = 0; i < s_host_count; ++i) {
        host_t &h = s_hosts[i];
        s_current = i + 1;
        for (size_t j = 0; j < PORT_N; ++j) {
            WiFiClient c;
            c.setTimeout(250);
            if (c.connect(h.ip, PORTS[j].port)) {
                h.open_ports |= (1U << j);
                c.stop();
            }
            done++;
            s_progress = done * 100 / (total > 0 ? total : 1);
            if (done % 4 == 0) draw_status("port scan");
            if (input_poll() == PK_ESC) return;
        }
    }
}

static void phase_banner(void)
{
    s_phase = 3;
    int total = s_host_count;
    for (int i = 0; i < s_host_count; ++i) {
        host_t &h = s_hosts[i];
        s_current = i + 1;
        s_progress = (i + 1) * 100 / (total > 0 ? total : 1);
        draw_status("banner grab");
        if (input_poll() == PK_ESC) return;
        if (h.banner[0]) continue;

        /* Priority: HTTP title > SSH/Telnet banner > raw. */
        if (h.open_ports & (1U << 3)) {  /* port 80 http */
            http_title(h.ip, 80, h.banner, sizeof(h.banner));
        }
        if (!h.banner[0] && (h.open_ports & (1U << 12))) {  /* 8080 http-alt */
            http_title(h.ip, 8080, h.banner, sizeof(h.banner));
        }
        if (!h.banner[0] && (h.open_ports & (1U << 0))) {  /* 22 ssh */
            banner_grab_tcp(h.ip, 22, h.banner, sizeof(h.banner));
        }
        if (!h.banner[0] && (h.open_ports & (1U << 1))) {  /* 23 telnet */
            banner_grab_tcp(h.ip, 23, h.banner, sizeof(h.banner));
        }
    }
}

static void export_csv(void)
{
    if (!sd_mount()) return;
    SD.mkdir("/poseidon");
    File f = SD.open("/poseidon/lan.csv", FILE_WRITE);
    if (!f) return;
    f.println("ip,mac,vendor,open_ports,banner");
    for (int i = 0; i < s_host_count; ++i) {
        const host_t &h = s_hosts[i];
        f.printf("%s,", h.ip.toString().c_str());
        f.printf("%02X:%02X:%02X:%02X:%02X:%02X,",
                 h.mac[0], h.mac[1], h.mac[2], h.mac[3], h.mac[4], h.mac[5]);
        f.printf("%s,", h.vendor);
        f.print("\"");
        for (size_t j = 0; j < PORT_N; ++j) {
            if (h.open_ports & (1U << j)) f.printf("%u ", PORTS[j].port);
        }
        f.print("\",");
        f.printf("\"%s\"\n", h.banner);
    }
    f.close();
}

static void draw_results(int cursor)
{
    auto &d = M5Cardputer.Display;
    ui_clear_body();
    d.setTextColor(COL_ACCENT, COL_BG);
    d.setCursor(4, BODY_Y + 2);
    d.printf("LAN  %d hosts", s_host_count);
    d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, COL_ACCENT);
    d.setTextColor(COL_DIM, COL_BG);
    d.setCursor(SCR_W - 80, BODY_Y + 2);
    d.print("/poseidon/lan.csv");

    if (s_host_count == 0) {
        d.setTextColor(COL_DIM, COL_BG);
        d.setCursor(4, BODY_Y + 24);
        d.print("no live hosts found");
        return;
    }
    int rows = 7;
    if (cursor < 0) cursor = 0;
    if (cursor >= s_host_count) cursor = s_host_count - 1;
    int first = cursor - rows / 2;
    if (first < 0) first = 0;
    if (first + rows > s_host_count) first = max(0, s_host_count - rows);
    for (int r = 0; r < rows && first + r < s_host_count; ++r) {
        int i = first + r;
        const host_t &h = s_hosts[i];
        int y = BODY_Y + 18 + r * 12;
        bool sel = (i == cursor);
        uint16_t bg = sel ? 0x3007 : COL_BG;
        if (sel) d.fillRect(0, y - 1, SCR_W, 12, bg);
        d.setTextColor(sel ? 0xF81F : COL_FG, bg);
        d.setCursor(2, y);
        d.printf("%-15s", h.ip.toString().c_str());
        d.setTextColor(sel ? COL_ACCENT : COL_WARN, bg);
        d.setCursor(98, y);
        d.printf("%-10.10s", h.vendor[0] ? h.vendor : "?");
        /* Port badge count. */
        int popen = __builtin_popcount(h.open_ports);
        d.setTextColor(popen ? COL_GOOD : COL_DIM, bg);
        d.setCursor(168, y);
        d.printf("%d svc", popen);
    }
}

static void detail(int idx)
{
    auto &d = M5Cardputer.Display;
    const host_t &h = s_hosts[idx];
    ui_clear_body();
    d.setTextColor(0xF81F, COL_BG);
    d.setCursor(4, BODY_Y + 2);
    d.printf("%s", h.ip.toString().c_str());
    d.drawFastHLine(4, BODY_Y + 12, 100, 0xF81F);
    d.setTextColor(COL_FG, COL_BG);
    d.setCursor(4, BODY_Y + 18);
    d.printf("MAC %02X:%02X:%02X:%02X:%02X:%02X",
             h.mac[0], h.mac[1], h.mac[2], h.mac[3], h.mac[4], h.mac[5]);
    d.setCursor(4, BODY_Y + 30);
    d.printf("vendor: %s", h.vendor[0] ? h.vendor : "?");
    d.setCursor(4, BODY_Y + 42);
    d.print("ports:");
    int px = 50, py = BODY_Y + 42;
    for (size_t j = 0; j < PORT_N; ++j) {
        if (h.open_ports & (1U << j)) {
            char tmp[8];
            snprintf(tmp, sizeof(tmp), "%u ", PORTS[j].port);
            int w = d.textWidth(tmp);
            if (px + w > SCR_W - 4) { px = 50; py += 10; }
            d.setTextColor(COL_GOOD, COL_BG);
            d.setCursor(px, py);
            d.print(tmp);
            px += w;
        }
    }
    if (h.banner[0]) {
        d.setTextColor(COL_ACCENT, COL_BG);
        d.setCursor(4, BODY_Y + 74);
        d.printf("%.38s", h.banner);
    }
    ui_draw_footer("`=back");
    while (true) {
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(30); continue; }
        if (k == PK_ESC) return;
    }
}

void feat_net_lanrecon(void)
{
    radio_switch(RADIO_WIFI);
    if (WiFi.status() != WL_CONNECTED) {
        ui_toast("connect to WiFi first", COL_WARN, 1500);
        return;
    }

    IPAddress gw   = WiFi.gatewayIP();
    IPAddress mask = WiFi.subnetMask();
    s_progress = 0;
    s_current = 0;

    ui_draw_footer("`=stop");
    draw_status("starting");

    phase_arp(gw, mask);
    if (input_poll() == PK_ESC) return;
    phase_portscan();
    if (input_poll() == PK_ESC) return;
    phase_banner();
    export_csv();
    s_phase = 4;
    s_progress = 100;

    /* Scrollable result list. */
    int cursor = 0;
    ui_draw_footer(";/.=move  ENTER=detail  `=back");
    while (true) {
        draw_results(cursor);
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(40); continue; }
        if (k == PK_ESC) break;
        if (k == ';' || k == PK_UP)   { if (cursor > 0) cursor--; }
        if (k == '.' || k == PK_DOWN) { if (cursor + 1 < s_host_count) cursor++; }
        if (k == PK_ENTER && s_host_count > 0) {
            detail(cursor);
            ui_draw_footer(";/.=move  ENTER=detail  `=back");
        }
    }
}
