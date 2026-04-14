/*
 * net_tools — basic network utilities run from STA mode.
 *
 * Port scan: iterate a target host's TCP ports, log which connect.
 * Ping: ICMP echo to a host, round-trip time.
 * DNS: lookup a hostname's A records.
 *
 * All require you to be associated to a WiFi AP first — via a saved
 * network (feat_wifi_connect) or by adding one in settings.
 */
#include "app.h"
#include "ui.h"
#include "input.h"
#include "radio.h"
#include <WiFi.h>
#include <ESP32Ping.h>

static void draw_waiting(const char *title, const char *sub)
{
    auto &d = M5Cardputer.Display;
    ui_clear_body();
    d.setTextColor(COL_ACCENT, COL_BG);
    d.setCursor(4, BODY_Y + 2); d.print(title);
    d.drawFastHLine(4, BODY_Y + 12, 100, COL_ACCENT);
    d.setTextColor(COL_FG, COL_BG);
    d.setCursor(4, BODY_Y + 24); d.print(sub);
}

static bool require_sta(void)
{
    if (WiFi.status() != WL_CONNECTED) {
        ui_toast("connect to WiFi first", COL_WARN, 1500);
        return false;
    }
    return true;
}

void feat_net_portscan(void)
{
    radio_switch(RADIO_WIFI);
    if (!require_sta()) return;
    char host[64], range_buf[16];
    if (!input_line("host (ip or name):", host, sizeof(host))) return;
    if (!input_line("port range (22-443):", range_buf, sizeof(range_buf))) return;
    int lo = 1, hi = 1024;
    sscanf(range_buf, "%d-%d", &lo, &hi);
    if (lo < 1) lo = 1;
    if (hi > 65535) hi = 65535;
    if (hi < lo) hi = lo;

    IPAddress ip;
    if (!WiFi.hostByName(host, ip)) {
        ui_toast("dns fail", COL_BAD, 1200);
        return;
    }

    draw_waiting("PORT SCAN", host);
    ui_draw_footer("`=stop");
    auto &d = M5Cardputer.Display;
    int y = BODY_Y + 36;

    int open_count = 0;
    for (int p = lo; p <= hi; ++p) {
        d.fillRect(0, BODY_Y + 22, SCR_W, 12, COL_BG);
        d.setTextColor(COL_DIM, COL_BG);
        d.setCursor(4, BODY_Y + 22);
        d.printf("port %d...", p);
        WiFiClient c;
        c.setTimeout(300);
        if (c.connect(ip, p)) {
            d.setTextColor(COL_GOOD, COL_BG);
            d.setCursor(4, y);
            d.printf("OPEN %d", p);
            y += 10;
            if (y > FOOTER_Y - 12) y = BODY_Y + 36;
            open_count++;
            c.stop();
        }
        uint16_t k = input_poll();
        if (k == PK_ESC) break;
    }
    d.fillRect(0, BODY_Y + 22, SCR_W, 12, COL_BG);
    d.setTextColor(COL_ACCENT, COL_BG);
    d.setCursor(4, BODY_Y + 22);
    d.printf("done. %d open.", open_count);
    while (true) {
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) break;
    }
}

void feat_net_ping(void)
{
    radio_switch(RADIO_WIFI);
    if (!require_sta()) return;
    char host[64];
    if (!input_line("host:", host, sizeof(host))) return;

    draw_waiting("PING", host);
    ui_draw_footer("`=stop");
    auto &d = M5Cardputer.Display;
    int y = BODY_Y + 36;
    int seq = 0;
    while (true) {
        bool ok = Ping.ping(host, 1);
        int rtt = ok ? (int)Ping.averageTime() : -1;
        d.fillRect(0, y, SCR_W, 12, COL_BG);
        d.setTextColor(ok ? COL_GOOD : COL_BAD, COL_BG);
        d.setCursor(4, y);
        if (ok) d.printf("seq=%d rtt=%dms", seq, rtt);
        else    d.printf("seq=%d TIMEOUT", seq);
        y += 10;
        if (y > FOOTER_Y - 12) { d.fillRect(0, BODY_Y + 36, SCR_W, FOOTER_Y - BODY_Y - 36, COL_BG); y = BODY_Y + 36; }
        ++seq;

        for (int i = 0; i < 20; ++i) {
            uint16_t k = input_poll();
            if (k == PK_ESC) return;
            delay(50);
        }
    }
}

void feat_net_dns(void)
{
    radio_switch(RADIO_WIFI);
    if (!require_sta()) return;
    char host[64];
    if (!input_line("hostname:", host, sizeof(host))) return;

    IPAddress ip;
    bool ok = WiFi.hostByName(host, ip);
    ui_clear_body();
    auto &d = M5Cardputer.Display;
    d.setTextColor(COL_ACCENT, COL_BG);
    d.setCursor(4, BODY_Y + 2); d.print("DNS LOOKUP");
    d.drawFastHLine(4, BODY_Y + 12, 90, COL_ACCENT);
    d.setTextColor(COL_FG, COL_BG);
    d.setCursor(4, BODY_Y + 22); d.printf("%s", host);
    d.setTextColor(ok ? COL_GOOD : COL_BAD, COL_BG);
    d.setCursor(4, BODY_Y + 36);
    d.print(ok ? ip.toString().c_str() : "FAILED");
    ui_draw_footer("`=back");
    while (true) {
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) break;
    }
}
