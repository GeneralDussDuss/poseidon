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
#include "ui.h"
#include "input.h"
#include "radio.h"
#include <WiFi.h>
#include <esp_wifi.h>

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

/* Raw 802.11 beacon frame template (IEEE 802.11 probe response style). */
static uint8_t s_beacon[128] = {
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

/* Append the post-SSID tags (rates + DS). Returns total frame length. */
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
    /* DS parameter set (current channel) */
    *p++ = 0x03; *p++ = 0x01; *p++ = ch;
    return (int)(p - s_beacon);
}

/* User-entered or selected list. */
static const char **s_list = s_meme_ssids;
static int          s_list_n = MEME_COUNT;
static char         s_custom[MAX_SPAM][33];
static int          s_custom_n = 0;
static volatile bool     s_running = false;
static volatile uint32_t s_sent    = 0;

static void spam_task(void *)
{
    uint8_t bssid[6] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x00 };
    int i = 0;
    while (s_running) {
        const char *ssid = s_list_n > 0 ? s_list[i % s_list_n] :
                           s_custom_n > 0 ? s_custom[i % s_custom_n] : "POSEIDON";
        /* rotate bssid low bytes so scanners see distinct "APs" */
        bssid[4] = (uint8_t)(i >> 8);
        bssid[5] = (uint8_t)(i & 0xFF);
        uint8_t ch = 1 + (i % 11);
        esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
        int len = build_beacon(ssid, ch, bssid);
        esp_wifi_80211_tx(WIFI_IF_STA, s_beacon, len, false);
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
    d.setTextColor(COL_ACCENT, COL_BG);
    d.setCursor(4, BODY_Y + 2);   d.print("BEACON SPAM");
    d.drawFastHLine(4, BODY_Y + 12, 120, COL_ACCENT);
    d.setTextColor(COL_FG, COL_BG);
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
    WiFi.mode(WIFI_STA);
    esp_wifi_set_promiscuous(true);   /* required for raw 802.11 TX */

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
    d.setTextColor(COL_BAD, COL_BG);
    d.setCursor(4, BODY_Y + 2); d.print("BEACON SPAM ACTIVE");
    d.drawFastHLine(4, BODY_Y + 12, 180, COL_BAD);

    uint32_t last = 0;
    int n = s_list_n > 0 ? s_list_n : s_custom_n;
    while (true) {
        if (millis() - last > 300) {
            last = millis();
            d.fillRect(0, BODY_Y + 20, SCR_W, 40, COL_BG);
            d.setTextColor(COL_FG, COL_BG);
            d.setCursor(4, BODY_Y + 20); d.printf("SSIDs: %d", n);
            d.setCursor(4, BODY_Y + 32); d.printf("sent:  %lu", (unsigned long)s_sent);
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
