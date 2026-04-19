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
#include <esp_random.h>

/* ========== Tracker detector ========== */

struct tracker_t {
    uint8_t  addr[6];
    char     type[12];
    int8_t   rssi;
    uint32_t first_seen;
    uint32_t last_seen;
};
#define TRACKER_MAX 16

static tracker_t s_trackers[TRACKER_MAX];
static volatile int s_tracker_count = 0;

static bool is_tracker(NimBLEAdvertisedDevice *d, char *type_out)
{
    if (d->haveManufacturerData()) {
        std::string md = d->getManufacturerData();
        if (md.size() >= 3) {
            uint16_t cid = (uint8_t)md[0] | ((uint8_t)md[1] << 8);
            if (cid == 0x004C && (uint8_t)md[2] == 0x12) { strcpy(type_out, "AirTag");   return true; }
            if (cid == 0x0075)                           { strcpy(type_out, "SmartTag"); return true; }
        }
    }
    if (d->haveServiceUUID()) {
        for (int i = 0; i < d->getServiceUUIDCount(); ++i) {
            NimBLEUUID u = d->getServiceUUID(i);
            if (u.equals(NimBLEUUID((uint16_t)0xFEED)) ||
                u.equals(NimBLEUUID((uint16_t)0xFD84))) {
                strcpy(type_out, "Tile");
                return true;
            }
        }
    }
    return false;
}

class tracker_cb : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice *d) override {
        char type[12] = {0};
        if (!is_tracker(d, type)) return;
        NimBLEAddress _addr = d->getAddress(); const uint8_t *a = _addr.getNative();
        for (int i = 0; i < s_tracker_count; ++i) {
            if (memcmp(s_trackers[i].addr, a, 6) == 0) {
                s_trackers[i].last_seen = millis();
                if (d->getRSSI() > s_trackers[i].rssi) s_trackers[i].rssi = d->getRSSI();
                return;
            }
        }
        if (s_tracker_count >= TRACKER_MAX) return;
        tracker_t &t = s_trackers[s_tracker_count++];
        memcpy(t.addr, a, 6);
        strncpy(t.type, type, sizeof(t.type) - 1);
        t.rssi = d->getRSSI();
        t.first_seen = millis();
        t.last_seen = millis();
    }
};
static tracker_cb s_tracker_cb_obj;
static tracker_cb *s_tracker_cb = &s_tracker_cb_obj;

void feat_ble_tracker(void)
{
    radio_switch(RADIO_BLE);
    s_tracker_count = 0;
    /* s_tracker_cb is static-allocated. */

    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->setAdvertisedDeviceCallbacks(s_tracker_cb, true);
    scan->setActiveScan(false);
    scan->setInterval(45);
    scan->setWindow(30);
    scan->start(0, nullptr, false);

    ui_clear_body();
    ui_draw_footer("`=back");

    size_t last_alert_count = 0;
    uint32_t last = 0;
    while (true) {
        if (millis() - last > 400) {
            last = millis();
            auto &d = M5Cardputer.Display;
            ui_clear_body();
            d.setTextColor(T_ACCENT, T_BG);
            d.setCursor(4, BODY_Y + 2);
            d.printf("TRACKERS  %d", s_tracker_count);
            d.drawFastHLine(4, BODY_Y + 12, 100, T_ACCENT);

            if (s_tracker_count == 0) {
                d.setTextColor(T_DIM, T_BG);
                d.setCursor(4, BODY_Y + 24);
                d.print("scanning for AirTag/SmartTag/Tile");
            } else {
                /* Distance estimate from RSSI: empirical free-space
                 *   d ≈ 10 ^ ((tx_power - rssi) / (10 * N))
                 * with tx_power ≈ -59 dBm @ 1m and path-loss N=2.
                 * Render as a proximity ring (CLOSE / NEAR / FAR). */
                for (int i = 0; i < s_tracker_count && i < 6; ++i) {
                    const tracker_t &t = s_trackers[i];
                    int y = BODY_Y + 18 + i * 13;

                    const char *prox;
                    uint16_t prox_col;
                    if (t.rssi > -55)      { prox = "CLOSE"; prox_col = T_BAD; }
                    else if (t.rssi > -72) { prox = "NEAR ";  prox_col = T_WARN; }
                    else                   { prox = "FAR  ";  prox_col = T_DIM; }

                    d.setTextColor(prox_col, T_BG);
                    d.setCursor(4, y);
                    d.printf("%-5s", prox);
                    d.setTextColor(T_BAD, T_BG);
                    d.setCursor(36, y);
                    d.printf("%-9s %ddB", t.type, t.rssi);
                    d.setTextColor(T_DIM, T_BG);
                    d.setCursor(140, y);
                    d.printf("%02X:%02X %lus",
                             t.addr[4], t.addr[5],
                             (unsigned long)((millis() - t.first_seen) / 1000));

                    /* Signal bar (small). */
                    int pct = (t.rssi + 100) * 100 / 70;
                    if (pct < 0) pct = 0; if (pct > 100) pct = 100;
                    d.drawRect(200, y + 1, 36, 6, T_DIM);
                    d.fillRect(201, y + 2, 34 * pct / 100, 4, prox_col);
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

        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) break;
    }
    scan->stop();
}

/* ========== BLE sniffer → CSV ========== */

static volatile uint32_t s_sniff_count = 0;
static File s_sniff_file;

class sniff_cb : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice *d) override {
        if (!s_sniff_file) return;
        s_sniff_count++;
        NimBLEAddress _addr = d->getAddress(); const uint8_t *a = _addr.getNative();
        s_sniff_file.printf("%lu,%02X:%02X:%02X:%02X:%02X:%02X,%d,%u,",
                 (unsigned long)millis(),
                 a[5], a[4], a[3], a[2], a[1], a[0],
                 d->getRSSI(), d->getAddressType());
        if (d->haveName()) s_sniff_file.printf("\"%s\"", d->getName().c_str());
        s_sniff_file.print(",");
        std::string payload = d->getPayload() ? "" : "";
        uint8_t *p = d->getPayload();
        size_t n = d->getPayloadLength();
        for (size_t i = 0; i < n; ++i) s_sniff_file.printf("%02X", p[i]);
        s_sniff_file.print("\n");
        if ((s_sniff_count & 31) == 0) s_sniff_file.flush();
    }
};
static sniff_cb s_sniff_cb_obj;
static sniff_cb *s_sniff_cb = &s_sniff_cb_obj;

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

    /* s_sniff_cb is static-allocated. */
    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->setAdvertisedDeviceCallbacks(s_sniff_cb, true);
    scan->setActiveScan(false);
    scan->start(0, nullptr, false);

    ui_clear_body();
    ui_draw_footer("`=stop");
    uint32_t last = 0;
    while (true) {
        if (millis() - last > 300) {
            last = millis();
            auto &d = M5Cardputer.Display;
            ui_clear_body();
            d.setTextColor(T_ACCENT, T_BG);
            d.setCursor(4, BODY_Y + 2); d.print("BLE SNIFFER");
            d.drawFastHLine(4, BODY_Y + 12, 90, T_ACCENT);
            d.setTextColor(T_FG, T_BG);
            d.setCursor(4, BODY_Y + 22); d.printf("packets: %lu", (unsigned long)s_sniff_count);
            d.setTextColor(T_DIM, T_BG);
            d.setCursor(4, BODY_Y + 40); d.printf("%s", path);
            ui_draw_status(radio_name(), "sniff");
        }
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) break;
    }
    scan->stop();
    if (s_sniff_file) { s_sniff_file.flush(); s_sniff_file.close(); }
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
    data.addData(std::string((const char *)payload, sizeof(payload)));
    adv->setAdvertisementData(data);
    adv->setAdvertisementType(BLE_GAP_CONN_MODE_NON);
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
