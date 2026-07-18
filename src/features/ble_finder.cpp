/*
 * ble_finder — Geiger-counter physical tracker locator.
 *
 * Lists recently-seen trackers. Pick one, then the whole screen turns
 * into a big live RSSI meter for THAT MAC. Beep rate and display
 * urgency scale with signal strength — walk around, get warmer/colder
 * until you find where it's stashed.
 *
 * Use case: phone/security tool flagged a rogue AirTag traveling with
 * you. Pick it from the list, sweep the car/bag until you find it.
 */
#include "app.h"
#include "../theme.h"
#include "ui.h"
#include "input.h"
#include "radio.h"
#include <NimBLEDevice.h>

struct found_t {
    uint8_t  addr[6];
    char     type[12];
    int8_t   rssi;
    int8_t   best_rssi;
    uint32_t last_seen;
};

#define FIND_MAX 16
static found_t s_found[FIND_MAX];
static volatile int s_found_n = 0;

/* During the locate phase: which MAC are we tracking, and the most
 * recent live RSSI. -100 = no signal. */
static uint8_t  s_lock[6];
static volatile int8_t  s_lock_rssi = -100;
static volatile uint32_t s_lock_last = 0;
static volatile bool     s_locating = false;
/* Optional label for the hunt screen header — e.g. the device name when
 * launched from BLE Scan. Empty = show "HUNT" + MAC only. */
static char     s_lock_label[24] = {0};

static const char *tracker_kind(const NimBLEAdvertisedDevice *d)
{
    if (d->haveManufacturerData()) {
        std::string md = d->getManufacturerData();
        if (md.size() >= 3) {
            uint16_t cid = (uint8_t)md[0] | ((uint8_t)md[1] << 8);
            if (cid == 0x004C && (uint8_t)md[2] == 0x12) return "AirTag";
            if (cid == 0x0075) return "SmartTag";
        }
    }
    if (d->haveServiceUUID()) {
        for (int i = 0; i < d->getServiceUUIDCount(); ++i) {
            NimBLEUUID u = d->getServiceUUID(i);
            if (u.equals(NimBLEUUID((uint16_t)0xFEED)) ||
                u.equals(NimBLEUUID((uint16_t)0xFD84))) return "Tile";
        }
    }
    return nullptr;
}

class finder_cb : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice *d) override {
        NimBLEAddress _addr = d->getAddress(); const uint8_t *a = _addr.getBase()->val;
        if (s_locating) {
            if (memcmp(a, s_lock, 6) == 0) {
                s_lock_rssi = d->getRSSI();
                s_lock_last = millis();
            }
            return;
        }
        const char *kind = tracker_kind(d);
        if (!kind) return;

        for (int i = 0; i < s_found_n; ++i) {
            if (memcmp(s_found[i].addr, a, 6) == 0) {
                s_found[i].rssi = d->getRSSI();
                if (d->getRSSI() > s_found[i].best_rssi) s_found[i].best_rssi = d->getRSSI();
                s_found[i].last_seen = millis();
                return;
            }
        }
        int n = s_found_n;
        if (n >= FIND_MAX) return;
        found_t &f = s_found[n];
        memcpy(f.addr, a, 6);
        strncpy(f.type, kind, sizeof(f.type) - 1);
        f.type[sizeof(f.type) - 1] = '\0';
        f.rssi = d->getRSSI();
        f.best_rssi = f.rssi;
        f.last_seen = millis();
        s_found_n = n + 1;   /* publish LAST — slot fully filled above */
    }
};
static finder_cb s_cb_obj;

static bool s_meter_invalid = true;

static void draw_meter(void)
{
    auto &d = M5Cardputer.Display;
    static const char *last_prox = nullptr;
    static int8_t last_rssi = 127;

    /* Stale timeout: after 4s without a hit, show "no signal". */
    uint32_t age = millis() - s_lock_last;
    int8_t rssi = (age > 4000) ? -100 : s_lock_rssi;

    /* Static header drawn once — label (if any) on row 1, MAC on row 2.
     * Label is set by the "hunt this scanned device" path; tracker-picker
     * path leaves it empty and we just show "HUNT" + MAC like before. */
    if (s_meter_invalid) {
        d.fillRect(0, BODY_Y, SCR_W, BODY_H, T_BG);
        d.setTextColor(T_ACCENT, T_BG);
        d.setCursor(4, BODY_Y + 2);
        if (s_lock_label[0]) {
            d.printf("HUNT  %.18s", s_lock_label);
            d.setTextColor(T_DIM, T_BG);
            d.setCursor(4, BODY_Y + 12);
            d.printf("%02X:%02X:%02X:%02X:%02X:%02X",
                     s_lock[0], s_lock[1], s_lock[2],
                     s_lock[3], s_lock[4], s_lock[5]);
            d.drawFastHLine(4, BODY_Y + 22, SCR_W - 8, T_ACCENT);
        } else {
            d.printf("HUNT  %02X:%02X:%02X:%02X:%02X:%02X",
                     s_lock[0], s_lock[1], s_lock[2],
                     s_lock[3], s_lock[4], s_lock[5]);
            d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT);
        }
        d.drawRect(8, BODY_Y + 78, SCR_W - 16, 8, T_DIM);
        last_prox = nullptr;
        last_rssi = 127;
        s_meter_invalid = false;
    }

    /* Big proximity label. */
    const char *prox;
    uint16_t prox_col;
    if (rssi <= -99)     { prox = "NO SIGNAL"; prox_col = T_DIM;  }
    else if (rssi > -45) { prox = "RIGHT HERE";prox_col = T_BAD;  }
    else if (rssi > -60) { prox = "HOT";       prox_col = T_BAD;  }
    else if (rssi > -72) { prox = "WARM";      prox_col = T_WARN; }
    else if (rssi > -84) { prox = "COOL";      prox_col = T_ACCENT;}
    else                 { prox = "COLD";      prox_col = T_DIM;  }

    if (prox != last_prox) {
        d.fillRect(0, BODY_Y + 22, SCR_W, 24, T_BG);
        d.setTextColor(prox_col, T_BG);
        d.setTextSize(3);
        int w = d.textWidth(prox) * 3;
        d.setCursor((SCR_W - w) / 2, BODY_Y + 22);
        d.print(prox);
        d.setTextSize(1);
        last_prox = prox;
    }

    /* RSSI numeric readout. */
    if (rssi != last_rssi) {
        d.fillRect(0, BODY_Y + 52, SCR_W, 16, T_BG);
        d.setTextColor(T_FG, T_BG);
        d.setTextSize(2);
        char rbuf[16];
        if (rssi <= -99) snprintf(rbuf, sizeof(rbuf), "--- dBm");
        else             snprintf(rbuf, sizeof(rbuf), "%d dBm", rssi);
        int rw = d.textWidth(rbuf) * 2;
        d.setCursor((SCR_W - rw) / 2, BODY_Y + 52);
        d.print(rbuf);
        d.setTextSize(1);

        /* Signal bar, full width — confined to inside the static frame. */
        int pct = (rssi + 100) * 100 / 70;
        if (pct < 0) pct = 0; if (pct > 100) pct = 100;
        int by = BODY_Y + 78;
        int fill = (SCR_W - 18) * pct / 100;
        d.fillRect(9, by + 1, fill, 6, prox_col);
        if (fill < SCR_W - 18) d.fillRect(9 + fill, by + 1, (SCR_W - 18) - fill, 6, T_BG);

        last_rssi = rssi;
    }
}

static bool s_picker_invalid = true;

static void draw_picker_row(int i, int cursor)
{
    auto &d = M5Cardputer.Display;
    int y = BODY_Y + 18 + i * 12;
    if (i >= s_found_n) {
        d.fillRect(0, y - 1, SCR_W, 12, T_BG);
        return;
    }
    const found_t &f = s_found[i];
    bool sel = (i == cursor);
    uint16_t bg = sel ? 0x18C7 : T_BG;
    d.fillRect(0, y - 1, SCR_W, 12, bg);
    d.setTextColor(sel ? T_ACCENT : T_WARN, bg);
    d.setCursor(4, y);
    d.printf("%-9s", f.type);
    d.setTextColor(sel ? T_ACCENT : T_FG, bg);
    d.setCursor(68, y);
    d.printf("%02X:%02X:%02X", f.addr[3], f.addr[4], f.addr[5]);
    d.setTextColor(T_DIM, bg);
    d.setCursor(140, y);
    d.printf("%d/%d", f.rssi, f.best_rssi);
}

static void draw_picker(int cursor)
{
    auto &d = M5Cardputer.Display;
    static int last_cursor = -1;
    static int last_count = -1;
    static bool last_empty = false;

    if (s_picker_invalid) {
        s_picker_invalid = false;
        last_cursor = -1;
        last_count = -1;
        last_empty = false;
    }

    if (s_found_n != last_count) {
        d.fillRect(0, BODY_Y, SCR_W, 14, T_BG);
        d.setTextColor(T_ACCENT, T_BG);
        d.setCursor(4, BODY_Y + 2);
        d.printf("FINDER  %d candidate%s", s_found_n, s_found_n == 1 ? "" : "s");
        d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT);
    }

    if (s_found_n == 0) {
        if (!last_empty || last_count != 0) {
            d.fillRect(0, BODY_Y + 14, SCR_W, BODY_H - 14, T_BG);
            d.setTextColor(T_DIM, T_BG);
            d.setCursor(4, BODY_Y + 28);
            d.print("scanning for nearby trackers...");
            d.setCursor(4, BODY_Y + 42);
            d.print("AirTag / SmartTag / Tile");
        }
        last_empty = true;
        last_count = 0;
        last_cursor = -1;
        return;
    }

    bool full = last_empty || last_count != s_found_n || last_cursor < 0;
    if (full) {
        for (int i = 0; i < 7; ++i) draw_picker_row(i, cursor);
    } else if (cursor != last_cursor) {
        if (last_cursor < 7) draw_picker_row(last_cursor, cursor);
        if (cursor < 7)      draw_picker_row(cursor, cursor);
    }

    last_empty = false;
    last_count = s_found_n;
    last_cursor = cursor;
}

/* Beep a tracker via BLE GATT connect + write. Apple's anti-stalking
 * spec (iOS 15.2+) allows non-owner devices to make a "separated"
 * AirTag play sound. The tag must have been away from its owner for
 * 8+ hours — Apple's intent is letting victims of stalking find tags
 * planted on them. AirTags currently with their owner refuse the write.
 *
 * Protocol (from F-Secure / SR Labs / AirGuard / OpenHaystack research):
 *   - Connect to AirTag via BLE
 *   - Locate Apple Continuity / Find My service (custom 128-bit UUID
 *     7DFC9000-7E2C-11E7-8EF0-9A6F90BE45D5) — characteristic
 *     7DFC9001-... accepts a single-byte write to trigger sound.
 *   - Write 0xAF — known magic value used by AirGuard.
 *
 * If the canonical write fails, also dumps every service and writable
 * characteristic to serial so the operator can iterate on unknown
 * trackers (SmartTag, Tile, knockoffs may use different UUIDs). */
static bool beep_tracker(const uint8_t mac[6], const char *kind)
{
    auto &d = M5Cardputer.Display;
    ui_clear_body();
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2); d.print("BEEP TRACKER");
    d.drawFastHLine(4, BODY_Y + 12, 110, T_ACCENT);
    d.setTextColor(T_FG, T_BG);
    d.setCursor(4, BODY_Y + 18);
    d.printf("%-9s %02X:%02X:%02X:%02X:%02X:%02X",
             kind ? kind : "?", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    /* Stop the current scan — concurrent scan + connect on the same
     * NimBLE host fails on ESP32-S3. */
    NimBLEScan *scan = NimBLEDevice::getScan();
    if (scan && scan->isScanning()) scan->stop();
    delay(50);

    NimBLEClient *client = NimBLEDevice::createClient();
    if (!client) {
        d.setCursor(4, BODY_Y + 44); d.setTextColor(T_BAD, T_BG);
        d.print("client null");
        delay(1500);
        return false;
    }
    client->setConnectTimeout(5000);

    /* Use random address type — AirTags advertise with random MACs. */
    NimBLEAddress target(mac, BLE_ADDR_RANDOM);
    /* Animated CONNECTING screen right before the blocking 5s connect. */
    char tname[24];
    if (kind && kind[0]) snprintf(tname, sizeof(tname), "%s", kind);
    else snprintf(tname, sizeof(tname), "%02X:%02X:%02X:%02X:%02X:%02X",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ui_connecting_screen(tname);
    bool connected = client->connect(target);
    Serial.printf("[beep] connect %02X:%02X:%02X:%02X:%02X:%02X -> %d\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], (int)connected);
    if (!connected) {
        d.setCursor(4, BODY_Y + 44); d.setTextColor(T_BAD, T_BG);
        d.print("connect failed");
        d.setCursor(4, BODY_Y + 56); d.setTextColor(T_DIM, T_BG);
        d.print("(may be paired/protected)");
        NimBLEDevice::deleteClient(client);
        delay(1800);
        return false;
    }

    /* Dump everything to serial — research aid for unknown trackers. */
    Serial.println("[beep] services + chars:");
    const std::vector<NimBLERemoteService *> &svcs = client->getServices(true);
    for (auto *svc : svcs) {
        Serial.printf("  svc %s\n", svc->getUUID().toString().c_str());
        const std::vector<NimBLERemoteCharacteristic *> &chrs = svc->getCharacteristics(true);
        for (auto *c : chrs) {
            Serial.printf("    chr %s%s%s%s%s\n",
                          c->getUUID().toString().c_str(),
                          c->canRead()    ? " R"  : "",
                          c->canWrite()   ? " W"  : "",
                          c->canWriteNoResponse() ? " WNR" : "",
                          c->canNotify()  ? " N"  : "");
        }
    }

    /* Try the canonical AirTag anti-stalking sound write. */
    bool beeped = false;
    NimBLEUUID svc_uuid("7DFC9000-7E2C-11E7-8EF0-9A6F90BE45D5");
    NimBLERemoteService *snd_svc = client->getService(svc_uuid);
    if (snd_svc) {
        NimBLEUUID chr_uuid("7DFC9001-7E2C-11E7-8EF0-9A6F90BE45D5");
        NimBLERemoteCharacteristic *snd_chr = snd_svc->getCharacteristic(chr_uuid);
        if (snd_chr && snd_chr->canWrite()) {
            uint8_t magic = 0xAF;
            beeped = snd_chr->writeValue(&magic, 1, false);
            Serial.printf("[beep] wrote 0xAF -> %d\n", (int)beeped);
        } else {
            Serial.println("[beep] sound characteristic not found / not writable");
        }
    } else {
        Serial.println("[beep] Apple Continuity service not present — likely Tile/SmartTag/owner-attached");
    }

    client->disconnect();
    delay(50);
    NimBLEDevice::deleteClient(client);

    d.setCursor(4, BODY_Y + 44);
    if (beeped) {
        d.setTextColor(T_GOOD, T_BG);
        d.print("BEEP sent");
        d.setCursor(4, BODY_Y + 56); d.setTextColor(T_DIM, T_BG);
        d.print("(only fires if separated 8h+)");
    } else {
        d.setTextColor(T_WARN, T_BG);
        d.print("no audio chr / not separated");
        d.setCursor(4, BODY_Y + 56); d.setTextColor(T_DIM, T_BG);
        d.print("see serial for svc dump");
    }
    delay(2200);
    return beeped;
}

void feat_ble_finder(void)
{
    radio_switch(RADIO_BLE);
    s_found_n = 0;
    s_locating = false;

    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->setScanCallbacks(&s_cb_obj, true);
    scan->setMaxResults(0);   /* POS-AUDIT-011: don't accumulate ad vector */
    /* Active scan — AirTag advertises passively (mfr-data only), but
     * Tile and some SmartTag knockoffs only emit name + service UUID
     * in scan-response, which is invisible to passive scanners. Without
     * active scan we'd under-count Tile devices in the tracker picker. */
    scan->setActiveScan(true);
    /* Aggressive interval → more frequent samples for live RSSI. */
    scan->setInterval(40);
    scan->setWindow(30);
    scan->start(0, false);

    int cursor = 0;
    ui_force_clear_body();
    ui_draw_footer(";/.=move  ENTER=hunt  B=beep  `=back");

    s_picker_invalid = true;
    uint32_t last = 0;
    int last_drawn_cursor = -1;
    int last_drawn_count = -1;
    while (true) {
        if (millis() - last > 250 &&
            (cursor != last_drawn_cursor || s_found_n != last_drawn_count)) {
            last = millis();
            last_drawn_cursor = cursor;
            last_drawn_count = s_found_n;
            draw_picker(cursor);
        }
        /* Keep the empty wait alive: animated radar + "trackers..." strip. */
        if (s_found_n == 0) ui_scanning_indicator("trackers", s_found_n);
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) { scan->stop(); return; }
        if (k == ';' || k == PK_UP)   { if (cursor > 0) cursor--; }
        if (k == '.' || k == PK_DOWN) { if (cursor + 1 < s_found_n) cursor++; }
        if ((k == 'b' || k == 'B') && s_found_n > 0) {
            /* Run beep_tracker — connects, dumps GATT, attempts anti-
             * stalking sound write. Returns to picker after. */
            beep_tracker(s_found[cursor].addr, s_found[cursor].type);
            /* Re-arm scan callback (beep_tracker creates a client which
             * can disrupt scan state). */
            scan = NimBLEDevice::getScan();
            scan->setScanCallbacks(&s_cb_obj, true);
    scan->setMaxResults(0);   /* POS-AUDIT-011: don't accumulate ad vector */
            scan->setActiveScan(false);
            scan->setInterval(40);
            scan->setWindow(30);
            scan->start(0, false);
            ui_force_clear_body();
            ui_draw_footer(";/.=move  ENTER=hunt  B=beep  `=back");
            last = 0;  /* force redraw */
            last_drawn_cursor = -1;
            last_drawn_count = -1;
            s_picker_invalid = true;
        }
        if (k == PK_ENTER && s_found_n > 0) {
            memcpy(s_lock, s_found[cursor].addr, 6);
            s_lock_rssi = s_found[cursor].rssi;
            s_lock_last = millis();
            s_locating = true;
            break;
        }
    }

    /* ---- Hunt loop ---- */
    ui_draw_footer("move around  `=back");
    s_meter_invalid = true;
    uint32_t last_draw = 0;
    uint32_t last_beep = 0;
    while (true) {
        if (millis() - last_draw > 120) { last_draw = millis(); draw_meter(); }

        /* Geiger pulse: period shrinks as signal gets stronger. */
        uint32_t age = millis() - s_lock_last;
        int8_t rssi = (age > 4000) ? -100 : s_lock_rssi;
        uint32_t period;
        if (rssi <= -99)      period = 0;      /* silent */
        else if (rssi > -45)  period = 80;     /* frantic */
        else if (rssi > -60)  period = 160;
        else if (rssi > -72)  period = 300;
        else if (rssi > -84)  period = 600;
        else                  period = 1200;

        if (period > 0 && millis() - last_beep >= period) {
            last_beep = millis();
            int pitch = 600 + ((rssi + 100) * 30);
            if (pitch < 600) pitch = 600;
            if (pitch > 3500) pitch = 3500;
            M5Cardputer.Speaker.tone(pitch, 40);
        }

        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(15); continue; }
        if (k == PK_ESC) break;
    }

    s_locating = false;
    scan->stop();
}

/* Public entry point for "hunt this specific MAC" — called from BLE Scan
 * via the F hotkey on the device-detail screen. Skips the tracker-only
 * picker and goes straight to the Geiger hunt loop on whatever MAC the
 * caller passed in. Label is shown in the header; pass nullptr for
 * MAC-only display. */
void feat_ble_finder_hunt_mac(const uint8_t mac[6], const char *label)
{
    radio_switch(RADIO_BLE);
    /* NimBLE may have been left running by ble_scan; ensure we have our
     * callback registered so live RSSI updates flow into s_lock_rssi. */
    NimBLEScan *scan = NimBLEDevice::getScan();
    if (scan->isScanning()) scan->stop();
    scan->setScanCallbacks(&s_cb_obj, true);
    scan->setMaxResults(0);   /* POS-AUDIT-011: don't accumulate ad vector */
    scan->setActiveScan(false);
    scan->setInterval(40);
    scan->setWindow(30);

    memcpy(s_lock, mac, 6);
    s_lock_rssi = -100;
    s_lock_last = 0;
    s_locating = true;
    if (label && label[0]) {
        strncpy(s_lock_label, label, sizeof(s_lock_label) - 1);
        s_lock_label[sizeof(s_lock_label) - 1] = '\0';
    } else {
        s_lock_label[0] = '\0';
    }

    scan->start(0, false);
    ui_draw_footer("move around  `=back");

    s_meter_invalid = true;
    uint32_t last_draw = 0;
    uint32_t last_beep = 0;
    while (true) {
        if (millis() - last_draw > 120) { last_draw = millis(); draw_meter(); }

        uint32_t age = millis() - s_lock_last;
        int8_t rssi = (age > 4000) ? -100 : s_lock_rssi;
        uint32_t period;
        if (rssi <= -99)      period = 0;
        else if (rssi > -45)  period = 80;
        else if (rssi > -60)  period = 160;
        else if (rssi > -72)  period = 300;
        else if (rssi > -84)  period = 600;
        else                  period = 1200;

        if (period > 0 && millis() - last_beep >= period) {
            last_beep = millis();
            int pitch = 600 + ((rssi + 100) * 30);
            if (pitch < 600) pitch = 600;
            if (pitch > 3500) pitch = 3500;
            M5Cardputer.Speaker.tone(pitch, 40);
        }

        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(15); continue; }
        if (k == PK_ESC) break;
    }

    s_locating = false;
    s_lock_label[0] = '\0';
    scan->stop();
}
