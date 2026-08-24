/*
 * wifi_beacon_spam — broadcast fake AP beacons.
 *
 * Built-in lists (meme, rickroll, FBI) + typed custom entries. User
 * picks a list or types their own SSIDs via the line editor.
 *
 * Each SSID gets a rotating fake BSSID and a beacon at the WiFi beacon
 * interval. 10 Hz cycle through the list so scanners pick all of them up.
 */
#include "app.h"
#include "../theme.h"
#include "ui.h"
#include "input.h"
#include "radio.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include "wifi_deauth_frame.h"   /* wifi_lean_sta_init pairs with wifi_silent_ap_begin */

#define MAX_SPAM 32

static const char *s_meme_ssids[] = {
    "FREE WIFI - CLICK HERE", "NSA Surveillance Van", "Hidden Network",
    "DEFCON", "hackme", "404 Not Found", "wtfwifi", "Tell My Wifi Love Her",
    "FBI Surveillance Van", "Abraham Linksys", "No Free WiFi Here",
    "Go Go Gadget Internet", "Skynet Global Defense", "LAN of the Free",
    "Loading...", "Virus.exe", "Pretty Fly for a Wifi", "iHackedYou",
    "Router? I hardly know her!", "Penny get your own wifi",
};
#define MEME_COUNT (sizeof(s_meme_ssids)/sizeof(s_meme_ssids[0]))

static const char *s_rick_ssids[] = {
    "Never Gonna Give You Up", "Never Gonna Let You Down",
    "Never Gonna Run Around", "Never Gonna Make You Cry",
    "Never Gonna Say Goodbye", "Never Gonna Tell A Lie",
    "You know the rules", "And so do I",
    "A full commitment",  "is what Im thinking of",
};
#define RICK_COUNT (sizeof(s_rick_ssids)/sizeof(s_rick_ssids[0]))

/* POS-AUDIT-200 / wifi-012: const template; spam_task copies into its
 * own stack-local buffer before mutating per-iteration fields (BSSID,
 * SSID length, SSID, rates, DS). Audit posited a "main↔task race" on
 * the prior static s_beacon — pick_list returns before spam_task
 * spawns so the race didn't actually exist, but const+stack-local is
 * the right pattern regardless and matches POS-AUDIT-018 on C5 side. */
static const uint8_t s_beacon_tmpl[128] = {
    0x80, 0x00, 0x00, 0x00,                              /* type: beacon */
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff,                  /* dst: broadcast */
    0,0,0,0,0,0,                                         /* src: spoofed BSSID */
    0,0,0,0,0,0,                                         /* bssid */
    0x00, 0x00,                                          /* seq */
    0x83, 0x51, 0xf7, 0x8f, 0x0f, 0x00, 0x00, 0x00,      /* timestamp */
    0x64, 0x00,                                          /* beacon interval */
    0x31, 0x04,                                          /* capabilities (ESS+Privacy) */
    0x00,                                                /* SSID tag id */
    0x00,                                                /* SSID length (filled at TX) */
    /* SSID bytes go here at runtime */
    /* Then: supported rates + DS param */
};

/* Append the post-SSID tags (rates + DS). Returns total frame length.
 * Caller passes its own 128 B `frame` buffer (initialised from
 * s_beacon_tmpl); we mutate that in place. */
static int build_beacon(uint8_t *frame, const char *ssid, uint8_t ch, uint8_t bssid[6])
{
    memcpy(frame + 10, bssid, 6);
    memcpy(frame + 16, bssid, 6);
    size_t ssid_len = strlen(ssid);
    if (ssid_len > 32) ssid_len = 32;
    frame[37] = (uint8_t)ssid_len;
    memcpy(frame + 38, ssid, ssid_len);

    uint8_t *p = frame + 38 + ssid_len;
    /* Supported rates */
    *p++ = 0x01; *p++ = 0x08;
    *p++ = 0x82; *p++ = 0x84; *p++ = 0x8B; *p++ = 0x96;
    *p++ = 0x24; *p++ = 0x30; *p++ = 0x48; *p++ = 0x6C;
    /* DS parameter set (current channel) */
    *p++ = 0x03; *p++ = 0x01; *p++ = ch;
    return (int)(p - frame);
}

/* User-entered or selected list. */
static const char **s_list = s_meme_ssids;
static int          s_list_n = MEME_COUNT;
static char         s_custom[MAX_SPAM][33];
static int          s_custom_n = 0;
static volatile bool     s_running = false;
static volatile uint32_t s_sent    = 0;
static volatile int      s_last_rc = -99;   /* last esp_wifi_80211_tx rc */

static void spam_task(void *)
{
    uint8_t bssid[6] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x00 };
    uint8_t frame[128];
    memcpy(frame, s_beacon_tmpl, sizeof(frame));
    int i = 0;
    while (s_running) {
        const char *ssid = s_list_n > 0 ? s_list[i % s_list_n] :
                           s_custom_n > 0 ? s_custom[i % s_custom_n] : "POSEIDON";
        /* rotate bssid low bytes so scanners see distinct "APs" */
        bssid[4] = (uint8_t)(i >> 8);
        bssid[5] = (uint8_t)(i & 0xFF);
        uint8_t ch = 1 + (i % 11);
        esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
        int len = build_beacon(frame, ssid, ch, bssid);
        int rc = esp_wifi_80211_tx(WIFI_IF_STA, frame, len, false);
        for (int r = 0; rc == ESP_ERR_NO_MEM && r < 4; ++r) {   /* pool full — retry like deauth */
            delay(3);
            rc = esp_wifi_80211_tx(WIFI_IF_STA, frame, len, false);
        }
        s_last_rc = rc;
        s_sent++;
        i++;
        delay(30);
    }
    vTaskDelete(nullptr);
}

static void pick_list(void)
{
    /* Tiny sub-menu: M=meme, R=rickroll, C=custom. */
    ui_clear_body();
    auto &d = M5Cardputer.Display;
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2);   d.print("BEACON SPAM");
    d.drawFastHLine(4, BODY_Y + 12, 120, T_ACCENT);
    d.setTextColor(T_FG, T_BG);
    d.setCursor(4, BODY_Y + 20);  d.print("[M] Meme SSIDs (20)");
    d.setCursor(4, BODY_Y + 32);  d.print("[R] Rickroll (10)");
    d.setCursor(4, BODY_Y + 44);  d.print("[C] Custom (type your own)");
    ui_draw_footer("letter=pick  `=back");

    s_list = nullptr;
    s_list_n = 0;
    s_custom_n = 0;

    while (true) {
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) return;

        /* Encoder path. The letter bindings below cannot be produced on the
         * T-Embed (encoder emits only ENTER/ESC/UP/DOWN/ACTIONS), so this
         * picker spun forever and the whole feature was unreachable there.
         * Custom entry now works too, since input_line() has a character
         * wheel on that board. */
        if (k == PK_ENTER || k == PK_ACTIONS) {
            static const char *const ACTS[] = { "Meme SSIDs", "Rickroll", "Custom" };
            const int pick = ui_action_menu("BEACON SPAM", ACTS, 3);
            if (pick == 0) { s_list = s_meme_ssids; s_list_n = MEME_COUNT; return; }
            if (pick == 1) { s_list = s_rick_ssids; s_list_n = RICK_COUNT; return; }
            if (pick == 2) { k = 'c'; }      /* fall into the custom branch */
            else           { continue; }     /* backed out -> repaint & wait */
        }

        if (k == 'm' || k == 'M') { s_list = s_meme_ssids; s_list_n = MEME_COUNT; return; }
        if (k == 'r' || k == 'R') { s_list = s_rick_ssids; s_list_n = RICK_COUNT; return; }
        if (k == 'c' || k == 'C') {
            /* Collect custom lines until ESC. */
            while (s_custom_n < MAX_SPAM) {
                char buf[33];
                char prompt[32];
                snprintf(prompt, sizeof(prompt), "SSID #%d (ESC=done):", s_custom_n + 1);
                if (!input_line(prompt, buf, sizeof(buf))) break;
                if (buf[0]) {
                    strncpy(s_custom[s_custom_n], buf, 32);
                    s_custom[s_custom_n][32] = '\0';
                    s_custom_n++;
                }
            }
            return;
        }
    }
}

void feat_wifi_beacon_spam(void)
{
    radio_switch(RADIO_WIFI);
    /* Root cause of "beacon spam does nothing": the old WiFi.mode(WIFI_STA)
     * is Arduino's, and it asserts / leaves WiFi wedged when a prior feature
     * (the scan, deauth) already raw-inited WiFi via esp_wifi_init (the netif
     * already exists). Use the exact raw-safe setup deauth uses to land raw
     * TX: lean IDF init + silent-AP begin (STA mode + promiscuous, no Arduino
     * WiFi.mode). TX stays on WIFI_IF_STA, same as deauth. */
    wifi_lean_sta_init();
    wifi_silent_ap_begin(1);

    pick_list();
    if (s_list_n == 0 && s_custom_n == 0) {
        esp_wifi_set_promiscuous(false);
        return;
    }

    s_sent = 0;
    s_running = true;
    xTaskCreate(spam_task, "spam", 3072, nullptr, 4, nullptr);

    ui_clear_body();
    ui_draw_footer("`=stop");
    auto &d = M5Cardputer.Display;
    d.setTextColor(T_BAD, T_BG);
    d.setCursor(4, BODY_Y + 2); d.print("BEACON SPAM ACTIVE");
    d.drawFastHLine(4, BODY_Y + 12, 180, T_BAD);

    uint32_t last = 0;
    int n = s_list_n > 0 ? s_list_n : s_custom_n;
    while (true) {
        if (millis() - last > 300) {
            last = millis();
            d.fillRect(0, BODY_Y + 20, SCR_W, 40, T_BG);
            d.setTextColor(T_FG, T_BG);
            d.setCursor(4, BODY_Y + 20); d.printf("SSIDs: %d", n);
            d.setCursor(4, BODY_Y + 32); d.printf("sent:  %lu", (unsigned long)s_sent);
            int rc = s_last_rc;
            d.setTextColor(rc == 0 ? T_GOOD : (rc > 0 ? T_BAD : T_DIM), T_BG);
            d.setCursor(4, BODY_Y + 44);
            d.printf("tx rc: %d %s", rc,
                     rc == 0 ? "on-air" : rc == 257 ? "NO_MEM" :
                     rc == 258 ? "filtered" : rc == 12289 ? "no-init" : "");
            d.setTextColor(T_FG, T_BG);
            ui_draw_status(radio_name(), "spam");
        }
        /* Hex packet-stream readout across the bottom. */
        ui_hexstream(4, BODY_Y + BODY_H - 30, SCR_W - 8, 28, 0xF81F);
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) break;
    }

    s_running = false;
    delay(150);
    esp_wifi_set_promiscuous(false);
}
