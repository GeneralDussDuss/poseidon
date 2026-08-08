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
#include "menu.h"
#include "gps.h"
#include "../wifi_wardrive.h"
#include "../c5_cmd.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_heap_caps.h>
#include <SD.h>
#include "../sd_helper.h"
#include "../argus.h"
#include "wdr_mood.h"
#include "wdr_matrix.h"
#include "../sfx.h"

static portMUX_TYPE s_wdr_mux = portMUX_INITIALIZER_UNLOCKED;

/* Public AP table — persists across feature exits so Triton + others can
 * seed themselves from what we've already catalogued in this session. */
wdr_ap_t *g_wdr_aps = nullptr;

/* Allocate the 20 KB AP buffer on first wardrive use. Kept resident afterward
 * because triton/pmkid read it later in the session; freeing on exit would
 * corrupt those. Sessions that never wardrive keep the 20 KB free. */
bool wdr_aps_ensure(void)
{
    if (g_wdr_aps) return true;
    g_wdr_aps = (wdr_ap_t *)heap_caps_calloc(WARDRIVE_MAX_APS, sizeof(wdr_ap_t),
                                             MALLOC_CAP_INTERNAL);
    if (!g_wdr_aps) { ui_toast("Low memory for wardrive", T_BAD, 1500); return false; }
    return true;
}
int      g_wdr_ap_count = 0;

/* File-scope aliases for the existing internal code — keeps the diff
 * minimal. Both names refer to the same storage. */
#define s_aps      g_wdr_aps
#define s_ap_count g_wdr_ap_count
static volatile bool s_running = false;
static volatile bool s_hop_alive = false;
static volatile uint32_t s_beacons = 0;
static volatile uint8_t  s_current_ch = 1;
static volatile bool     s_c5_hold    = false;  /* true = hop task parks on ch1 for a C5 5 GHz harvest window */
static volatile int      s_5g_count = 0;   /* distinct 5 GHz APs from the C5 */
static File       s_csv;
static char       s_csv_path[64] = {0};

enum wdr_view_t { WDR_VIEW_ARGUS = 0, WDR_VIEW_PLAIN = 1, WDR_VIEW_MATRIX = 2, WDR_VIEW__COUNT = 3 };
#define MX_SEED 5   /* recent APs to preload into the matrix roster on entry */
static wdr_view_t s_view = WDR_VIEW_ARGUS;
static volatile int s_new_this_run = 0;   /* distinct new APs this session */
static uint32_t s_entry_ms = 0;           /* millis() at feature start */
static volatile uint32_t s_last_new_ms = 0;      /* millis() of most recent new AP */
static volatile bool     s_gps_ever_locked = false;
static volatile bool     s_juicy_pending = false; /* set in RX cb, consumed in UI loop */

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
    /* Data-race fix: this runs on the UI task while the promisc RX callback
     * writes s_aps[]/s_ap_count under s_wdr_mux. Snapshot the count, then
     * per row briefly enter the mux to resolve the dirty/null-island
     * bookkeeping and copy the row into a local — the SD I/O (printf) is
     * done OUTSIDE the lock so the critical section stays short. */
    int n;
    portENTER_CRITICAL(&s_wdr_mux);
    n = s_ap_count;
    portEXIT_CRITICAL(&s_wdr_mux);
    for (int i = 0; i < n; ++i) {
        wdr_ap_t snap;
        bool write_row = false;
        portENTER_CRITICAL(&s_wdr_mux);
        wdr_ap_t &a = s_aps[i];
        if (a.dirty) {
            /* POS-AUDIT-208 / wifi-016: skip rows that never had a GPS fix.
             * The previous code would write lat=0.0,lon=0.0 placeholders
             * which WiGLE silently accepts but which corrupt aggregate
             * maps — Gulf of Guinea null-island clusters from missed
             * fixes. Better to drop the row entirely; the AP stays in the
             * in-RAM table for a later flush when GPS catches up. Leave
             * dirty=true so it retries on the next flush after a fix. */
            if (!(a.lat == 0.0 && a.lon == 0.0)) {
                a.dirty = false;
                snap = a;           /* copy fields to write under the lock */
                write_row = true;
            }
        }
        portEXIT_CRITICAL(&s_wdr_mux);
        if (!write_row) continue;
        /* POS-AUDIT-207 / wifi-020: FirstSeen field left empty rather
         * than stamped with the CURRENT GPS snapshot's date — the per-AP
         * first_seen is a millis() since boot which can't be converted
         * to a wall-clock string without a stored RTC date, and we don't
         * keep per-AP first-fix dates in the struct (would add ~5 KB
         * BSS for negligible WiGLE benefit; their importer infers time
         * from the upload telemetry header). Empty is honest. */
        s_csv.printf("%02X:%02X:%02X:%02X:%02X:%02X,%s,%s,,%u,%d,%.6f,%.6f,%.1f,5,WIFI\n",
                     snap.bssid[0], snap.bssid[1], snap.bssid[2],
                     snap.bssid[3], snap.bssid[4], snap.bssid[5],
                     snap.ssid, auth_to_wigle(snap.auth),
                     snap.channel, snap.rssi, snap.lat, snap.lon, snap.alt);
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
    bool is_new_ap = false;
    if (idx < 0) {
        if (s_ap_count >= WARDRIVE_MAX_APS) {
            portEXIT_CRITICAL_ISR(&s_wdr_mux);
            return;
        }
        idx = s_ap_count++;
        is_new_ap = true;
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
    if (tag_len >= 2 && tags[0] == 0 && tags[1] <= 32 && 2 + tags[1] <= tag_len) {
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
        if (tag == 48) {
            auth = WIFI_AUTH_WPA2_PSK;
            /* Scan the RSN element for the SAE AKM suite (00-0F-AC-08) ->
             * WPA3-Personal. Transition APs list both PSK and SAE; SAE
             * present is enough to call it WPA3 for the catch banner. */
            for (int k = 0; k + 3 < tlen; ++k) {
                if (tags[off+2+k]==0x00 && tags[off+2+k+1]==0x0F &&
                    tags[off+2+k+2]==0xAC && tags[off+2+k+3]==0x08) {
                    auth = WIFI_AUTH_WPA3_PSK; break;
                }
            }
        }
        else if (tag == 221 && tlen >= 4 && tags[off+2]==0x00 && tags[off+3]==0x50) {
            if (auth != WIFI_AUTH_WPA2_PSK) auth = WIFI_AUTH_WPA_PSK;
        }
        off += 2 + tlen;
    }
    a.auth = auth;

    if (is_new_ap) {
        s_new_this_run++;
        s_last_new_ms = millis();
        if (auth == WIFI_AUTH_OPEN || auth == WIFI_AUTH_WPA3_PSK) s_juicy_pending = true;
    }

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
    s_hop_alive = true;
    while (s_running) {
        if (s_c5_hold) {                 /* C5 5 GHz window: hold ch1 so the */
            if (s_current_ch != 1) {     /* satellite's ESP-NOW batch reaches us */
                s_current_ch = 1;
                esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
            }
            delay(50);
            continue;
        }
        s_current_ch = (s_current_ch % 13) + 1;
        esp_wifi_set_channel(s_current_ch, WIFI_SECOND_CHAN_NONE);
        delay(400);
    }
    s_hop_alive = false;
    vTaskDelete(nullptr);
}

/* Fold the C5 satellite's collected 5 GHz APs into the shared wardrive
 * table so they land in the same WiGLE CSV — deduped by BSSID, tagged
 * with the current GPS fix (same null-island guard as 2.4 GHz rows).
 * Best-effort: the C5 talks ESP-NOW on ch1 but wardrive channel-hops
 * 1-13, so 5 GHz sightings are harvested on the hop's ch1 passes. Runs
 * from the UI task — shares s_wdr_mux with the promisc ISR. */
static void merge_c5_5g(void)
{
    c5_ap_t buf[32];
    int n = c5_aps(buf, 32);
    if (n <= 0) return;

    gps_fix_t g;
    bool have_gps = gps_snapshot(&g);

    for (int i = 0; i < n; ++i) {
        if (!buf[i].is_5g) continue;
        bool is_new = false;
        portENTER_CRITICAL(&s_wdr_mux);
        int idx = find_ap(buf[i].bssid);
        if (idx < 0) {
            if (s_ap_count >= WARDRIVE_MAX_APS) { portEXIT_CRITICAL(&s_wdr_mux); break; }
            idx = s_ap_count++;
            is_new = true;
            memset(&s_aps[idx], 0, sizeof(wdr_ap_t));
            memcpy(s_aps[idx].bssid, buf[i].bssid, 6);
            s_aps[idx].first_seen = millis();
        }
        wdr_ap_t &a = s_aps[idx];
        a.last_seen = millis();
        a.channel   = buf[i].channel;
        a.auth      = buf[i].auth;
        strncpy(a.ssid, buf[i].ssid, sizeof(a.ssid) - 1);
        a.ssid[sizeof(a.ssid) - 1] = '\0';
        if (buf[i].rssi > a.rssi || a.rssi == 0) {
            a.rssi = buf[i].rssi;
            if (have_gps) { a.lat = g.lat_deg; a.lon = g.lon_deg; a.alt = g.alt_m; }
            a.dirty = true;
        }
        portEXIT_CRITICAL(&s_wdr_mux);
        if (is_new) {
            s_5g_count++;
            s_new_this_run++;
            s_last_new_ms = millis();
            if (buf[i].auth == WIFI_AUTH_OPEN || buf[i].auth == WIFI_AUTH_WPA3_PSK)
                s_juicy_pending = true;
        }
    }
}

static void draw_plain_view(bool &dirty)
{
    auto &d = M5Cardputer.Display;
    if (dirty) {
        ui_clear_body();
        d.setTextColor(T_ACCENT, T_BG);
        d.setCursor(4, BODY_Y + 2);  d.print("WARDRIVE");
        dirty = false;
    }
    d.setTextColor(T_FG, T_BG);
    d.setCursor(4, BODY_Y + 18); d.printf("APs: %-5d  5G: %-4d", s_ap_count, s_5g_count);
    d.setCursor(4, BODY_Y + 30); d.printf("Beacons: %-7lu",  (unsigned long)s_beacons);
    d.setCursor(4, BODY_Y + 42); d.printf("Channel: %-2u  C5:%-3s",
                                          s_current_ch, c5_any_online() ? "on" : "off");
    const gps_fix_t &g = gps_get();
    d.setTextColor(g.valid ? T_GOOD : T_DIM, T_BG);
    d.setCursor(4, BODY_Y + 54);
    if (g.valid) d.printf("GPS: %.4f, %.4f (%d sats)   ", g.lat_deg, g.lon_deg, g.sats);
    else         d.printf("GPS: waiting for fix...      ");
    d.setTextColor(T_DIM, T_BG);
    d.setCursor(4, BODY_Y + 70); d.printf("%-30s", s_csv_path);
}

static void draw_argus_view(argus_mood_t base, bool &dirty)
{
    auto &d = M5Cardputer.Display;
    /* Clear ONCE on entry / view switch. argus_draw caches and will not
     * re-push an unchanged mood, so a per-frame clear would leave a gap.
     * Invalidate the cache here so the face repaints after the wipe (fixes
     * the "Argus vanishes after exit + re-enter" case, where mood/x/y are
     * unchanged from the previous session and the cache would skip). */
    if (dirty) { ui_clear_body(); argus_invalidate(); dirty = false; }

    argus_draw(base, 8, BODY_Y);   /* 96x96, matches Triton placement */

    const int rx = 110;            /* right stat column */
    d.setTextColor(T_FG, T_BG);
    d.setCursor(rx, BODY_Y + 2);  d.printf("APs %-5d", s_ap_count);
    d.setCursor(rx, BODY_Y + 14); d.printf("new %-5d", s_new_this_run);
    d.setCursor(rx, BODY_Y + 26); d.printf("bcn %-6lu", (unsigned long)s_beacons);
    /* ch + 5G count (magenta when a C5 satellite is feeding us) + C5 pip */
    bool c5on = c5_any_online();
    d.setCursor(rx, BODY_Y + 38);
    d.setTextColor(T_FG, T_BG);              d.printf("ch%-3u", s_current_ch);
    d.setTextColor(c5on ? T_ACCENT2 : T_DIM, T_BG); d.printf("5G%-3d", s_5g_count);
    d.setTextColor(c5on ? T_GOOD : T_DIM, T_BG);    d.printf("C5%c", c5on ? '*' : '.');

    const gps_fix_t &g = gps_get();
    d.setTextColor(g.valid ? T_GOOD : T_DIM, T_BG);
    d.setCursor(rx, BODY_Y + 50);
    if (g.valid) d.printf("GPS %c%-2d ", '*', g.sats);
    else         d.printf("no fix   ");

    d.setTextColor(T_DIM, T_BG);
    d.setCursor(rx, BODY_Y + 62);
    if (g.valid) d.printf("%-14s", s_csv_path + 10);  /* skip "/poseidon/" prefix */
    else         d.printf("holding rows ");
}

void feat_wifi_wardrive(void)
{
    /* SD mount BEFORE radio_switch — WiFi init grabs ~30 KB of heap and
     * fragments what's left, and FATFS's mount allocation can then fail
     * even on a healthy card. Mount first while heap is clean. */
    if (!sd_mount() && !sd_remount()) {
        ui_toast("SD mount failed - reseat card?", T_BAD, 1800);
        return;
    }

    /* Reserve the AP buffer while heap is still clean, before wifi init grabs
     * its ~30 KB. Bails cleanly (toast) if the 20 KB can't be allocated. */
    if (!wdr_aps_ensure()) return;

    radio_switch(RADIO_WIFI);
    wifi_lean_sta_init();
    /* Wardrive is the canonical GPS-using feature; treat entry as the
     * opt-in event (persists user_enabled to NVS so cold-boots also
     * spawn the poller). gps_ensure_running internally calls gps_begin
     * + spawns the polling task if not already running. */
    gps_ensure_running();
    if (!wdr_open_csv()) {
        /* CSV open failed despite mount — try a remount and re-open once
         * more. Covers the case where mount thinks it's good but the
         * underlying FAT state is wonky after a format. */
        if (!sd_remount() || !wdr_open_csv()) {
            ui_toast("cant open csv", T_BAD, 1500);
            return;
        }
    }

    /* Keep accumulated AP table across sessions so Triton + wifi_scan can
     * seed themselves from prior wardrive runs. Each session still writes
     * a fresh CSV; only new sightings flip dirty=true for the new file. */
    s_beacons  = 0;
    s_current_ch = 1;
    s_5g_count = 0;
    s_new_this_run = 0;
    s_entry_ms = millis();

    /* Explicit MASK_ALL filter. On IDF 5.5, NOT setting a filter (or
     * passing nullptr) silently disables capture for some frame types
     * — Triton hit this bug. Without this we wouldn't see beacons. */
    static const wifi_promiscuous_filter_t s_all_filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_ALL
    };
    esp_wifi_set_promiscuous_filter(&s_all_filter);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(promisc_cb);
    esp_wifi_set_channel(s_current_ch, WIFI_SECOND_CHAN_NONE);

    s_running = true;
    xTaskCreate(hop_task, "wdr_hop", 3072, nullptr, 4, nullptr);

    /* Bring up the C5 ESP-NOW link AFTER the hop task has its stack — ESP-NOW
     * init eats internal SRAM, and doing it first starved xTaskCreate(hop_task)
     * (silent fail → wardrive froze on channel 1). Idempotent; if a satellite
     * is present it feeds us 5 GHz APs. Note: c5_begin re-pins ch1 once, but
     * the hop task immediately resumes sweeping. */
    c5_begin();

    ui_clear_body();
    ui_draw_footer("ESC=stop  A=view  F=flush  ?=help");

    uint32_t last_redraw = 0;
    uint32_t last_flush  = 0;
    uint32_t last_c5_scan = 0;
    uint32_t last_c5_merge = 0;
    uint32_t c5_window_end = 0;
    bool dirty = true;
    bool     prev_gps_valid = false;
    int      prev_new       = 0;
    int      new_5s_ref     = 0;
    uint32_t new_5s_ref_ms  = s_entry_ms;
    int      mx_prev_apc    = s_ap_count;   /* only feed the matrix view APs seen from here on */
    bool     prev_c5        = false;
    while (true) {
        gps_poll();
        uint32_t now = millis();

        /* C5 5 GHz augmentation — hop-SYNCHRONOUS. The old "opportunistic over
         * the hop" approach logged zero 5 GHz APs: the C5 ships its result
         * batch on ch1 ~2 s after the command, by which point the hop task has
         * moved us off ch1, so the batch was never received. Fix: every ~6 s
         * open a short window where the hop task parks on ch1 (s_c5_hold) so the
         * scan command AND the streamed batch both land, then resume hopping. */
        if (c5_any_online()) {
            if (!s_c5_hold && now - last_c5_scan > 6000) {
                last_c5_scan   = now;
                s_c5_hold      = true;                 /* hop task parks on ch1 */
                esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
                s_current_ch   = 1;
                c5_clear_results();
                c5_cmd_scan_5g(2000);
                c5_window_end  = now + 2700;           /* scan dur + result margin */
            }
            if (now - last_c5_merge > 500) {           /* harvest streamed batches */
                last_c5_merge = now;
                merge_c5_5g();
            }
            if (s_c5_hold && now > c5_window_end) {     /* close window, resume hop */
                merge_c5_5g();
                s_c5_hold = false;
            }
        }

        if (now - last_flush > 3000) {
            last_flush = now;
            flush_dirty_rows();
        }
        /* Matrix view animates fast; the partial-redraw views stay at 250ms. */
        uint32_t redraw_iv = (s_view == WDR_VIEW_MATRIX) ? 40 : 250;
        if (now - last_redraw > redraw_iv) {
            last_redraw = now;
            const gps_fix_t &g = gps_get();

            /* ---- process newly discovered APs (runs in every view) ----
             * Feed the matrix decode with real SSIDs, and fire the OPEN/WPA3
             * sound cue. AP data snapshotted under the promisc mutex. */
            int apc = s_ap_count;
            if (apc > mx_prev_apc) {
                for (int i = mx_prev_apc; i < apc && i < WARDRIVE_MAX_APS; ++i) {
                    char ss[40]; uint8_t au; int8_t rs; uint8_t ch; uint8_t bb[6];
                    portENTER_CRITICAL(&s_wdr_mux);
                    strncpy(ss, s_aps[i].ssid, 33); ss[33] = 0;
                    au = s_aps[i].auth; rs = s_aps[i].rssi; ch = s_aps[i].channel;
                    memcpy(bb, s_aps[i].bssid, 6);
                    portEXIT_CRITICAL(&s_wdr_mux);
                    if (ss[0] == 0) snprintf(ss, sizeof(ss), "<hidden %02X:%02X>", bb[4], bb[5]);
                    bool juicy = (au == WIFI_AUTH_OPEN || au == WIFI_AUTH_WPA3_PSK);
                    if (s_view == WDR_VIEW_MATRIX) wdr_matrix_feed(ss, au, rs, ch);
                    if (juicy) sfx_glitch();
                }
                mx_prev_apc = apc;
            }

            /* Consume the ISR juicy flag every frame so it can't go stale and
             * strobe on a later view switch; only Argus reacts to it. */
            bool juicy_flash = s_juicy_pending; s_juicy_pending = false;

            /* C5 satellite came online -> one celebration flash + sound cue. */
            bool c5_now = c5_any_online();
            if (c5_now && !prev_c5) { argus_flash(ARGUS_PLEASED, 800); sfx_scan_hit(); }
            prev_c5 = c5_now;

            if (s_view == WDR_VIEW_MATRIX) {
                wdr_matrix_render(s_current_ch, s_ap_count, g.valid, g.sats);
            } else {
                ui_draw_status(radio_name(), "wardrive");

                /* GPS lock edges -> celebration / annoyance flashes. */
                if (g.valid && !prev_gps_valid) {
                    s_gps_ever_locked = true;
                    argus_flash(ARGUS_PLEASED, 900);
                } else if (!g.valid && prev_gps_valid) {
                    argus_flash(ARGUS_ANNOYED, 900);
                }
                prev_gps_valid = g.valid;

                /* Milestone every 50 new APs + juicy instant flash. */
                int new_now = s_new_this_run;
                if (wdr_milestone_crossed(prev_new, new_now, 50)) argus_flash(ARGUS_PLEASED, 700);
                prev_new = new_now;
                if (juicy_flash) argus_flash(ARGUS_PLEASED, 500);

                /* Sliding ~5 s window of new APs for the dense-zone mood. */
                if (now - new_5s_ref_ms >= 5000) { new_5s_ref = new_now; new_5s_ref_ms = now; }

                if (s_view == WDR_VIEW_ARGUS) {
                    wdr_mood_ctx mc;
                    mc.gps_valid       = g.valid;
                    mc.gps_ever_locked = s_gps_ever_locked;
                    mc.gps_speed_kts   = g.speed_kts;
                    mc.now_ms          = now;
                    mc.entry_ms        = s_entry_ms;
                    mc.last_new_ms     = s_last_new_ms;
                    mc.new_in_5s       = new_now - new_5s_ref;
                    mc.ap_count        = s_ap_count;
                    mc.ap_cap          = WARDRIVE_MAX_APS;
                    draw_argus_view(wdr_pick_mood(mc), dirty);
                } else {
                    draw_plain_view(dirty);
                }
            }
        }

        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) break;
        if (k == '?') { ui_show_current_help(); dirty = true; }
        if (k == PK_ACTIONS) {
            static const char *const acts[] = { "Toggle filter", "Cycle view" };
            int pick = ui_action_menu("WARDRIVE", acts, 2);
            if (pick == 0) k = 'f';
            else if (pick == 1) k = 'a';
        }
        if (k == 'f' || k == 'F') {
            flush_dirty_rows();
            ui_toast("flushed", T_GOOD, 400);
            dirty = true;
        }
        if (k == 'a' || k == 'A') {
            s_view = (wdr_view_t)((s_view + 1) % WDR_VIEW__COUNT);
            if (s_view == WDR_VIEW_MATRIX) {
                /* Seed the roster from the most recent APs, oldest->newest so
                 * the newest lands on top; new arrivals decode in live. */
                wdr_matrix_begin();
                int start = s_ap_count > MX_SEED ? s_ap_count - MX_SEED : 0;
                for (int j = start; j < s_ap_count; ++j) {
                    char ss[40]; uint8_t au; int8_t rs; uint8_t ch; uint8_t bb[6];
                    portENTER_CRITICAL(&s_wdr_mux);
                    strncpy(ss, s_aps[j].ssid, 33); ss[33] = 0;
                    au = s_aps[j].auth; rs = s_aps[j].rssi; ch = s_aps[j].channel;
                    memcpy(bb, s_aps[j].bssid, 6);
                    portEXIT_CRITICAL(&s_wdr_mux);
                    if (ss[0] == 0) snprintf(ss, sizeof(ss), "<hidden %02X:%02X>", bb[4], bb[5]);
                    wdr_matrix_seed(ss, au, rs, ch);
                }
            } else {
                /* Leaving the full-screen matrix: restore chrome the next
                 * frame (status cache was clobbered, footer overwritten). */
                ui_status_invalidate();
                ui_draw_footer("ESC=stop  A=view  F=flush  ?=help");
            }
            dirty = true;
        }
    }

    s_running = false;
    /* Join the hop task before returning — its loop period (400ms) is
     * longer than the old delay(150), so it could still be issuing
     * esp_wifi_set_channel when the next feature inits the radio. */
    uint32_t deadline = millis() + 800;
    while (s_hop_alive && millis() < deadline) delay(5);
    esp_wifi_set_promiscuous(false);
    flush_dirty_rows();
    if (s_csv) { s_csv.close(); }
    delay(150);
}
