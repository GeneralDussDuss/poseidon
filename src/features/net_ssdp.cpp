/*
 * net_ssdp — UPnP / SSDP discovery scanner.
 *
 * Sends M-SEARCH to the SSDP multicast group (239.255.255.250:1900)
 * and collects responses from UPnP devices on the LAN. Pulls the
 * LOCATION URL from each, fetches the XML description, extracts
 * friendlyName + modelName + serialNumber. Logs to SD.
 *
 * Great for mapping internal IoT — routers, printers, IP cams, NAS,
 * smart TVs, Sonos, Chromecasts, and most smart plugs expose SSDP.
 */
#include "app.h"
#include "../theme.h"
#include "ui.h"
#include "input.h"
#include "radio.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>
#include <SD.h>
#include "../sd_helper.h"

#define MAX_DEV 16

struct ssdp_dev_t {
    char ip[16];
    char location[96];
    char friendly[40];
    char model[32];
};

static ssdp_dev_t s_dev[MAX_DEV];
static int s_dev_n = 0;

static const char *MSEARCH =
    "M-SEARCH * HTTP/1.1\r\n"
    "HOST: 239.255.255.250:1900\r\n"
    "MAN: \"ssdp:discover\"\r\n"
    "MX: 2\r\n"
    "ST: ssdp:all\r\n"
    "USER-AGENT: POSEIDON/1.0\r\n"
    "\r\n";

static void extract_tag(const String &xml, const char *tag, char *out, int out_sz)
{
    char open[32], close[32];
    snprintf(open, sizeof(open), "<%s>", tag);
    snprintf(close, sizeof(close), "</%s>", tag);
    int a = xml.indexOf(open);
    if (a < 0) { out[0] = '\0'; return; }
    a += strlen(open);
    int b = xml.indexOf(close, a);
    if (b < 0) { out[0] = '\0'; return; }
    String s = xml.substring(a, b);
    strncpy(out, s.c_str(), out_sz - 1);
    out[out_sz - 1] = '\0';
}

static void fetch_desc(ssdp_dev_t *d)
{
    HTTPClient http;
    http.setConnectTimeout(1500);
    http.setTimeout(2500);
    if (!http.begin(d->location)) return;
    int code = http.GET();
    if (code == 200) {
        String body = http.getString();
        extract_tag(body, "friendlyName", d->friendly, sizeof(d->friendly));
        extract_tag(body, "modelName",    d->model,    sizeof(d->model));
    }
    http.end();
}

void feat_net_ssdp(void)
{
    radio_switch(RADIO_WIFI);
    if (WiFi.status() != WL_CONNECTED) {
        ui_toast("connect to WiFi first", T_WARN, 1500);
        return;
    }
    s_dev_n = 0;

    WiFiUDP udp;
    udp.begin(1900);

    ui_clear_body();
    auto &d = M5Cardputer.Display;
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2); d.print("SSDP / UPnP");
    d.drawFastHLine(4, BODY_Y + 12, 90, T_ACCENT);
    d.setTextColor(T_DIM, T_BG);
    d.setCursor(4, BODY_Y + 22); d.print("sending M-SEARCH...");
    ui_draw_footer("`=stop");

    /* Three M-SEARCH bursts for good coverage. */
    IPAddress mcast(239, 255, 255, 250);
    for (int burst = 0; burst < 3; ++burst) {
        udp.beginPacket(mcast, 1900);
        udp.write((const uint8_t *)MSEARCH, strlen(MSEARCH));
        udp.endPacket();
        delay(200);
    }

    uint32_t deadline = millis() + 5000;
    while (millis() < deadline) {
        int len = udp.parsePacket();
        if (len > 0) {
            char buf[600];
            int n = udp.read((uint8_t *)buf, sizeof(buf) - 1);
            if (n <= 0) continue;
            buf[n] = '\0';
            /* Parse LOCATION header. */
            char *loc = strcasestr(buf, "LOCATION:");
            if (!loc) continue;
            loc += 9;
            while (*loc == ' ' || *loc == '\t') ++loc;
            char *eol = strstr(loc, "\r\n");
            if (!eol) continue;
            *eol = '\0';

            /* Dedup by location. */
            bool dup = false;
            for (int i = 0; i < s_dev_n; ++i)
                if (strcmp(s_dev[i].location, loc) == 0) { dup = true; break; }
            if (dup) continue;
            if (s_dev_n >= MAX_DEV) break;

            ssdp_dev_t &e = s_dev[s_dev_n];
            IPAddress rip = udp.remoteIP();
            snprintf(e.ip, sizeof(e.ip), "%u.%u.%u.%u", rip[0], rip[1], rip[2], rip[3]);
            strncpy(e.location, loc, sizeof(e.location) - 1);
            e.location[sizeof(e.location) - 1] = '\0';
            e.friendly[0] = '\0';
            e.model[0] = '\0';
            s_dev_n++;

            d.fillRect(0, BODY_Y + 22, SCR_W, 12, T_BG);
            d.setTextColor(T_FG, T_BG);
            d.setCursor(4, BODY_Y + 22);
            d.printf("found %d  %s", s_dev_n, e.ip);
        }
        if (input_poll() == PK_ESC) break;
    }
    udp.stop();

    /* Fetch each description. */
    for (int i = 0; i < s_dev_n; ++i) {
        d.fillRect(0, BODY_Y + 22, SCR_W, 12, T_BG);
        d.setTextColor(T_ACCENT, T_BG);
        d.setCursor(4, BODY_Y + 22);
        d.printf("probing %d/%d", i + 1, s_dev_n);
        fetch_desc(&s_dev[i]);
    }

    /* Save log. */
    if (sd_mount()) {
        SD.mkdir("/poseidon");
        File f = SD.open("/poseidon/ssdp.csv", FILE_APPEND);
        if (f) {
            f.println("ip,friendly,model,location");
            for (int i = 0; i < s_dev_n; ++i)
                f.printf("%s,%s,%s,%s\n", s_dev[i].ip,
                         s_dev[i].friendly, s_dev[i].model, s_dev[i].location);
            f.close();
        }
    }

    /* Scroll list. */
    int cursor = 0;
    ui_draw_footer(";/. move  `=back");
    while (true) {
        ui_clear_body();
        d.setTextColor(T_ACCENT, T_BG);
        d.setCursor(4, BODY_Y + 2);
        d.printf("UPnP %d devices", s_dev_n);
        d.drawFastHLine(4, BODY_Y + 12, 100, T_ACCENT);
        if (s_dev_n == 0) {
            d.setTextColor(T_DIM, T_BG);
            d.setCursor(4, BODY_Y + 24);
            d.print("nothing found");
        } else {
            int rows = 7;
            if (cursor < 0) cursor = 0;
            if (cursor >= s_dev_n) cursor = s_dev_n - 1;
            int first = cursor - rows / 2;
            if (first < 0) first = 0;
            if (first + rows > s_dev_n) first = max(0, s_dev_n - rows);
            for (int r = 0; r < rows && first + r < s_dev_n; ++r) {
                int y = BODY_Y + 18 + r * 12;
                bool sel = (first + r == cursor);
                if (sel) d.fillRect(0, y - 1, SCR_W, 12, 0x3007);
                const ssdp_dev_t &e = s_dev[first + r];
                d.setTextColor(sel ? 0xF81F : T_FG, sel ? 0x3007 : T_BG);
                d.setCursor(4, y);
                d.printf("%-15s", e.ip);
                d.setTextColor(sel ? 0xFFFF : T_ACCENT, sel ? 0x3007 : T_BG);
                d.setCursor(100, y);
                d.printf("%.22s", e.friendly[0] ? e.friendly : e.model[0] ? e.model : "?");
            }
        }
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(40); continue; }
        if (k == PK_ESC) return;
        if (k == ';' || k == PK_UP)   { if (cursor > 0) cursor--; }
        if (k == '.' || k == PK_DOWN) { if (cursor + 1 < s_dev_n) cursor++; }
    }
}
