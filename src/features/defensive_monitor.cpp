/*
 * defensive_monitor — passive counter-surveillance / anomaly detector.
 *
 * Multi-class detector running on the WiFi promisc RX path:
 *   - DEAUTH FLOOD     — rate of deauth/disassoc (subtype 0xA/0xC) from
 *                        any single source MAC exceeds threshold
 *   - EVIL TWIN        — same SSID seen from two different BSSIDs
 *                        (or strongly varying RSSI from "the same" BSSID
 *                        rapidly = position spoof candidate)
 *   - BEACON SPAM      — rate of new (never-before-seen) BSSIDs per
 *                        second exceeds threshold (Cardputer-Adv et al
 *                        beacon-spam tools fire >50 fake APs/sec)
 *
 * Alerts stream to /poseidon/defmon-<ts>.jsonl with GPS coords and
 * fire an audio cue. The live screen shows class counters + the most
 * recent alert.
 *
 * Why this is its own feature vs extending feat_wifi_deauth_detect:
 *   - That one is deauth-only with a tighter UX (just deauth rate +
 *     last source). Defensive Monitor is the broader umbrella.
 *   - Keeping both lets the user run the focused one when triaging
 *     a known deauth attack and the broader one when sweeping for
 *     anomalous activity in general.
 */
#include "app.h"
#include "../theme.h"
#include "ui.h"
#include "input.h"
#include "radio.h"
#include "gps.h"
#include "sfx.h"
#include "../sd_helper.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_heap_caps.h>
#include <SD.h>
#include <NimBLEDevice.h>

/* ---- detector thresholds ---- */
#define DM_DEAUTH_RATE_THRESHOLD     5   /* deauth/s from one src = flood    */
#define DM_DEAUTH_DEBOUNCE_WINDOWS   2   /* consecutive windows >threshold   */
#define DM_BEACON_SPAM_THRESHOLD    20   /* new BSSIDs/s = beacon spam       */
#define DM_BLE_FLOOD_THRESHOLD      15   /* new BLE addrs/s = adv flood/spam */
#define DM_ALERT_COOLDOWN_MS      4000   /* same alert class quiet period    */

/* Time-slicing: ESP32-S3 has one antenna so we can't run WiFi promisc
 * and NimBLE scan simultaneously. Alternate phases.
 *
 * POS-AUDIT-015 / dfn-001: each NimBLE init/deinit cycle churns ~30 KB
 * of heap. The previous 3 s / 2 s windows meant a deinit every 5 s —
 * long sessions silently fragmented heap until init failed and the
 * BLE phase silently stopped firing. Widened to 30 s / 15 s (12x less
 * churn). Detection latency for the BLE classes goes from "next 2 s
 * window" to "next 15 s window" — still well inside the human-attention
 * threshold for a defensive monitor + same order of magnitude as the
 * beacon-spam / deauth-flood threshold windows (1 s averaging × N
 * before alert). Full coexist pattern (NimBLE passive scan alongside
 * promisc) is the proper long-term fix and lands with POS-AUDIT-118
 * heap-policy gating. */
#define DM_PHASE_WIFI_MS          30000
#define DM_PHASE_BLE_MS           15000
/* Largest internal free block required before we dare re-init NimBLE for a
 * BLE phase. The repeated deinit/init fragments the heap (POS-AUDIT-015); if
 * the BT controller can't get a big enough contiguous block, init() aborts
 * and resets the device. Below this we skip BLE and keep WiFi monitoring. */
#define DM_BLE_MIN_BLOCK          50000

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool     s_running    = false;
static volatile bool     s_hop_alive  = false;
static volatile uint32_t s_total      = 0;
static volatile uint32_t s_deauth_now = 0;   /* current 1s-window count     */
static volatile uint32_t s_beacons_total = 0;
static volatile uint32_t s_new_bssids_now = 0;  /* new BSSIDs this 1s window */
static volatile uint8_t  s_current_ch = 1;

/* alert classes */
enum {
    DM_CLS_DEAUTH_FLOOD   = 1,
    DM_CLS_EVIL_TWIN      = 2,
    DM_CLS_BEACON_SPAM    = 3,
    DM_CLS_BCAST_DEAUTH   = 4,   /* src=00:00:00:00:00:00 — canonical attack */
    DM_CLS_WIFI_KARMA     = 5,   /* probe-resp BSSID that never beacons      */
    DM_CLS_BLE_SPOOF      = 6,   /* same advertised name from 2+ addrs       */
    DM_CLS_BLE_FLOOD      = 7,   /* new BLE addrs/s > threshold              */
    DM_CLS__MAX           = 8,
};
struct dm_alert_t {
    uint8_t  cls;
    uint8_t  bssid[6];
    char     ssid[33];
    uint8_t  channel;
    int8_t   rssi;
    uint32_t ts_ms;
    char     detail[40];
};
#define DM_Q_N 16
static volatile dm_alert_t s_alert_q[DM_Q_N];
static volatile uint8_t s_alert_q_head = 0;
static volatile uint8_t s_alert_q_tail = 0;

/* Last-alert times per class for cooldown. */
static uint32_t s_last_alert_ms[DM_CLS__MAX] = {0};

/* Per-source deauth tracker (small, LRU). Adds debounce — track count
 * of CONSECUTIVE 1s windows over threshold before firing the alert. */
struct dm_deauth_src_t {
    uint8_t  src[6];
    uint32_t count_in_window;
    uint32_t window_start_ms;
    uint8_t  consec_over;       /* DM_DEAUTH_DEBOUNCE_WINDOWS gate */
};
#define DM_DEAUTH_SRC_N 8
static volatile dm_deauth_src_t s_deauth_srcs[DM_DEAUTH_SRC_N];

/* WiFi Karma detector: BSSIDs that emit probe-resp without ever beaconing.
 * Track first-seen subtype per BSSID — if probe-resp arrives first AND
 * no beacon for the same BSSID lands within ~3 s, flag Karma. */
struct dm_karma_t {
    uint8_t  bssid[6];
    bool     saw_beacon;
    bool     saw_proberesp;
    bool     alerted;
    uint32_t first_seen_ms;
};
#define DM_KARMA_N 16
static volatile dm_karma_t s_karma[DM_KARMA_N];

/* SSID → BSSID map for evil-twin detection (LRU). 16 slots ≈ enough
 * for one room; >16 unique SSIDs and we drop the oldest. */
struct dm_ssid_t {
    char     ssid[33];
    uint8_t  bssid[6];
    uint32_t last_seen_ms;
};
#define DM_SSID_N 16
static volatile dm_ssid_t s_ssid_map[DM_SSID_N];

/* Seen-BSSIDs ring for beacon-spam rate. */
struct dm_bssid_t { uint8_t bssid[6]; uint32_t last_ms; };
#define DM_BSSID_N 64
static volatile dm_bssid_t s_bssid_seen[DM_BSSID_N];

/* BLE — name→address map for spoof/clone detection. Same name across
 * different addresses = clone or spoof. */
struct dm_ble_name_t {
    char     name[24];
    uint8_t  addr[6];
    uint32_t last_ms;
};
#define DM_BLE_NAME_N 24
static volatile dm_ble_name_t s_ble_names[DM_BLE_NAME_N];

/* BLE — seen-address ring for flood rate. */
struct dm_ble_addr_t { uint8_t addr[6]; uint32_t last_ms; };
#define DM_BLE_ADDR_N 64
static volatile dm_ble_addr_t s_ble_addrs[DM_BLE_ADDR_N];
static volatile uint32_t s_ble_total = 0;
static volatile uint32_t s_ble_new_now = 0;
static volatile uint32_t s_ble_new_last_window = 0;
static volatile uint32_t s_ble_window_start_ms = 0;

static File s_log;
static char s_log_path[64] = {0};

/* Per-class hit totals for the dashboard */
static volatile uint32_t s_alert_count[DM_CLS__MAX] = {0};

/* Phase tracking */
enum dm_phase_t { DM_PHASE_WIFI = 0, DM_PHASE_BLE = 1 };
static volatile dm_phase_t s_phase = DM_PHASE_WIFI;
static uint32_t s_phase_start_ms = 0;

/* ---- ISR-safe helpers (no SD, no Serial, no malloc) ---- */

static int find_deauth_src(const uint8_t src[6])
{
    for (int i = 0; i < DM_DEAUTH_SRC_N; i++)
        if (memcmp((const void *)s_deauth_srcs[i].src, src, 6) == 0) return i;
    return -1;
}

static int find_or_evict_ssid_slot(const char *ssid)
{
    /* match by SSID, LRU evict on miss */
    int oldest = 0;
    uint32_t oldest_ts = 0xFFFFFFFFu;
    for (int i = 0; i < DM_SSID_N; i++) {
        if (strncmp((const char *)s_ssid_map[i].ssid, ssid, 32) == 0) return i;
        if (s_ssid_map[i].last_seen_ms < oldest_ts) {
            oldest_ts = s_ssid_map[i].last_seen_ms;
            oldest = i;
        }
    }
    return oldest;
}

static bool bssid_was_seen(const uint8_t bssid[6])
{
    for (int i = 0; i < DM_BSSID_N; i++)
        if (memcmp((const void *)s_bssid_seen[i].bssid, bssid, 6) == 0) {
            ((volatile dm_bssid_t *)s_bssid_seen)[i].last_ms = millis();
            return true;
        }
    /* LRU-insert */
    int oldest = 0;
    uint32_t oldest_ts = 0xFFFFFFFFu;
    for (int i = 0; i < DM_BSSID_N; i++) {
        if (s_bssid_seen[i].last_ms < oldest_ts) {
            oldest_ts = s_bssid_seen[i].last_ms;
            oldest = i;
        }
    }
    memcpy((void *)s_bssid_seen[oldest].bssid, bssid, 6);
    s_bssid_seen[oldest].last_ms = millis();
    return false;
}

static void enqueue_alert(uint8_t cls, const uint8_t bssid[6], const char *ssid,
                          int8_t rssi, uint8_t channel, const char *detail)
{
    uint8_t head = s_alert_q_head;
    uint8_t next = (head + 1) % DM_Q_N;
    if (next == s_alert_q_tail) return;  /* full — drop */
    memcpy((void *)s_alert_q[head].bssid, bssid, 6);
    if (ssid) {
        size_t sl = strnlen(ssid, 32);
        memcpy((void *)s_alert_q[head].ssid, ssid, sl);
        ((char *)s_alert_q[head].ssid)[sl] = '\0';
    } else {
        ((char *)s_alert_q[head].ssid)[0] = '\0';
    }
    s_alert_q[head].cls     = cls;
    s_alert_q[head].rssi    = rssi;
    s_alert_q[head].channel = channel;
    s_alert_q[head].ts_ms   = millis();
    if (detail) {
        size_t dl = strnlen(detail, sizeof(s_alert_q[head].detail) - 1);
        memcpy((void *)s_alert_q[head].detail, detail, dl);
        ((char *)s_alert_q[head].detail)[dl] = '\0';
    } else {
        ((char *)s_alert_q[head].detail)[0] = '\0';
    }
    s_alert_count[cls]++;
    s_alert_q_head = next;
}

static const char *dm_class_name(uint8_t c)
{
    switch (c) {
    case DM_CLS_DEAUTH_FLOOD: return "DEAUTH-FLOOD";
    case DM_CLS_EVIL_TWIN:    return "EVIL-TWIN";
    case DM_CLS_BEACON_SPAM:  return "BEACON-SPAM";
    case DM_CLS_BCAST_DEAUTH: return "BCAST-DEAUTH";
    case DM_CLS_WIFI_KARMA:   return "WIFI-KARMA";
    case DM_CLS_BLE_SPOOF:    return "BLE-SPOOF";
    case DM_CLS_BLE_FLOOD:    return "BLE-FLOOD";
    default:                  return "UNKNOWN";
    }
}

/* ---- BLE detectors (called from NimBLE scan callback, main-loop ctx
 * — NimBLE callbacks are not ISR-class so SD writes etc. are safe but
 * we keep them out of the hot path for consistency). */

static bool ble_addr_was_seen(const uint8_t addr[6])
{
    for (int i = 0; i < DM_BLE_ADDR_N; i++)
        if (memcmp((const void *)s_ble_addrs[i].addr, addr, 6) == 0) {
            ((volatile dm_ble_addr_t *)s_ble_addrs)[i].last_ms = millis();
            return true;
        }
    int oldest = 0;
    uint32_t ots = 0xFFFFFFFFu;
    for (int i = 0; i < DM_BLE_ADDR_N; i++) {
        if (s_ble_addrs[i].last_ms < ots) {
            ots = s_ble_addrs[i].last_ms;
            oldest = i;
        }
    }
    memcpy((void *)s_ble_addrs[oldest].addr, addr, 6);
    s_ble_addrs[oldest].last_ms = millis();
    return false;
}

static void ble_on_adv(const NimBLEAdvertisedDevice *dev)
{
    s_ble_total++;
    uint8_t addr[6];
    memcpy(addr, dev->getAddress().getBase()->val, 6);

    /* Flood detector */
    if (!ble_addr_was_seen(addr)) {
        s_ble_new_now++;
    }

    /* Spoof detector — same advertised name from a different address. */
    if (!dev->haveName()) return;
    std::string name_s = dev->getName();
    if (name_s.empty()) return;
    const char *name = name_s.c_str();

    int slot = -1;
    int oldest = 0;
    uint32_t ots = 0xFFFFFFFFu;
    for (int i = 0; i < DM_BLE_NAME_N; i++) {
        if (strncmp((const char *)s_ble_names[i].name, name, 23) == 0) {
            slot = i; break;
        }
        if (s_ble_names[i].last_ms < ots) {
            ots = s_ble_names[i].last_ms;
            oldest = i;
        }
    }
    if (slot < 0) {
        slot = oldest;
        size_t nl = strnlen(name, 23);
        memcpy((void *)s_ble_names[slot].name, name, nl);
        ((char *)s_ble_names[slot].name)[nl] = '\0';
        memcpy((void *)s_ble_names[slot].addr, addr, 6);
        s_ble_names[slot].last_ms = millis();
        return;
    }
    if (memcmp((const void *)s_ble_names[slot].addr, addr, 6) != 0) {
        /* Same name, different MAC = spoof / clone */
        char detail[40];
        snprintf(detail, sizeof(detail), "name shared by 2+ MACs");
        /* POS-AUDIT-015 / dfn-002: enqueue_alert is shared with the WiFi
         * promisc_cb which holds s_mux as ISR-critical. NimBLE callbacks
         * run from the host task (regular context). Without serialisation
         * the head/tail advance in enqueue_alert races against the WiFi
         * side and corrupts the MPSC ring. Use the non-ISR variant
         * here — same s_mux as the WiFi callers consume. */
        portENTER_CRITICAL(&s_mux);
        enqueue_alert(DM_CLS_BLE_SPOOF, addr, name, dev->getRSSI(), 0, detail);
        portEXIT_CRITICAL(&s_mux);
        memcpy((void *)s_ble_names[slot].addr, addr, 6);
    }
    s_ble_names[slot].last_ms = millis();
}

/* NimBLE adapter for the C++ callback class. NimBLE 2.x signature
 * takes const* AdvertisedDevice. */
class DmScanCb : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice *dev) override { ble_on_adv(dev); }
};
static DmScanCb s_ble_cb;

/* BLE window-tick — same threshold logic as WiFi beacon spam. */
static void ble_window_tick(void)
{
    uint32_t now = millis();
    if (now - s_ble_window_start_ms < 1000) return;
    s_ble_window_start_ms = now;
    s_ble_new_last_window = s_ble_new_now;
    s_ble_new_now = 0;
    if (s_ble_new_last_window >= DM_BLE_FLOOD_THRESHOLD) {
        char detail[40];
        snprintf(detail, sizeof(detail), "%lu new BLE/s",
                 (unsigned long)s_ble_new_last_window);
        uint8_t zero[6] = {0};
        /* POS-AUDIT-015 / dfn-002: paired with the ISR-critical WiFi
         * callers. See ble_on_adv site for context. */
        portENTER_CRITICAL(&s_mux);
        enqueue_alert(DM_CLS_BLE_FLOOD, zero, nullptr, 0, 0, detail);
        portEXIT_CRITICAL(&s_mux);
    }
}

/* ---- promisc callback (ISR-class) ---- */
static void promisc_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_MGMT) return;
    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    const uint8_t *p = pkt->payload;
    int len = pkt->rx_ctrl.sig_len;
    if (len < 24) return;
    uint8_t fc = p[0];
    uint8_t subtype = (fc >> 4) & 0xF;
    s_total++;

    const uint8_t *addr2 = p + 10;  /* src (TA) */
    const uint8_t *bssid = p + 16;  /* normally BSSID; for some subtypes addr3 isn't the BSSID, fine for our purposes */

    portENTER_CRITICAL_ISR(&s_mux);

    /* ---- detector: deauth / disassoc flood + canonical-attack signature ---- */
    if (subtype == 0xC || subtype == 0xA) {
        s_deauth_now++;
        uint32_t now = millis();

        /* HydraESP signature: src=00:00:00:00:00:00 = canonical
         * Spacehuhn/deauther.cc attack. Always alert, no rate gate. */
        static const uint8_t k_bcast_zero[6] = {0};
        if (memcmp(addr2, k_bcast_zero, 6) == 0) {
            enqueue_alert(DM_CLS_BCAST_DEAUTH, addr2, nullptr,
                          pkt->rx_ctrl.rssi, s_current_ch, "src=00:..:00 (deauther.cc)");
        }

        int slot = find_deauth_src(addr2);
        if (slot < 0) {
            /* LRU evict */
            int oldest = 0;
            uint32_t ots = 0xFFFFFFFFu;
            for (int i = 0; i < DM_DEAUTH_SRC_N; i++) {
                if (s_deauth_srcs[i].window_start_ms < ots) {
                    ots = s_deauth_srcs[i].window_start_ms;
                    oldest = i;
                }
            }
            memcpy((void *)s_deauth_srcs[oldest].src, addr2, 6);
            s_deauth_srcs[oldest].count_in_window = 1;
            s_deauth_srcs[oldest].window_start_ms = now;
            s_deauth_srcs[oldest].consec_over = 0;
        } else {
            if (now - s_deauth_srcs[slot].window_start_ms > 1000) {
                /* window rollover — was last window over threshold? */
                if (s_deauth_srcs[slot].count_in_window >= DM_DEAUTH_RATE_THRESHOLD) {
                    s_deauth_srcs[slot].consec_over++;
                    if (s_deauth_srcs[slot].consec_over >= DM_DEAUTH_DEBOUNCE_WINDOWS) {
                        char detail[40];
                        snprintf(detail, sizeof(detail), "rate>%u/s x%u windows",
                                 DM_DEAUTH_RATE_THRESHOLD, s_deauth_srcs[slot].consec_over);
                        enqueue_alert(DM_CLS_DEAUTH_FLOOD, (uint8_t *)s_deauth_srcs[slot].src,
                                      nullptr, pkt->rx_ctrl.rssi, s_current_ch, detail);
                        s_deauth_srcs[slot].consec_over = 0;  /* cooldown */
                    }
                } else {
                    s_deauth_srcs[slot].consec_over = 0;
                }
                s_deauth_srcs[slot].count_in_window = 1;
                s_deauth_srcs[slot].window_start_ms = now;
            } else {
                s_deauth_srcs[slot].count_in_window++;
            }
        }
    }

    /* ---- detector: beacon spam + new-BSSID rate + Karma tracking ---- */
    if (subtype == 0x8) {  /* beacon */
        s_beacons_total++;
        if (!bssid_was_seen(bssid)) {
            s_new_bssids_now++;
        }
        /* Mark beacon-seen for the Karma tracker. */
        for (int i = 0; i < DM_KARMA_N; i++) {
            if (memcmp((const void *)s_karma[i].bssid, bssid, 6) == 0) {
                s_karma[i].saw_beacon = true;
                break;
            }
        }
    }

    /* ---- detector: WiFi Karma (probe-resp from never-beaconing AP) ---- */
    if (subtype == 0x5) {  /* probe response */
        int slot = -1;
        int oldest = 0;
        uint32_t ots = 0xFFFFFFFFu;
        for (int i = 0; i < DM_KARMA_N; i++) {
            if (memcmp((const void *)s_karma[i].bssid, bssid, 6) == 0) {
                slot = i; break;
            }
            if (s_karma[i].first_seen_ms < ots) {
                ots = s_karma[i].first_seen_ms;
                oldest = i;
            }
        }
        if (slot < 0) {
            slot = oldest;
            memcpy((void *)s_karma[slot].bssid, bssid, 6);
            s_karma[slot].saw_beacon    = false;
            s_karma[slot].saw_proberesp = false;
            s_karma[slot].alerted       = false;
            s_karma[slot].first_seen_ms = millis();
        }
        s_karma[slot].saw_proberesp = true;
        /* If we've been tracking this BSSID for >3s and STILL no beacon
         * but we've seen ≥2 probe-resps, that's the classic Karma
         * signature: an AP that ONLY responds to probes, never advertises. */
        if (!s_karma[slot].alerted &&
            !s_karma[slot].saw_beacon &&
            (millis() - s_karma[slot].first_seen_ms > 3000)) {
            s_karma[slot].alerted = true;
            char detail[40];
            snprintf(detail, sizeof(detail), "probe-resp w/o beacon");
            /* Try to extract SSID for context */
            char ssid_tmp[33] = {0};
            const uint8_t *tags = p + 36;
            int tag_len = len - 36 - 4;
            if (tag_len > 1 && tags[0] == 0 && tags[1] <= 32 && tags[1] <= (tag_len - 2)) {
                memcpy(ssid_tmp, tags + 2, tags[1]);
                ssid_tmp[tags[1]] = '\0';
            }
            enqueue_alert(DM_CLS_WIFI_KARMA, bssid, ssid_tmp,
                          pkt->rx_ctrl.rssi, s_current_ch, detail);
        }
    }

    /* ---- detector: evil twin (dup SSID with different BSSID) ---- */
    if (subtype == 0x8 || subtype == 0x5) {
        const uint8_t *tags = p + 36;
        int tag_len = len - 36 - 4;
        if (tag_len > 1 && tags[0] == 0 && tags[1] <= 32 && tags[1] <= (tag_len - 2)) {
            char ssid[33] = {0};
            memcpy(ssid, tags + 2, tags[1]);
            ssid[tags[1]] = '\0';
            if (ssid[0]) {
                int slot = find_or_evict_ssid_slot(ssid);
                if (s_ssid_map[slot].ssid[0] == '\0') {
                    /* new entry */
                    size_t sl = strnlen(ssid, 32);
                    memcpy((void *)s_ssid_map[slot].ssid, ssid, sl);
                    ((char *)s_ssid_map[slot].ssid)[sl] = '\0';
                    memcpy((void *)s_ssid_map[slot].bssid, bssid, 6);
                    s_ssid_map[slot].last_seen_ms = millis();
                } else if (memcmp((const void *)s_ssid_map[slot].bssid, bssid, 6) != 0) {
                    /* DIFFERENT BSSID for same SSID → potential evil twin */
                    char detail[40];
                    snprintf(detail, sizeof(detail), "2 BSSIDs share SSID");
                    enqueue_alert(DM_CLS_EVIL_TWIN, bssid, ssid,
                                  pkt->rx_ctrl.rssi, s_current_ch, detail);
                    /* update the slot to track this latest sighting */
                    memcpy((void *)s_ssid_map[slot].bssid, bssid, 6);
                    s_ssid_map[slot].last_seen_ms = millis();
                } else {
                    s_ssid_map[slot].last_seen_ms = millis();
                }
            }
        }
    }
    portEXIT_CRITICAL_ISR(&s_mux);
}

/* ---- main-loop helpers ---- */

static bool open_log(void)
{
    uint32_t ts = millis() / 1000;
    snprintf(s_log_path, sizeof(s_log_path),
             "/poseidon/defmon-%lu.jsonl", (unsigned long)ts);
    SD.mkdir("/poseidon");
    s_log = SD.open(s_log_path, FILE_WRITE);
    return s_log ? true : false;
}

static void log_alert_row(const dm_alert_t &a)
{
    if (!s_log) return;
    gps_fix_t g;
    bool have_gps = gps_snapshot(&g);
    s_log.printf("{\"ts\":%lu,\"class\":\"%s\",\"bssid\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
                 "\"ssid\":\"%s\",\"ch\":%u,\"rssi\":%d,\"detail\":\"%s\","
                 "\"lat\":%.6f,\"lon\":%.6f}\n",
                 (unsigned long)a.ts_ms,
                 dm_class_name(a.cls),
                 a.bssid[0], a.bssid[1], a.bssid[2], a.bssid[3], a.bssid[4], a.bssid[5],
                 a.ssid,
                 a.channel, a.rssi, a.detail,
                 have_gps ? g.lat_deg : 0.0,
                 have_gps ? g.lon_deg : 0.0);
    s_log.flush();
}

static dm_alert_t s_last_painted = {};
static bool       s_have_painted_alert = false;

static void drain_alerts(void)
{
    while (s_alert_q_head != s_alert_q_tail) {
        portENTER_CRITICAL(&s_mux);
        uint8_t tail = s_alert_q_tail;
        dm_alert_t a;
        memcpy(&a, (const void *)&s_alert_q[tail], sizeof(a));
        s_alert_q_tail = (tail + 1) % DM_Q_N;
        portEXIT_CRITICAL(&s_mux);

        /* Cooldown — same class within DM_ALERT_COOLDOWN_MS = silenced
         * audio cue but still logged. */
        uint32_t now = millis();
        bool quiet = (now - s_last_alert_ms[a.cls] < DM_ALERT_COOLDOWN_MS);
        s_last_alert_ms[a.cls] = now;

        log_alert_row(a);
        s_last_painted       = a;
        s_have_painted_alert = true;
        if (!quiet) sfx_alert();
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

/* Beacon-spam window decay: every 1 s, sample s_new_bssids_now, alert
 * if >threshold, then reset. Same for deauth count display rate. */
static uint32_t s_window_start_ms = 0;
static uint32_t s_new_bssids_last_window = 0;
static uint32_t s_deauth_last_window     = 0;

static void window_tick(void)
{
    uint32_t now = millis();
    if (now - s_window_start_ms < 1000) return;
    s_window_start_ms = now;

    portENTER_CRITICAL(&s_mux);
    s_new_bssids_last_window = s_new_bssids_now;
    s_deauth_last_window     = s_deauth_now;
    s_new_bssids_now = 0;
    s_deauth_now     = 0;
    portEXIT_CRITICAL(&s_mux);

    if (s_new_bssids_last_window >= DM_BEACON_SPAM_THRESHOLD) {
        char detail[40];
        snprintf(detail, sizeof(detail), "%lu new BSSIDs/s",
                 (unsigned long)s_new_bssids_last_window);
        uint8_t zero[6] = {0};
        portENTER_CRITICAL(&s_mux);
        enqueue_alert(DM_CLS_BEACON_SPAM, zero, nullptr, 0, s_current_ch, detail);
        portEXIT_CRITICAL(&s_mux);
    }
}

static void enter_wifi_phase(void)
{
    s_phase = DM_PHASE_WIFI;
    s_phase_start_ms = millis();
    /* Tear down NimBLE so it releases the radio. */
    if (NimBLEDevice::getScan() && NimBLEDevice::getScan()->isScanning()) {
        NimBLEDevice::getScan()->stop();
    }
    if (NimBLEDevice::isInitialized()) NimBLEDevice::deinit(true);
    delay(80);
    radio_switch(RADIO_WIFI);
    wifi_lean_sta_init();   /* raw-safe STA init; Arduino WiFi.mode() asserts on a
                             * driver a prior feature already raw-inited (netif exists). */
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(promisc_cb);
    esp_wifi_set_channel(s_current_ch, WIFI_SECOND_CHAN_NONE);
}

static void enter_ble_phase(void)
{
    s_phase = DM_PHASE_BLE;
    s_phase_start_ms = millis();
    esp_wifi_set_promiscuous(false);
    delay(60);
    radio_switch(RADIO_BLE);
    if (!NimBLEDevice::isInitialized()) {
        NimBLEDevice::init("");
        NimBLEDevice::setPower(ESP_PWR_LVL_P9);
    }
    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->setScanCallbacks(&s_ble_cb, false);
    /* POS-AUDIT-011: process adverts via the callback only — store none.
     * Without this NimBLE accumulates every advertisement (duplicates too,
     * since the filter is off) in an internal results vector that grows
     * unbounded and reboots the device after a few hundred (~550). */
    scan->setMaxResults(0);
    scan->setActiveScan(false);
    scan->setInterval(97);
    scan->setWindow(67);
    scan->setDuplicateFilter(false);
    scan->start(DM_PHASE_BLE_MS / 1000, false, false);
}

void feat_defensive_monitor(void)
{
    if (!sd_mount() && !sd_remount()) {
        ui_toast("SD mount failed", T_BAD, 1500);
        return;
    }

    if (!open_log()) {
        ui_toast("cant open log", T_BAD, 1500);
        return;
    }

    /* Reset state */
    s_total = 0;
    s_deauth_now = s_beacons_total = s_new_bssids_now = 0;
    s_new_bssids_last_window = s_deauth_last_window = 0;
    s_window_start_ms = millis();
    s_ble_total = s_ble_new_now = s_ble_new_last_window = 0;
    s_ble_window_start_ms = millis();
    s_current_ch = 1;
    s_have_painted_alert = false;
    memset((void *)s_alert_count,   0, sizeof(s_alert_count));
    memset((void *)s_last_alert_ms, 0, sizeof(s_last_alert_ms));
    memset((void *)s_deauth_srcs,   0, sizeof(s_deauth_srcs));
    memset((void *)s_ssid_map,      0, sizeof(s_ssid_map));
    memset((void *)s_bssid_seen,    0, sizeof(s_bssid_seen));
    memset((void *)s_karma,         0, sizeof(s_karma));
    memset((void *)s_ble_names,     0, sizeof(s_ble_names));
    memset((void *)s_ble_addrs,     0, sizeof(s_ble_addrs));
    s_alert_q_head = s_alert_q_tail = 0;

    s_running = true;
    enter_wifi_phase();
    xTaskCreate(hop_task, "dm_hop", 3072, nullptr, 4, nullptr);

    auto &d = M5Cardputer.Display;
    ui_clear_body();
    ui_draw_footer("ESC=stop  WiFi+BLE time-sliced");

    uint32_t last_redraw = 0;
    bool dirty = true;
    while (true) {
        /* Phase switch logic */
        uint32_t now = millis();
        uint32_t phase_dur = (s_phase == DM_PHASE_WIFI) ? DM_PHASE_WIFI_MS : DM_PHASE_BLE_MS;
        if (now - s_phase_start_ms > phase_dur) {
            if (s_phase == DM_PHASE_WIFI) {
                /* Only flip to BLE if NimBLE can actually re-init — see
                 * DM_BLE_MIN_BLOCK. A fragmented heap here is what was
                 * resetting the device on the WiFi->BLE flip. */
                if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) > DM_BLE_MIN_BLOCK) {
                    enter_ble_phase();
                } else {
                    Serial.println("[defmon] heap too fragmented for NimBLE; staying WiFi-only");
                    s_phase_start_ms = now;   /* skip BLE this cycle, keep WiFi */
                }
            } else {
                enter_wifi_phase();
            }
        }

        if (s_phase == DM_PHASE_WIFI) window_tick();
        else                          ble_window_tick();
        drain_alerts();

        now = millis();
        if (now - last_redraw > 250) {
            last_redraw = now;
            ui_draw_status(radio_name(),
                           s_phase == DM_PHASE_WIFI ? "def-mon WiFi" : "def-mon BLE");
            if (dirty) {
                ui_clear_body();
                d.setTextColor(T_BAD, T_BG);
                d.setCursor(4, BODY_Y + 2); d.print("DEFENSIVE MONITOR");
                d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_BAD);
                dirty = false;
            }
            d.setTextColor(T_DIM, T_BG);
            d.setCursor(4, BODY_Y + 18);
            d.printf("phase: %-4s wifi=%lu ble=%lu",
                     s_phase == DM_PHASE_WIFI ? "WIFI" : "BLE",
                     (unsigned long)s_total, (unsigned long)s_ble_total);

            /* Row 1: WiFi anomaly counters */
            d.setTextColor(T_BAD,    T_BG); d.setCursor(4,   BODY_Y + 30);
            d.printf("DAUTH %-3lu BCAST %-3lu",
                     (unsigned long)s_alert_count[DM_CLS_DEAUTH_FLOOD],
                     (unsigned long)s_alert_count[DM_CLS_BCAST_DEAUTH]);
            d.setTextColor(T_WARN,   T_BG); d.setCursor(132, BODY_Y + 30);
            d.printf("KRMA %-3lu",
                     (unsigned long)s_alert_count[DM_CLS_WIFI_KARMA]);

            /* Row 2: WiFi/BLE rate + twin */
            d.setTextColor(T_ACCENT, T_BG); d.setCursor(4,   BODY_Y + 42);
            d.printf("TWIN  %-3lu BSPAM %-3lu",
                     (unsigned long)s_alert_count[DM_CLS_EVIL_TWIN],
                     (unsigned long)s_alert_count[DM_CLS_BEACON_SPAM]);

            /* Row 3: BLE anomaly counters */
            d.setTextColor(T_ACCENT2, T_BG); d.setCursor(4,  BODY_Y + 54);
            d.printf("BSPF %-3lu BFLD %-3lu  new/s w%-2lu b%-2lu",
                     (unsigned long)s_alert_count[DM_CLS_BLE_SPOOF],
                     (unsigned long)s_alert_count[DM_CLS_BLE_FLOOD],
                     (unsigned long)s_new_bssids_last_window,
                     (unsigned long)s_ble_new_last_window);

            /* Latest alert. */
            d.setTextColor(T_DIM, T_BG);
            d.setCursor(4, BODY_Y + 68); d.print("latest:");
            if (s_have_painted_alert) {
                d.setTextColor(T_GOOD, T_BG);
                d.setCursor(4, BODY_Y + 80);
                d.printf("%-13s %-22s",
                         dm_class_name(s_last_painted.cls),
                         s_last_painted.detail);
                d.setTextColor(T_FG, T_BG);
                d.setCursor(4, BODY_Y + 92);
                d.printf("%02X:%02X:%02X:%02X:%02X:%02X %.14s",
                         s_last_painted.bssid[0], s_last_painted.bssid[1],
                         s_last_painted.bssid[2], s_last_painted.bssid[3],
                         s_last_painted.bssid[4], s_last_painted.bssid[5],
                         s_last_painted.ssid[0] ? s_last_painted.ssid : "");
            } else {
                d.setTextColor(T_DIM, T_BG);
                d.setCursor(4, BODY_Y + 80);
                d.print("(no anomalies yet)            ");
                d.setCursor(4, BODY_Y + 92);
                d.print("                                  ");
            }
        }
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) break;
    }

    s_running = false;
    /* Join the hop task before tearing down radios — its loop period
     * (300ms) outlives the old delay(150), so it could still hit
     * esp_wifi_set_channel while the next feature inits. Bounded wait. */
    uint32_t deadline = millis() + 800;
    while (s_hop_alive && millis() < deadline) delay(5);
    esp_wifi_set_promiscuous(false);
    if (NimBLEDevice::getScan() && NimBLEDevice::getScan()->isScanning()) {
        NimBLEDevice::getScan()->stop();
    }
    if (NimBLEDevice::isInitialized()) NimBLEDevice::deinit(true);
    if (s_log) s_log.close();
    delay(150);
}
