/*
 * ble_extras — tracker detector, sniffer (CSV log), iBeacon broadcaster.
 */
#include "app.h"
#include "../theme.h"
#include "ui.h"
#include "input.h"
#include "radio.h"
#include <NimBLEDevice.h>
#include <SD.h>
#include "../sd_helper.h"
#include "ble_dult.h"
#include <esp_random.h>

/* ========== Tracker detector ==========
 *
 * Detect, select, then act. The advertisement alone gives type, separated
 * state and battery for free; selecting a tracker opens the DULT target
 * screen where the non-owner sound trigger and the silent DULT info reads
 * live (see ble_dult.cpp). */

struct tracker_t {
    dult_target_t t;
    uint32_t first_seen;
    uint32_t last_seen;
};
#define TRACKER_MAX 16
#define TRACKER_ROWS ((BODY_H - 24) / 13 > 8 ? 8 : (BODY_H - 24) / 13)

static tracker_t s_trackers[TRACKER_MAX];
static volatile int s_tracker_count = 0;

/* NimBLE 2.x: callback base class renamed to NimBLEScanCallbacks and
 * onResult now takes a const pointer. Address bytes via getBase()->val. */
class tracker_cb : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice *d) override {
        dult_target_t nt;
        if (!dult_classify(d, &nt)) return;
        for (int i = 0; i < s_tracker_count; ++i) {
            if (memcmp(s_trackers[i].t.addr, nt.addr, 6) == 0) {
                s_trackers[i].last_seen = millis();
                /* Refresh the volatile fields: separation state can flip
                 * mid-session, which is exactly the transition we care
                 * about, and it must not be masked by first-seen data. */
                s_trackers[i].t.state   = nt.state;
                s_trackers[i].t.battery = nt.battery;
                if (nt.hint != DULT_PROTO_NONE) s_trackers[i].t.hint = nt.hint;
                if (nt.rssi > s_trackers[i].t.rssi) s_trackers[i].t.rssi = nt.rssi;
                return;
            }
        }
        int n = s_tracker_count;
        if (n >= TRACKER_MAX) return;
        s_trackers[n].t = nt;
        s_trackers[n].first_seen = millis();
        s_trackers[n].last_seen  = millis();
        s_tracker_count = n + 1;   /* publish LAST - slot fully filled above */
    }
};
static tracker_cb s_tracker_cb_obj;
static tracker_cb *s_tracker_cb = &s_tracker_cb_obj;

static void tracker_scan_start(void)
{
    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->setScanCallbacks(s_tracker_cb, true);
    scan->setMaxResults(0);   /* POS-AUDIT-011 */
    scan->setActiveScan(false);
    scan->setInterval(45);
    scan->setWindow(30);
    scan->start(0, false);  /* duration=0 (indefinite), is_continue=false */
}

void feat_ble_tracker(void)
{
    radio_switch(RADIO_BLE);
    s_tracker_count = 0;
    /* s_tracker_cb is static-allocated. */
    tracker_scan_start();

    ui_clear_body();
    ui_draw_footer("turn=pick  hold=actions  back=exit");

    size_t last_alert_count = 0;
    int last_count = -1;
    int cursor = 0, top = 0;
    bool force_redraw = false;
    uint32_t last = 0;
    while (true) {
        if (millis() - last > 400 || force_redraw) {
            last = millis();
            auto &d = M5Cardputer.Display;
            if (s_tracker_count != last_count || force_redraw) {
                ui_clear_body();
                d.setTextColor(T_ACCENT, T_BG);
                d.setCursor(4, BODY_Y + 2);
                d.printf("TRACKERS  %d", s_tracker_count);
                d.drawFastHLine(4, BODY_Y + 12, 100, T_ACCENT);
                if (s_tracker_count == 0) {
                    d.setTextColor(T_DIM, T_BG);
                    d.setCursor(4, BODY_Y + 24);
                    d.print("scanning for Find My / DULT / Tile / SmartTag");
                }
                last_count = s_tracker_count;
                force_redraw = false;
            }

            if (s_tracker_count > 0) {
                if (cursor >= s_tracker_count) cursor = s_tracker_count - 1;
                if (cursor < top) top = cursor;
                if (cursor >= top + TRACKER_ROWS) top = cursor - TRACKER_ROWS + 1;

                /* Distance estimate from RSSI: empirical free-space
                 *   d ~ 10 ^ ((tx_power - rssi) / (10 * N))
                 * with tx_power ~ -59 dBm @ 1m and path-loss N=2.
                 * Render as a proximity ring (CLOSE / NEAR / FAR). */
                for (int r = 0; r < TRACKER_ROWS; ++r) {
                    int i = top + r;
                    int y = BODY_Y + 18 + r * 13;
                    if (i >= s_tracker_count) {
                        d.fillRect(0, y, SCR_W, 12, T_BG);
                        continue;
                    }
                    const dult_target_t &t = s_trackers[i].t;
                    const bool sel = (i == cursor);
                    uint16_t bg = sel ? T_SEL_BG : T_BG;
                    d.fillRect(0, y - 1, SCR_W, 13, bg);

                    const char *prox;
                    uint16_t prox_col;
                    if (t.rssi > -55)      { prox = "CLOSE"; prox_col = T_BAD; }
                    else if (t.rssi > -72) { prox = "NEAR "; prox_col = T_WARN; }
                    else                   { prox = "FAR  "; prox_col = T_DIM; }

                    d.setTextColor(prox_col, bg);
                    d.setCursor(4, y); d.print(prox);
                    d.setTextColor(sel ? T_ACCENT : T_BAD, bg);
                    d.setCursor(38, y); d.printf("%-8s", dult_kind_name(t.kind));
                    /* Separated state is the single most useful thing on
                     * this screen: sound only works when separated. */
                    uint16_t st_col = (t.state == DULT_STATE_SEPARATED) ? T_GOOD
                                    : (t.state == DULT_STATE_NEAR_OWNER) ? T_WARN : T_DIM;
                    d.setTextColor(st_col, bg);
                    d.setCursor(94, y);
                    d.print(t.state == DULT_STATE_SEPARATED  ? "SEP "
                          : t.state == DULT_STATE_NEAR_OWNER ? "ownr"
                                                             : "  ? ");
                    d.setTextColor(T_DIM, bg);
                    d.setCursor(126, y);
                    d.printf("%ddB %02X:%02X %lus", t.rssi, t.addr[1], t.addr[0],
                             (unsigned long)((millis() - s_trackers[i].first_seen) / 1000));

                    /* Signal bar (small). */
                    int pct = (t.rssi + 100) * 100 / 70;
                    if (pct < 0)   pct = 0;
                    if (pct > 100) pct = 100;
                    int bx = SCR_W - 42;
                    d.drawRect(bx, y + 1, 36, 6, T_DIM);
                    d.fillRect(bx + 1, y + 2, 34, 4, bg);
                    d.fillRect(bx + 1, y + 2, 34 * pct / 100, 4, prox_col);
                }
                /* Alert on new detection: flash screen border + chirp. */
                if ((size_t)s_tracker_count > last_alert_count) {
                    M5Cardputer.Speaker.tone(3200, 80);
                    delay(90);
                    M5Cardputer.Speaker.tone(2400, 80);
                    /* Red border flash. */
                    for (int f = 0; f < 3; ++f) {
                        d.drawRect(0, 0, SCR_W, SCR_H, T_BAD);
                        d.drawRect(1, 1, SCR_W - 2, SCR_H - 2, T_BAD);
                        delay(60);
                        d.drawRect(0, 0, SCR_W, SCR_H, T_BG);
                        d.drawRect(1, 1, SCR_W - 2, SCR_H - 2, T_BG);
                        delay(60);
                    }
                    last_alert_count = s_tracker_count;
                }
            }
            ui_draw_status(radio_name(), "tracker");
        }

        /* Keep the empty wait alive: animated radar + "trackers..." strip. */
        if (s_tracker_count == 0) ui_scanning_indicator("trackers", s_tracker_count);

        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) break;
        if (k == PK_UP   || k == ';') { if (cursor > 0) cursor--; force_redraw = true; }
        if (k == PK_DOWN || k == '.') { if (cursor + 1 < s_tracker_count) cursor++; force_redraw = true; }
        if ((k == PK_ACTIONS || k == PK_ENTER) && s_tracker_count > 0) {
            /* NimBLE cannot open a connection while a discovery is
             * running, and the sound path needs one. Stop, act, resume. */
            dult_target_t sel = s_trackers[cursor].t;
            NimBLEDevice::getScan()->stop();
            dult_target_screen(&sel);
            tracker_scan_start();
            ui_screen_enter();
            ui_draw_footer("turn=pick  hold=actions  back=exit");
            last_count = -1;
            force_redraw = true;
        }
    }
    NimBLEDevice::getScan()->stop();
}

/* ========== BLE sniffer → CSV ========== */

static volatile uint32_t s_sniff_count = 0;
static volatile uint32_t s_sniff_dropped = 0;
static File s_sniff_file;

/*
 * The scan callback runs on the NIMBLE HOST TASK, not our UI task. Doing SD
 * I/O there was wrong in three separate ways:
 *
 *   1. FATFS + SPI writes are slow and blocking. Stalling the BLE host task
 *      inside a callback makes the controller drop advertisements -- the
 *      sniffer was losing exactly the packets it existed to record.
 *   2. On the T-Embed the SD card shares one SPI bus with the DISPLAY. Writing
 *      from the BLE task while the UI task drives LovyanGFX on the same bus is
 *      a cross-task collision on the peripheral (the bug class that already
 *      blanked this panel once).
 *   3. feat_ble_sniff() closed s_sniff_file from the UI task while a callback
 *      could be mid-printf on it -- a use-after-close race that only shows up
 *      under heavy advertisement traffic, i.e. exactly when you are using it.
 *
 * So the callback is now a pure PRODUCER: it copies the advertisement into a
 * fixed ring and returns immediately. The UI loop drains the ring and owns all
 * file access. Single producer, single consumer, so no lock is needed as long
 * as the record is fully written BEFORE the head index is published.
 */
struct sniff_rec_t {
    uint32_t ms;
    uint8_t  addr[6];
    uint8_t  addr_type;
    int8_t   rssi;
    uint8_t  payload_len;
    uint8_t  payload[31];
    char     name[24];
};

#define SNIFF_RING 48
static sniff_rec_t       s_ring[SNIFF_RING];
static volatile uint16_t s_ring_head = 0;   /* written by NimBLE task */
static volatile uint16_t s_ring_tail = 0;   /* written by UI task     */
static volatile bool     s_sniff_active = false;

class sniff_cb : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice *d) override {
        if (!s_sniff_active) return;

        const uint16_t head = s_ring_head;
        const uint16_t next = (uint16_t)((head + 1) % SNIFF_RING);
        if (next == s_ring_tail) {         /* consumer behind -> drop, don't block */
            s_sniff_dropped++;
            return;
        }

        sniff_rec_t &r = s_ring[head];
        r.ms = millis();
        NimBLEAddress _addr = d->getAddress();   /* getAddress() returns by value */
        memcpy(r.addr, _addr.getBase()->val, 6);
        r.addr_type = d->getAddressType();
        r.rssi      = (int8_t)d->getRSSI();

        const std::vector<uint8_t> &payload = d->getPayload();
        size_t n = payload.size();
        if (n > sizeof(r.payload)) n = sizeof(r.payload);
        memcpy(r.payload, payload.data(), n);
        r.payload_len = (uint8_t)n;

        r.name[0] = '\0';
        if (d->haveName()) {
            const std::string nm = d->getName();
            size_t ln = nm.size();
            if (ln > sizeof(r.name) - 1) ln = sizeof(r.name) - 1;
            memcpy(r.name, nm.data(), ln);
            r.name[ln] = '\0';
        }

        /* Publish LAST: the consumer must never see a half-written record. */
        s_ring_head = next;
        s_sniff_count++;
    }
};
static sniff_cb s_sniff_cb_obj;
static sniff_cb *s_sniff_cb = &s_sniff_cb_obj;

/* Drain whatever the callback has queued. Runs on the UI task, which is the
 * only task that ever touches s_sniff_file. Returns records written. */
static int sniff_drain(void)
{
    int wrote = 0;
    while (s_ring_tail != s_ring_head) {
        const sniff_rec_t &r = s_ring[s_ring_tail];
        if (s_sniff_file) {
            s_sniff_file.printf("%lu,%02X:%02X:%02X:%02X:%02X:%02X,%d,%u,",
                                (unsigned long)r.ms,
                                r.addr[5], r.addr[4], r.addr[3],
                                r.addr[2], r.addr[1], r.addr[0],
                                (int)r.rssi, (unsigned)r.addr_type);
            if (r.name[0]) s_sniff_file.printf("\"%s\"", r.name);
            s_sniff_file.print(",");
            for (uint8_t i = 0; i < r.payload_len; ++i)
                s_sniff_file.printf("%02X", r.payload[i]);
            s_sniff_file.print("\n");
            ++wrote;
        }
        s_ring_tail = (uint16_t)((s_ring_tail + 1) % SNIFF_RING);
    }
    return wrote;
}

void feat_ble_sniff(void)
{
    radio_switch(RADIO_BLE);
    if (!sd_mount()) { ui_toast("SD needed", T_BAD, 1500); return; }
    SD.mkdir("/poseidon");
    char path[64];
    snprintf(path, sizeof(path), "/poseidon/blesniff-%lu.csv", (unsigned long)(millis() / 1000));
    s_sniff_file = SD.open(path, FILE_WRITE);
    if (!s_sniff_file) { ui_toast("cant open file", T_BAD, 1500); return; }
    s_sniff_file.println("ms,mac,rssi,addr_type,name,adv_hex");
    s_sniff_count = 0;
    s_sniff_dropped = 0;
    s_ring_head = s_ring_tail = 0;
    s_sniff_active = true;      /* arm the producer only once the file is open */

    /* s_sniff_cb is static-allocated. */
    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->setScanCallbacks(s_sniff_cb, true);
    scan->setMaxResults(0);   /* POS-AUDIT-011 */
    scan->setActiveScan(false);
    scan->start(0, false);

    ui_clear_body();
    ui_draw_footer("`=stop");
    {
        auto &d = M5Cardputer.Display;
        d.setTextColor(T_ACCENT, T_BG);
        d.setCursor(4, BODY_Y + 2); d.print("BLE SNIFFER");
        d.drawFastHLine(4, BODY_Y + 12, 90, T_ACCENT);
        d.setTextColor(T_DIM, T_BG);
        d.setCursor(4, BODY_Y + 40); d.printf("%s", path);
    }
    uint32_t last = 0;
    uint32_t flushed = 0;
    while (true) {
        /* Drain every iteration, not just on the 300 ms UI tick: the ring is
         * only 48 deep and a busy RF environment fills it fast. Dropped
         * records are counted and surfaced rather than hidden, so the operator
         * can tell "quiet air" from "we could not keep up". */
        flushed += (uint32_t)sniff_drain();

        if (millis() - last > 300) {
            last = millis();
            if (s_sniff_dropped)
                ui_text_w(4, BODY_Y + 22, SCR_W - 8, T_WARN, "packets: %lu  dropped: %lu",
                          (unsigned long)s_sniff_count, (unsigned long)s_sniff_dropped);
            else
                ui_text_w(4, BODY_Y + 22, SCR_W - 8, T_FG, "packets: %lu",
                          (unsigned long)s_sniff_count);
            if (s_sniff_file) s_sniff_file.flush();
            ui_draw_status(radio_name(), "sniff");
        }
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) break;
    }

    /* Shutdown ORDER matters. Disarm the producer first so no callback can
     * touch the ring, then stop the scan, then drain what is still queued, and
     * only then close the file. Closing while the callback could still be
     * running was the original use-after-close race. */
    s_sniff_active = false;
    scan->stop();
    sniff_drain();
    if (s_sniff_file) { s_sniff_file.flush(); s_sniff_file.close(); }
    (void)flushed;
}

/* ========== iBeacon broadcaster ========== */

void feat_ble_beacon(void)
{
    radio_switch(RADIO_BLE);
    /* radio_switch(RADIO_BLE) already calls NimBLEDevice::init("POSEIDON").
     * A redundant second init here is a no-op on recent NimBLE builds but
     * asserts on some — drop it. */
    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();

    uint8_t payload[30] = {
        0x02, 0x01, 0x06,
        0x1A, 0xFF, 0x4C, 0x00, 0x02, 0x15,
        /* UUID */
        0xE2, 0xC5, 0x6D, 0xB5, 0xDF, 0xFB, 0x48, 0xD2,
        0xB0, 0x60, 0xD0, 0xF5, 0xA7, 0x10, 0x96, 0xE0,
        /* major */ 0x00, 0x01,
        /* minor */ 0x00, 0x01,
        /* power */ 0xC5
    };
    NimBLEAdvertisementData data;
    /* NimBLE 2.x: addData now takes uint8_t*+size_t. */
    data.addData(payload, sizeof(payload));
    adv->setAdvertisementData(data);
    /* setAdvertisementType removed — use setConnectableMode. BLE_GAP_CONN_MODE_NON
     * = non-connectable advertising (pure beacon). */
    adv->setConnectableMode(BLE_GAP_CONN_MODE_NON);
    adv->start();

    ui_clear_body();
    auto &d = M5Cardputer.Display;
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2); d.print("iBEACON");
    d.drawFastHLine(4, BODY_Y + 12, 60, T_ACCENT);
    d.setTextColor(T_FG, T_BG);
    d.setCursor(4, BODY_Y + 22); d.print("broadcasting iBeacon 1/1");
    d.setTextColor(T_DIM, T_BG);
    d.setCursor(4, BODY_Y + 40); d.print("UUID: E2C56DB5-...96E0");
    ui_draw_footer("`=stop");
    ui_draw_status(radio_name(), "beacon");

    while (true) {
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(50); continue; }
        if (k == PK_ESC) break;
    }
    adv->stop();
}
