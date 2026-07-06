/*
 * surveillance_hunter — passive Flock Safety ALPR + ShotSpotter Raven
 * detector. Channel-hops 2.4 GHz in promiscuous mode, parses every
 * beacon/probe, scores against sigdb_surveillance.h.
 *
 * Hits stream to two SD files:
 *   /poseidon/surv-<ts>.csv   WiGLE-format (importable into Plume tooling)
 *   /poseidon/surv-<ts>.json  JSONL one-event-per-line (Plume-compatible)
 *
 * GPS coords come from the existing gps_poll background task — same
 * source wardrive uses. Status bar shows live hit counts by class.
 *
 * BLE side (Raven manufacturer-ID 0x09C8 + custom GATT UUIDs) is a
 * follow-up — implementing here would require pausing WiFi promisc
 * and bouncing through NimBLE init/deinit, big complexity bump.
 * First cut is WiFi-only which already catches every Flock camera
 * (the high-value target). Raven side comes after this is proven.
 */
#include "app.h"
#include "../theme.h"
#include "ui.h"
#include "input.h"
#include "radio.h"
#include "menu.h"
#include "gps.h"
#include "sfx.h"
#include "../sigdb_surveillance.h"
#include "../sd_helper.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <SD.h>

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool      s_running   = false;
static volatile bool      s_hop_alive = false;
static volatile uint32_t  s_total     = 0;
static volatile uint32_t  s_hit_t1    = 0;
static volatile uint32_t  s_hit_t2    = 0;
static volatile uint32_t  s_hit_ssid  = 0;
static volatile uint32_t  s_hit_probe = 0;
static volatile uint8_t   s_current_ch = 1;
static volatile uint8_t   s_last_class = 0;
static uint8_t            s_last_bssid[6] = {0};
static char               s_last_ssid[33] = {0};
static int8_t             s_last_rssi = 0;
static File               s_csv;
static File               s_jsonl;
static char               s_csv_path[64]   = {0};
static char               s_jsonl_path[64] = {0};

/* Deferred log queue. promisc_cb runs in WiFi RX context — SD/FATFS
 * I/O from there will crash because the FATFS mutex may already be
 * held on the other core. ISR enqueues; main loop drains. */
struct hit_evt_t {
    uint8_t      bssid[6];
    char         ssid[33];
    int8_t       rssi;
    uint8_t      channel;
    surv_class_t cls;
};
#define HIT_Q_N 16
static volatile hit_evt_t s_hitq[HIT_Q_N];
static volatile uint8_t   s_hitq_head = 0;
static volatile uint8_t   s_hitq_tail = 0;

/* Dedup ring — 5s suppression by BSSID so a single chatty AP doesn't
 * spam the log with one hit every beacon interval. */
struct dedup_t { uint8_t bssid[6]; uint32_t last_ms; };
#define DEDUP_N 32
static dedup_t s_dedup[DEDUP_N];

static bool dedup_should_suppress(const uint8_t bssid[6], uint32_t now)
{
    for (int i = 0; i < DEDUP_N; i++) {
        if (memcmp(s_dedup[i].bssid, bssid, 6) == 0) {
            if (now - s_dedup[i].last_ms < 5000) return true;
            s_dedup[i].last_ms = now;
            return false;
        }
    }
    /* New entry — replace oldest. */
    int oldest = 0;
    for (int i = 1; i < DEDUP_N; i++)
        if (s_dedup[i].last_ms < s_dedup[oldest].last_ms) oldest = i;
    memcpy(s_dedup[oldest].bssid, bssid, 6);
    s_dedup[oldest].last_ms = now;
    return false;
}

static bool open_logs(void)
{
    uint32_t ts = millis() / 1000;
    snprintf(s_csv_path,   sizeof(s_csv_path),   "/poseidon/surv-%lu.csv",   (unsigned long)ts);
    snprintf(s_jsonl_path, sizeof(s_jsonl_path), "/poseidon/surv-%lu.jsonl", (unsigned long)ts);
    SD.mkdir("/poseidon");

    s_csv = SD.open(s_csv_path, FILE_WRITE);
    if (!s_csv) {
        Serial.printf("[surv] SD.open CSV failed path='%s' free_heap=%u\n",
                      s_csv_path,
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        return false;
    }
    /* WiGLE 1.6 header + Plume-compatible Type column values
     * (FLOCK-T1/T2/SSID, RAVEN-BLE/UUID). */
    s_csv.println("WigleWifi-1.6,appRelease=POSEIDON," POSEIDON_VERSION ",model=M5Cardputer,release=1,device=POSEIDON,display=ST7789,board=ESP32S3,brand=M5Stack");
    s_csv.println("MAC,SSID,AuthMode,FirstSeen,Channel,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,Type");
    s_csv.flush();

    /* JSONL is OPTIONAL — same data in JSON-lines format for jq/scripts.
     * Used to be required and aborted the whole feature if it failed,
     * but FATFS open-file limits (4 by default; SD root + CSV + JSONL
     * + any persistent handle = at limit) caused intermittent failures.
     * Now: if jsonl can't open, log a warning and continue with CSV only. */
    s_jsonl = SD.open(s_jsonl_path, FILE_WRITE);
    if (!s_jsonl) {
        Serial.printf("[surv] JSONL open failed (path='%s') — continuing CSV-only\n",
                      s_jsonl_path);
    }
    return true;
}

static void log_hit(surv_class_t cls, const uint8_t bssid[6],
                    const char *ssid, int8_t rssi, uint8_t channel)
{
    if (!s_csv) return;
    gps_fix_t g;
    bool have_gps = gps_snapshot(&g);
    const char *kind = surv_class_name(cls);

    /* Coordinates only with an actual fix — empty CSV fields / JSON null
     * otherwise, so "location unknown" is never confused with a real
     * reading at 0,0 (null island). */
    char lat[16] = "", lon[16] = "", alt[16] = "";
    if (have_gps) {
        snprintf(lat, sizeof(lat), "%.6f", g.lat_deg);
        snprintf(lon, sizeof(lon), "%.6f", g.lon_deg);
        snprintf(alt, sizeof(alt), "%.1f", g.alt_m);
    }

    s_csv.printf("%02X:%02X:%02X:%02X:%02X:%02X,%s,[FLOCK],%s,%u,%d,%s,%s,%s,10,SURV-%s\n",
                 bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5],
                 ssid ? ssid : "",
                 have_gps ? g.date : "",
                 (unsigned)channel, (int)rssi,
                 lat, lon, alt,
                 kind);
    s_csv.flush();

    if (s_jsonl) {
        char coords[48];
        if (have_gps)
            snprintf(coords, sizeof(coords), "%.6f,\"lon\":%.6f", g.lat_deg, g.lon_deg);
        else
            snprintf(coords, sizeof(coords), "null,\"lon\":null");
        s_jsonl.printf("{\"ts\":%lu,\"class\":\"%s\",\"bssid\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"ssid\":\"%s\",\"ch\":%u,\"rssi\":%d,\"lat\":%s}\n",
                       (unsigned long)millis(), kind,
                       bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5],
                       ssid ? ssid : "",
                       (unsigned)channel, (int)rssi,
                       coords);
        s_jsonl.flush();
    }
}

static void promisc_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_MGMT) return;
    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    const uint8_t *p = pkt->payload;
    int len = pkt->rx_ctrl.sig_len;
    if (len < 36) return;
    uint8_t fc = p[0];
    uint8_t subtype = (fc >> 4) & 0xF;

    /* Beacon (8) + probe response (5) + probe request (4) */
    if (subtype != 0x8 && subtype != 0x5 && subtype != 0x4) return;

    /* For probe request, addr2 = src STA. For beacon/probe-resp, addr2 = AP BSSID. */
    const uint8_t *bssid = (subtype == 0x4) ? (p + 10) : (p + 16);

    portENTER_CRITICAL_ISR(&s_mux);
    s_total = s_total + 1;
    portEXIT_CRITICAL_ISR(&s_mux);

    surv_class_t cls = SURV_UNKNOWN;

    /* OUI classification first — cheapest. */
    cls = flock_classify_oui(bssid);

    /* SSID extraction + classification for beacon / probe-resp / probe-req
     * (all carry tagged params starting at offset 36 in beacon/probe-resp,
     * offset 24 in probe-req). */
    char ssid[33] = {0};
    const uint8_t *tags = (subtype == 0x4) ? (p + 24) : (p + 36);
    int tag_len = len - (tags - p) - 4;
    if (tag_len > 1 && tags[0] == 0 && tags[1] <= 32 && tags[1] <= (tag_len - 2)) {
        memcpy(ssid, tags + 2, tags[1]);
        ssid[tags[1]] = '\0';
    }

    /* DeFlockJoplin wildcard-probe fingerprint: probe-req with empty
     * SSID + known-Flock OUI source = definitive. */
    if (subtype == 0x4 && ssid[0] == '\0' && cls != SURV_UNKNOWN) {
        cls = SURV_FLOCK_PROBE;
    }

    /* SSID-pattern override (catches Flock APs on contract-manuf OUIs
     * that wouldn't trigger OUI classification alone). */
    if (cls == SURV_UNKNOWN || cls == SURV_FLOCK_T2) {
        surv_class_t s_cls = flock_classify_ssid(ssid);
        if (s_cls != SURV_UNKNOWN) cls = s_cls;
    }

    if (cls == SURV_UNKNOWN) return;

    /* Cache the latest for UI display. */
    portENTER_CRITICAL_ISR(&s_mux);
    memcpy(s_last_bssid, bssid, 6);
    strncpy(s_last_ssid, ssid, sizeof(s_last_ssid) - 1);
    s_last_ssid[sizeof(s_last_ssid) - 1] = '\0';
    s_last_rssi  = pkt->rx_ctrl.rssi;
    s_last_class = (uint8_t)cls;
    switch (cls) {
    case SURV_FLOCK_T1:    s_hit_t1++;    break;
    case SURV_FLOCK_T2:    s_hit_t2++;    break;
    case SURV_FLOCK_SSID:  s_hit_ssid++;  break;
    case SURV_FLOCK_PROBE: s_hit_probe++; break;
    default: break;
    }
    portEXIT_CRITICAL_ISR(&s_mux);

    /* Suppress dedup-spammed hits but still count them. */
    if (dedup_should_suppress(bssid, millis())) return;

    /* Enqueue — main loop drains and writes to SD. NEVER touch SD or
     * gps_snapshot from this ISR-class callback. */
    uint8_t head = s_hitq_head;
    uint8_t next = (head + 1) % HIT_Q_N;
    if (next != s_hitq_tail) {
        memcpy((void *)s_hitq[head].bssid, bssid, 6);
        size_t sl = strnlen(ssid, sizeof(ssid));
        memcpy((void *)s_hitq[head].ssid, ssid, sl);
        ((char *)s_hitq[head].ssid)[sl] = '\0';
        s_hitq[head].rssi    = pkt->rx_ctrl.rssi;
        s_hitq[head].channel = s_current_ch;
        s_hitq[head].cls     = cls;
        s_hitq_head = next;
    }
}

/* Drain the enqueue ring on the main loop side. Safe to call SD +
 * gps_snapshot here. */
static void drain_hit_queue(void)
{
    while (s_hitq_head != s_hitq_tail) {
        portENTER_CRITICAL(&s_mux);
        uint8_t tail = s_hitq_tail;
        hit_evt_t e;
        memcpy(&e, (const void *)&s_hitq[tail], sizeof(e));
        s_hitq_tail = (tail + 1) % HIT_Q_N;
        portEXIT_CRITICAL(&s_mux);
        log_hit(e.cls, e.bssid, e.ssid, e.rssi, e.channel);
    }
}

static void hop_task(void *)
{
    s_hop_alive = true;
    while (s_running) {
        s_current_ch = (s_current_ch % 13) + 1;
        esp_wifi_set_channel(s_current_ch, WIFI_SECOND_CHAN_NONE);
        delay(300);
    }
    s_hop_alive = false;
    vTaskDelete(nullptr);
}

void feat_surveillance_hunter(void)
{
    /* SD mount before WiFi init — same reasoning as wardrive (FATFS
     * mount can fail under WiFi heap pressure). */
    if (!sd_mount() && !sd_remount()) {
        ui_toast("SD mount failed", T_BAD, 1500);
        return;
    }
    radio_switch(RADIO_WIFI);
    wifi_lean_sta_init();   /* raw-safe STA init; Arduino WiFi.mode() asserts / NO_MEMs. */
    /* OPSEC: GPS only if the user explicitly opted in. Otherwise hits are
     * logged without coordinates (have_gps stays false in log_hit). */
    if (gps_user_enabled()) gps_begin();

    if (!open_logs()) {
        ui_toast("cant open log files", T_BAD, 1500);
        return;
    }

    /* Reset state */
    s_total = s_hit_t1 = s_hit_t2 = s_hit_ssid = s_hit_probe = 0;
    s_current_ch = 1;
    s_last_class = 0;
    s_last_ssid[0] = '\0';
    memset(s_dedup, 0, sizeof(s_dedup));

    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(promisc_cb);
    esp_wifi_set_channel(s_current_ch, WIFI_SECOND_CHAN_NONE);

    s_running = true;
    xTaskCreate(hop_task, "surv_hop", 3072, nullptr, 4, nullptr);

    auto &d = M5Cardputer.Display;
    ui_clear_body();
    ui_draw_footer("ESC=stop  any other key=ignored");

    uint32_t last_redraw = 0;
    bool dirty = true;
    while (true) {
        gps_poll();
        drain_hit_queue();   /* main-loop SD writes only */
        uint32_t now = millis();
        if (now - last_redraw > 250) {
            last_redraw = now;
            ui_draw_status(radio_name(), "surveillance");
            if (dirty) {
                ui_clear_body();
                d.setTextColor(T_BAD, T_BG);
                d.setCursor(4, BODY_Y + 2); d.print("SURVEILLANCE AUDIT");
                d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_BAD);
                dirty = false;
            }
            d.setTextColor(T_FG, T_BG);
            d.setCursor(4, BODY_Y + 18); d.printf("frames : %-7lu",  (unsigned long)s_total);
            d.setCursor(4, BODY_Y + 28); d.printf("channel: %-2u",   s_current_ch);

            d.setTextColor(T_BAD,    T_BG); d.setCursor(4,   BODY_Y + 42); d.printf("T1   %-4lu", (unsigned long)s_hit_t1);
            d.setTextColor(T_WARN,   T_BG); d.setCursor(60,  BODY_Y + 42); d.printf("T2   %-4lu", (unsigned long)s_hit_t2);
            d.setTextColor(T_ACCENT, T_BG); d.setCursor(116, BODY_Y + 42); d.printf("SSID %-4lu", (unsigned long)s_hit_ssid);
            d.setTextColor(T_ACCENT2,T_BG); d.setCursor(172, BODY_Y + 42); d.printf("PRB %-4lu",  (unsigned long)s_hit_probe);

            /* Latest hit. */
            d.setTextColor(T_DIM, T_BG);
            d.setCursor(4, BODY_Y + 56); d.print("latest:");
            if (s_last_class != SURV_UNKNOWN) {
                d.setTextColor(T_GOOD, T_BG);
                d.setCursor(4, BODY_Y + 68);
                d.printf("%-12s %d dBm   ",
                         surv_class_name((surv_class_t)s_last_class), (int)s_last_rssi);
                d.setCursor(4, BODY_Y + 78);
                d.printf("%02X:%02X:%02X:%02X:%02X:%02X %.16s    ",
                         s_last_bssid[0], s_last_bssid[1], s_last_bssid[2],
                         s_last_bssid[3], s_last_bssid[4], s_last_bssid[5],
                         s_last_ssid[0] ? s_last_ssid : "<no-ssid>");
            } else {
                d.setTextColor(T_DIM, T_BG);
                d.setCursor(4, BODY_Y + 68);
                d.print("(no hits yet)             ");
                d.setCursor(4, BODY_Y + 78);
                d.print("                                  ");
            }
        }
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) break;
    }

    s_running = false;
    /* Join the hop task before returning — its loop period (300ms) is
     * longer than the old delay(150), so it could still be live when the
     * next feature inits the radio. Bounded wait. */
    uint32_t deadline = millis() + 800;
    while (s_hop_alive && millis() < deadline) delay(5);
    esp_wifi_set_promiscuous(false);
    if (s_csv)   s_csv.close();
    if (s_jsonl) s_jsonl.close();
    delay(150);
}
