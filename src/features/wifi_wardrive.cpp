/*
 * wifi_wardrive — channel-hopping beacon logger → WiGLE v1.6 CSV.
 *
 * Requires:
 *   - GPS fix from the M5Stack LoRa-GNSS HAT (NMEA on UART1)
 *   - SD card mounted (M5Cardputer.Display.getSDCard() or sd_mount())
 *
 * Output: /poseidon/wigle-YYYYMMDD-HHMMSS.csv with the standard
 * WiGLE CSV v1.6 header. Rows are deduped by BSSID — stronger RSSI
 * + latest GPS fix win.
 */
#include "app.h"
#include "../theme.h"
#include "ui.h"
#include "input.h"
#include "radio.h"
#include "gps.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <SD.h>
#include "../sd_helper.h"

static portMUX_TYPE s_wdr_mux = portMUX_INITIALIZER_UNLOCKED;

#define WARDRIVE_MAX_APS 256

struct wdr_ap_t {
    uint8_t  bssid[6];
    char     ssid[33];
    int8_t   rssi;
    uint8_t  channel;
    uint8_t  auth;
    double   lat;
    double   lon;
    float    alt;
    uint32_t first_seen;
    uint32_t last_seen;
    bool     dirty;
};

static wdr_ap_t  s_aps[WARDRIVE_MAX_APS];
static int       s_ap_count = 0;
static volatile bool s_running = false;
static volatile uint32_t s_beacons = 0;
static volatile uint8_t  s_current_ch = 1;
static File       s_csv;
static char       s_csv_path[64] = {0};

static int find_ap(const uint8_t *bssid)
{
    for (int i = 0; i < s_ap_count; ++i)
        if (memcmp(s_aps[i].bssid, bssid, 6) == 0) return i;
    return -1;
}

/* WiGLE v1.6 header + metadata line */
static bool wdr_open_csv(void)
{
    gps_fix_t g;
    if (!gps_snapshot(&g)) {
        /* No GPS fix yet — write with placeholder timestamp. */
        g.utc[0] = '\0';
        g.date[0] = '\0';
    }
    snprintf(s_csv_path, sizeof(s_csv_path),
             "/poseidon/wigle-%lu.csv", (unsigned long)(millis() / 1000));
    SD.mkdir("/poseidon");
    s_csv = SD.open(s_csv_path, FILE_WRITE);
    if (!s_csv) return false;

    /* WiGLE requires a pre-header meta line. */
    s_csv.println("WigleWifi-1.6,appRelease=POSEIDON," POSEIDON_VERSION ",model=M5Cardputer,release=1,device=POSEIDON,display=ST7789,board=ESP32S3,brand=M5Stack");
    s_csv.println("MAC,SSID,AuthMode,FirstSeen,Channel,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,Type");
    s_csv.flush();
    return true;
}

static const char *auth_to_wigle(uint8_t a)
{
    switch (a) {
    case WIFI_AUTH_OPEN:          return "[ESS]";
    case WIFI_AUTH_WEP:           return "[WEP][ESS]";
    case WIFI_AUTH_WPA_PSK:       return "[WPA-PSK-CCMP][ESS]";
    case WIFI_AUTH_WPA2_PSK:      return "[WPA2-PSK-CCMP][ESS]";
    case WIFI_AUTH_WPA_WPA2_PSK:  return "[WPA-PSK-CCMP][WPA2-PSK-CCMP][ESS]";
    case WIFI_AUTH_WPA3_PSK:      return "[WPA3-SAE-CCMP][ESS]";
    default:                      return "[ESS]";
    }
}

static void flush_dirty_rows(void)
{
    if (!s_csv) return;
    gps_fix_t g;
    bool have_gps = gps_snapshot(&g);
    for (int i = 0; i < s_ap_count; ++i) {
        wdr_ap_t &a = s_aps[i];
        if (!a.dirty) continue;
        a.dirty = false;
        s_csv.printf("%02X:%02X:%02X:%02X:%02X:%02X,%s,%s,%s,%u,%d,%.6f,%.6f,%.1f,5,WIFI\n",
                     a.bssid[0], a.bssid[1], a.bssid[2],
                     a.bssid[3], a.bssid[4], a.bssid[5],
                     a.ssid, auth_to_wigle(a.auth),
                     have_gps ? g.date : "",
                     a.channel, a.rssi, a.lat, a.lon, a.alt);
    }
    s_csv.flush();
}

static void promisc_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_MGMT) return;
    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    const uint8_t *p = pkt->payload;
    if (pkt->rx_ctrl.sig_len < 36) return;
    uint8_t fc = p[0];
    uint8_t subtype = (fc >> 4) & 0xF;
    if (subtype != 0x8 && subtype != 0x5) return;

    portENTER_CRITICAL_ISR(&s_wdr_mux);
    const uint8_t *bssid = p + 16;
    int idx = find_ap(bssid);
    if (idx < 0) {
        if (s_ap_count >= WARDRIVE_MAX_APS) return;
        idx = s_ap_count++;
        memset(&s_aps[idx], 0, sizeof(wdr_ap_t));
        memcpy(s_aps[idx].bssid, bssid, 6);
        s_aps[idx].first_seen = millis();
    }
    wdr_ap_t &a = s_aps[idx];
    a.last_seen = millis();
    s_beacons++;

    /* Parse SSID from tagged parameters (offset 36 in beacon body).
     * tag 0 = SSID. */
    const uint8_t *tags = p + 36;
    int tag_len = pkt->rx_ctrl.sig_len - 36 - 4;  /* minus FCS */
    if (tag_len > 0 && tags[0] == 0 && tags[1] <= 32) {
        memcpy(a.ssid, tags + 2, tags[1]);
        a.ssid[tags[1]] = '\0';
    }

    /* Channel is current hop. */
    a.channel = s_current_ch;

    /* Capability bits: WEP is bit 4, plus RSN/WPA info elements for WPA/2. */
    /* Quick hack: check for RSN (48) or WPA (221) in tag list. */
    int off = 2 + tags[1];
    uint8_t auth = WIFI_AUTH_OPEN;
    uint16_t cap = p[34] | (p[35] << 8);
    if (cap & (1 << 4)) auth = WIFI_AUTH_WEP;
    while (off + 1 < tag_len) {
        uint8_t tag = tags[off];
        uint8_t tlen = tags[off + 1];
        if (off + 2 + tlen > tag_len) break;
        if (tag == 48) { auth = WIFI_AUTH_WPA2_PSK; }
        else if (tag == 221 && tlen >= 4 && tags[off+2]==0x00 && tags[off+3]==0x50) {
            if (auth != WIFI_AUTH_WPA2_PSK) auth = WIFI_AUTH_WPA_PSK;
        }
        off += 2 + tlen;
    }
    a.auth = auth;

    if (pkt->rx_ctrl.rssi > a.rssi || a.rssi == 0) {
        a.rssi = pkt->rx_ctrl.rssi;
        /* Update GPS position on new best RSSI. */
        gps_fix_t g;
        if (gps_snapshot(&g)) { a.lat = g.lat_deg; a.lon = g.lon_deg; a.alt = g.alt_m; }
        a.dirty = true;
    }
    portEXIT_CRITICAL_ISR(&s_wdr_mux);
}

static void hop_task(void *)
{
    while (s_running) {
        s_current_ch = (s_current_ch % 13) + 1;
        esp_wifi_set_channel(s_current_ch, WIFI_SECOND_CHAN_NONE);
        delay(400);
    }
    vTaskDelete(nullptr);
}

void feat_wifi_wardrive(void)
{
    radio_switch(RADIO_WIFI);
    WiFi.mode(WIFI_STA);
    gps_begin();  /* idempotent */

    if (!sd_mount()) {
        ui_toast("SD card required", T_BAD, 1500);
        return;
    }
    if (!wdr_open_csv()) {
        ui_toast("cant open csv", T_BAD, 1500);
        return;
    }

    s_ap_count = 0;
    s_beacons  = 0;
    s_current_ch = 1;

    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(promisc_cb);
    esp_wifi_set_channel(s_current_ch, WIFI_SECOND_CHAN_NONE);

    s_running = true;
    xTaskCreate(hop_task, "wdr_hop", 3072, nullptr, 4, nullptr);

    ui_clear_body();
    ui_draw_footer("ESC=stop  F=flush  any=ignored");

    uint32_t last_redraw = 0;
    uint32_t last_flush  = 0;
    while (true) {
        gps_poll();
        uint32_t now = millis();

        if (now - last_flush > 3000) {
            last_flush = now;
            flush_dirty_rows();
        }
        if (now - last_redraw > 250) {
            last_redraw = now;
            auto &d = M5Cardputer.Display;
            ui_draw_status(radio_name(), "wardrive");
            ui_clear_body();
            d.setTextColor(T_ACCENT, T_BG);
            d.setCursor(4, BODY_Y + 2);  d.print("WARDRIVE");
            d.setTextColor(T_FG, T_BG);
            d.setCursor(4, BODY_Y + 18); d.printf("APs:     %d", s_ap_count);
            d.setCursor(4, BODY_Y + 30); d.printf("Beacons: %lu", (unsigned long)s_beacons);
            d.setCursor(4, BODY_Y + 42); d.printf("Channel: %u", s_current_ch);
            const gps_fix_t &g = gps_get();
            d.setTextColor(g.valid ? T_GOOD : T_DIM, T_BG);
            d.setCursor(4, BODY_Y + 54);
            if (g.valid) d.printf("GPS: %.4f, %.4f (%d sats)", g.lat_deg, g.lon_deg, g.sats);
            else         d.printf("GPS: waiting for fix...");
            d.setTextColor(T_DIM, T_BG);
            d.setCursor(4, BODY_Y + 70); d.printf("%s", s_csv_path);
        }

        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) break;
        if (k == 'f' || k == 'F') {
            flush_dirty_rows();
            ui_toast("flushed", T_GOOD, 400);
        }
    }

    s_running = false;
    esp_wifi_set_promiscuous(false);
    flush_dirty_rows();
    if (s_csv) { s_csv.close(); }
    delay(150);
}
