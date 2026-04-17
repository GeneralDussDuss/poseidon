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

static const char *tracker_kind(NimBLEAdvertisedDevice *d)
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

class finder_cb : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice *d) override {
        NimBLEAddress _addr = d->getAddress(); const uint8_t *a = _addr.getNative();
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
        if (s_found_n >= FIND_MAX) return;
        found_t &f = s_found[s_found_n++];
        memcpy(f.addr, a, 6);
        strncpy(f.type, kind, sizeof(f.type) - 1);
        f.type[sizeof(f.type) - 1] = '\0';
        f.rssi = d->getRSSI();
        f.best_rssi = f.rssi;
        f.last_seen = millis();
    }
};
static finder_cb s_cb_obj;

static void draw_meter(void)
{
    auto &d = M5Cardputer.Display;
    ui_clear_body();

    /* Stale timeout: after 4s without a hit, show "no signal". */
    uint32_t age = millis() - s_lock_last;
    int8_t rssi = (age > 4000) ? -100 : s_lock_rssi;

    /* Header with MAC. */
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2);
    d.printf("HUNT  %02X:%02X:%02X:%02X:%02X:%02X",
             s_lock[0], s_lock[1], s_lock[2],
             s_lock[3], s_lock[4], s_lock[5]);
    d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT);

    /* Big proximity label. */
    const char *prox;
    uint16_t prox_col;
    if (rssi <= -99)     { prox = "NO SIGNAL"; prox_col = T_DIM;  }
    else if (rssi > -45) { prox = "RIGHT HERE";prox_col = T_BAD;  }
    else if (rssi > -60) { prox = "HOT";       prox_col = T_BAD;  }
    else if (rssi > -72) { prox = "WARM";      prox_col = T_WARN; }
    else if (rssi > -84) { prox = "COOL";      prox_col = T_ACCENT;}
    else                 { prox = "COLD";      prox_col = T_DIM;  }

    d.setTextColor(prox_col, T_BG);
    d.setTextSize(3);
    int w = d.textWidth(prox) * 3;
    d.setCursor((SCR_W - w) / 2, BODY_Y + 22);
    d.print(prox);
    d.setTextSize(1);

    /* RSSI numeric readout. */
    d.setTextColor(T_FG, T_BG);
    d.setTextSize(2);
    char rbuf[16];
    if (rssi <= -99) snprintf(rbuf, sizeof(rbuf), "--- dBm");
    else             snprintf(rbuf, sizeof(rbuf), "%d dBm", rssi);
    int rw = d.textWidth(rbuf) * 2;
    d.setCursor((SCR_W - rw) / 2, BODY_Y + 52);
    d.print(rbuf);
    d.setTextSize(1);

    /* Signal bar, full width. */
    int pct = (rssi + 100) * 100 / 70;
    if (pct < 0) pct = 0; if (pct > 100) pct = 100;
    int by = BODY_Y + 78;
    d.drawRect(8, by, SCR_W - 16, 8, T_DIM);
    d.fillRect(9, by + 1, (SCR_W - 18) * pct / 100, 6, prox_col);
}

static void draw_picker(int cursor)
{
    auto &d = M5Cardputer.Display;
    ui_clear_body();
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2);
    d.printf("FINDER  %d candidate%s", s_found_n, s_found_n == 1 ? "" : "s");
    d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT);

    if (s_found_n == 0) {
        d.setTextColor(T_DIM, T_BG);
        d.setCursor(4, BODY_Y + 28);
        d.print("scanning for nearby trackers...");
        d.setCursor(4, BODY_Y + 42);
        d.print("AirTag / SmartTag / Tile");
        return;
    }
    for (int i = 0; i < s_found_n && i < 7; ++i) {
        const found_t &f = s_found[i];
        int y = BODY_Y + 18 + i * 12;
        bool sel = (i == cursor);
        if (sel) d.fillRect(0, y - 1, SCR_W, 12, 0x18C7);
        d.setTextColor(sel ? T_ACCENT : T_WARN, sel ? 0x18C7 : T_BG);
        d.setCursor(4, y);
        d.printf("%-9s", f.type);
        d.setTextColor(sel ? T_ACCENT : T_FG, sel ? 0x18C7 : T_BG);
        d.setCursor(68, y);
        d.printf("%02X:%02X:%02X", f.addr[3], f.addr[4], f.addr[5]);
        d.setTextColor(T_DIM, sel ? 0x18C7 : T_BG);
        d.setCursor(140, y);
        d.printf("%d/%d", f.rssi, f.best_rssi);
    }
}

void feat_ble_finder(void)
{
    radio_switch(RADIO_BLE);
    s_found_n = 0;
    s_locating = false;

    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->setAdvertisedDeviceCallbacks(&s_cb_obj, true);
    scan->setActiveScan(false);
    /* Aggressive interval → more frequent samples for live RSSI. */
    scan->setInterval(40);
    scan->setWindow(30);
    scan->start(0, nullptr, false);

    int cursor = 0;
    ui_draw_footer(";/.=move  ENTER=hunt  `=back");

    uint32_t last = 0;
    while (true) {
        if (millis() - last > 250) { last = millis(); draw_picker(cursor); }
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) { scan->stop(); return; }
        if (k == ';' || k == PK_UP)   { if (cursor > 0) cursor--; }
        if (k == '.' || k == PK_DOWN) { if (cursor + 1 < s_found_n) cursor++; }
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
