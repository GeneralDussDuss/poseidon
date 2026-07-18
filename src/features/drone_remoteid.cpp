/*
 * drone_remoteid — passive ASTM F3411-22a Drone Remote ID detector.
 *
 * Most US drones over 250 g must broadcast Remote ID per FAA Part 89.
 * The standard mandates a Bluetooth 4 Legacy Advertisement on the
 * advertising channels (37/38/39) with Service Data UUID 0xFFFA
 * containing one of six ASTM message types:
 *
 *   0x0  Basic ID         UAS ID + UA type
 *   0x1  Location/Vector  lat/lon/alt + speed + heading
 *   0x2  Authentication
 *   0x3  Self-ID          operator note (e.g. flight purpose)
 *   0x4  System           operator location + classification
 *   0x5  Operator ID      operator identifier
 *
 * This scanner passively listens for those frames, decodes the
 * Basic ID + Location, and shows live target list with operator
 * location when available. SD log to /poseidon/drone-<ts>.jsonl
 * with timestamps + observed RSSI.
 *
 * Source: ASTM F3411-22a spec + GhostBLE decoder pattern (SmonSE).
 */
#include "app.h"
#include "../theme.h"
#include "ui.h"
#include "input.h"
#include "radio.h"
#include "gps.h"
#include "sfx.h"
#include "../sd_helper.h"
#include "../sigdb_bt.h"
#include <NimBLEDevice.h>
#include <SD.h>
#include <math.h>

#define ASTM_SVC_UUID16   0xFFFA

enum {
    ASTM_MSG_BASIC_ID    = 0x0,
    ASTM_MSG_LOC_VECTOR  = 0x1,
    ASTM_MSG_AUTH        = 0x2,
    ASTM_MSG_SELF_ID     = 0x3,
    ASTM_MSG_SYSTEM      = 0x4,
    ASTM_MSG_OPERATOR_ID = 0x5,
};

struct drone_t {
    char     uas_id[21];     /* CAA / ANSI/CTA-2063 */
    uint8_t  src[6];
    double   lat_deg;
    double   lon_deg;
    float    alt_m;
    float    speed_ms;
    float    track_deg;
    double   op_lat_deg;
    double   op_lon_deg;
    int8_t   rssi;
    uint32_t last_ms;
    uint8_t  ua_type;        /* aeroplane / helicopter / etc. */
    bool     have_loc;
    bool     have_op;
};

#define DRONE_N 8
static drone_t s_drones[DRONE_N];
static int     s_drone_count = 0;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static File    s_log;
static char    s_log_path[64] = {0};

/* Quick decoder helpers — ASTM uses little-endian + fixed scaling. */
static inline int32_t le_i32(const uint8_t *p)
{
    return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                     ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}
static inline int16_t le_i16(const uint8_t *p)
{
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* Latitude/longitude scaled by 1e7 (degrees). */
static inline double astm_latlon(int32_t v) { return (double)v / 1e7; }

static int find_or_add_drone(const uint8_t src[6])
{
    for (int i = 0; i < s_drone_count; i++)
        if (memcmp(s_drones[i].src, src, 6) == 0) return i;
    if (s_drone_count >= DRONE_N) {
        /* Evict oldest. */
        int oldest = 0;
        for (int i = 1; i < DRONE_N; i++)
            if (s_drones[i].last_ms < s_drones[oldest].last_ms) oldest = i;
        memset(&s_drones[oldest], 0, sizeof(drone_t));
        memcpy(s_drones[oldest].src, src, 6);
        return oldest;
    }
    int idx = s_drone_count++;
    memset(&s_drones[idx], 0, sizeof(drone_t));
    memcpy(s_drones[idx].src, src, 6);
    return idx;
}

static void log_event(const drone_t &d, uint8_t msg_type)
{
    if (!s_log) return;
    gps_fix_t g;
    bool have_obs = gps_snapshot(&g);
    s_log.printf("{\"ts\":%lu,\"msg_type\":%u,\"src\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
                 "\"uas_id\":\"%s\",\"rssi\":%d,"
                 "\"drone_lat\":%.6f,\"drone_lon\":%.6f,\"drone_alt\":%.1f,"
                 "\"op_lat\":%.6f,\"op_lon\":%.6f,"
                 "\"obs_lat\":%.6f,\"obs_lon\":%.6f}\n",
                 (unsigned long)d.last_ms, (unsigned)msg_type,
                 d.src[0], d.src[1], d.src[2], d.src[3], d.src[4], d.src[5],
                 d.uas_id,
                 (int)d.rssi,
                 d.lat_deg, d.lon_deg, (double)d.alt_m,
                 d.op_lat_deg, d.op_lon_deg,
                 have_obs ? g.lat_deg : 0.0,
                 have_obs ? g.lon_deg : 0.0);
    s_log.flush();
}

static void decode_astm(const uint8_t *data, size_t len, const uint8_t src[6], int8_t rssi)
{
    /* ASTM frame inside Service Data 0xFFFA: first byte is message-type
     * (high nibble) + protocol-version (low nibble). Then a 24-byte
     * type-specific payload. Frame may carry multiple messages back to
     * back (Message Pack — type 0xF wraps up to 9 messages). */
    if (len < 1) return;

    /* Multi-pack: type 0xF, version 0x2 -> count at offset 1, then 25-byte chunks. */
    if ((data[0] >> 4) == 0xF) {
        if (len < 2) return;
        int n = data[1];
        if (n > 9) n = 9;
        const uint8_t *m = data + 2;
        for (int i = 0; i < n; i++) {
            if ((size_t)(m + 25 - data) > len) break;
            decode_astm(m, 25, src, rssi);
            m += 25;
        }
        return;
    }

    uint8_t msg_type = data[0] >> 4;
    /* uint8_t proto_ver = data[0] & 0xF; */
    portENTER_CRITICAL(&s_mux);
    int idx = find_or_add_drone(src);
    drone_t &dr = s_drones[idx];
    dr.rssi    = rssi;
    dr.last_ms = millis();

    if (msg_type == ASTM_MSG_BASIC_ID && len >= 25) {
        dr.ua_type = data[1] & 0x0F;
        /* UAS ID at bytes 2..21 (20 bytes), ASCII / CAA-format. */
        memcpy(dr.uas_id, data + 2, 20);
        dr.uas_id[20] = '\0';
        /* Trim trailing spaces / nulls. */
        for (int i = 19; i >= 0; i--) {
            if (dr.uas_id[i] == ' ' || dr.uas_id[i] == 0) dr.uas_id[i] = '\0';
            else break;
        }
    }
    else if (msg_type == ASTM_MSG_LOC_VECTOR && len >= 25) {
        /* Per F3411-22a §A.2.1.2 Location/Vector layout:
         *   byte 1: status + height type + e/w direction + speed multiplier
         *   byte 2: track direction
         *   byte 3: speed
         *   byte 4: vertical speed (signed)
         *   bytes 5-8: latitude (int32, /1e7)
         *   bytes 9-12: longitude (int32, /1e7)
         *   bytes 13-14: pressure alt (uint16, scale)
         *   bytes 15-16: geodetic alt (uint16, scale)
         *   bytes 17-18: height (uint16, scale)
         *   ...
         */
        dr.track_deg = (float)data[2];
        dr.speed_ms  = (float)data[3] * 0.25f;
        dr.lat_deg   = astm_latlon(le_i32(data + 5));
        dr.lon_deg   = astm_latlon(le_i32(data + 9));
        /* Geodetic altitude: stored as (alt + 1000) * 2 (meters). */
        uint16_t alt_raw = (uint16_t)le_i16(data + 15);
        dr.alt_m = ((float)alt_raw * 0.5f) - 1000.0f;
        dr.have_loc = true;
    }
    else if (msg_type == ASTM_MSG_SYSTEM && len >= 25) {
        /* System message — operator (pilot) location at bytes 1..8 (after
         * flags byte). */
        dr.op_lat_deg = astm_latlon(le_i32(data + 2));
        dr.op_lon_deg = astm_latlon(le_i32(data + 6));
        dr.have_op = true;
    }
    portEXIT_CRITICAL(&s_mux);

    log_event(dr, msg_type);
}

class DroneScanCb : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice *dev) override {
        /* Find ASTM service data (UUID 0xFFFA). NimBLE doesn't have a
         * direct "service data by UUID16" accessor in 2.x so we walk
         * the raw payload. Service Data 16-bit type = 0x16. */
        const std::vector<uint8_t> &payload = dev->getPayload();
        const uint8_t *p = payload.data();
        size_t plen = payload.size();
        size_t i = 0;
        while (i + 1 < plen) {
            uint8_t l = p[i];
            if (l == 0 || i + 1 + l > plen) break;
            uint8_t t = p[i + 1];
            if (t == 0x16 && l >= 3) {
                uint16_t svc = (uint16_t)p[i + 2] | ((uint16_t)p[i + 3] << 8);
                if (svc == ASTM_SVC_UUID16 && l > 3) {
                    uint8_t addr[6];
                    memcpy(addr, dev->getAddress().getBase()->val, 6);
                    decode_astm(p + i + 4, l - 3, addr, dev->getRSSI());
                }
            }
            i += 1 + l;
        }
    }
};
static DroneScanCb s_cb;

void feat_drone_remoteid(void)
{
    if (!sd_mount() && !sd_remount()) {
        ui_toast("SD mount failed", T_BAD, 1200);
        return;
    }
    uint32_t ts = millis() / 1000;
    snprintf(s_log_path, sizeof(s_log_path),
             "/poseidon/drone-%lu.jsonl", (unsigned long)ts);
    SD.mkdir("/poseidon");
    s_log = SD.open(s_log_path, FILE_WRITE);
    if (!s_log) { ui_toast("cant open log", T_BAD, 1200); return; }

    radio_switch(RADIO_BLE);
    if (!NimBLEDevice::isInitialized()) {
        NimBLEDevice::init("");
        NimBLEDevice::setPower(ESP_PWR_LVL_P9);
    }
    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->setScanCallbacks(&s_cb, false);
    /* POS-AUDIT-011: callback-only, store nothing. This is a forever scan
     * (duration 0) with duplicates on — without this NimBLE accumulates every
     * advert unbounded and reboots the device after a few hundred. */
    scan->setMaxResults(0);
    scan->setActiveScan(false);
    scan->setInterval(97);
    scan->setWindow(67);
    scan->setDuplicateFilter(false);

    /* Reset state. */
    s_drone_count = 0;
    memset(s_drones, 0, sizeof(s_drones));

    scan->start(0, false, false);   /* run until stopped */

    auto &d = M5Cardputer.Display;
    ui_clear_body();
    ui_draw_footer("ESC=stop  passive Remote ID listener");

    uint32_t last_redraw = 0;
    bool dirty = true;
    int last_count = -1;
    while (true) {
        uint32_t now = millis();
        if (now - last_redraw > 400) {
            last_redraw = now;
            ui_draw_status(radio_name(), "drone RID");

            if (dirty || s_drone_count != last_count) {
                ui_clear_body();
                d.setTextColor(T_ACCENT, T_BG);
                d.setCursor(4, BODY_Y + 2); d.print("DRONE REMOTE ID");
                d.setTextColor(T_DIM, T_BG);
                d.setCursor(SCR_W - 60, BODY_Y + 2);
                d.printf("%d targets", s_drone_count);
                d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT);
                dirty = false;
                last_count = s_drone_count;
            }

            if (s_drone_count == 0) {
                ui_scanning_indicator("waiting for drones", s_drone_count);
            } else {
                int rows = 5;
                for (int i = 0; i < rows && i < s_drone_count; i++) {
                    const drone_t &dr = s_drones[i];
                    int y = BODY_Y + 18 + i * 18;
                    d.setTextColor(T_FG, T_BG);
                    d.setCursor(4, y);
                    d.printf("%-20s %4d", dr.uas_id[0] ? dr.uas_id : "<no-id>", (int)dr.rssi);
                    d.setTextColor(T_DIM, T_BG);
                    d.setCursor(4, y + 8);
                    if (dr.have_loc) {
                        d.printf("%.4f,%.4f alt%.0fm",
                                 dr.lat_deg, dr.lon_deg, dr.alt_m);
                    } else {
                        d.print("(location pending)");
                    }
                }
            }
        }
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(30); continue; }
        if (k == PK_ESC) break;
    }

    scan->stop();
    if (s_log) s_log.close();
    delay(100);
}
