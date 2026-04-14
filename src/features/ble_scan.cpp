/*
 * ble_scan — passive BLE scan with device type detection.
 *
 * NimBLE-Arduino async scan. Results sorted by RSSI. Types guessed
 * from advertisement manufacturer data and service UUIDs, same
 * heuristics as Davey Jones.
 *
 * Keys in the results list:
 *   ; / .    scroll cursor
 *   /        filter by name/type substring
 *   R        rescan
 *   ENTER    show details
 *   FN+`     back
 */
#include "app.h"
#include "ui.h"
#include "input.h"
#include "radio.h"
#include <NimBLEDevice.h>

struct ble_dev_t {
    uint8_t  addr[6];
    char     name[20];
    char     type[10];   /* "Apple", "AirTag", "HID", "BLE", ... */
    int8_t   rssi;
    bool     is_public;
};

#define BLE_MAX_DEVS 48

static ble_dev_t s_devs[BLE_MAX_DEVS];
static int       s_count = 0;
static char      s_filter[24] = "";
static volatile bool s_scanning = false;

/* Sanitize a potentially non-printable name from a BLE advertisement. */
static void sanitize(const char *in, char *out, size_t out_sz)
{
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 1 < out_sz; ++i) {
        uint8_t c = (uint8_t)in[i];
        if (c >= 0x20 && c < 0x7F) out[j++] = (char)c;
    }
    out[j] = '\0';
}

/* Best-effort type classification. */
static const char *guess_type(NimBLEAdvertisedDevice *d)
{
    /* Manufacturer data. */
    if (d->haveManufacturerData()) {
        std::string md = d->getManufacturerData();
        if (md.size() >= 2) {
            uint16_t cid = (uint8_t)md[0] | ((uint8_t)md[1] << 8);
            if (cid == 0x004C) {           /* Apple */
                if (md.size() >= 3 && (uint8_t)md[2] == 0x12) return "AirTag";
                if (md.size() >= 3 && (uint8_t)md[2] == 0x07) return "AirPod";
                return "Apple";
            }
            if (cid == 0x0075) return "Samsung";
            if (cid == 0x0006) return "MS";
            if (cid == 0x00E0) return "Google";
        }
    }
    if (d->haveServiceUUID()) {
        for (int i = 0; i < d->getServiceUUIDCount(); ++i) {
            NimBLEUUID u = d->getServiceUUID(i);
            if (u.equals(NimBLEUUID((uint16_t)0x1812))) return "HID";
            if (u.equals(NimBLEUUID((uint16_t)0x180F))) return "Battery";
            if (u.equals(NimBLEUUID((uint16_t)0x180D))) return "HRM";
            if (u.equals(NimBLEUUID((uint16_t)0xFE2C))) return "FastPair";
            if (u.equals(NimBLEUUID((uint16_t)0xFD6F))) return "Exposure";
        }
    }
    return "BLE";
}

class scan_cb : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice *d) override {
        /* Dedup by address. */
        NimBLEAddress addr = d->getAddress();
        for (int i = 0; i < s_count; ++i) {
            if (memcmp(s_devs[i].addr, addr.getNative(), 6) == 0) {
                if (d->getRSSI() > s_devs[i].rssi)
                    s_devs[i].rssi = d->getRSSI();
                return;
            }
        }
        if (s_count >= BLE_MAX_DEVS) return;
        ble_dev_t &x = s_devs[s_count++];
        memcpy(x.addr, addr.getNative(), 6);
        x.rssi = d->getRSSI();
        x.is_public = (addr.getType() == BLE_ADDR_PUBLIC);
        x.name[0] = '\0';
        if (d->haveName()) sanitize(d->getName().c_str(), x.name, sizeof(x.name));
        strncpy(x.type, guess_type(d), sizeof(x.type) - 1);
        x.type[sizeof(x.type) - 1] = '\0';
    }
};

/* Static-allocated so repeat-entry doesn't leak. NimBLE doesn't own
 * the object after scan stops — it just drops the callback pointer. */
static scan_cb s_cb_obj;
static scan_cb *s_cb = &s_cb_obj;

static bool dev_matches_filter(const ble_dev_t &d)
{
    if (s_filter[0] == '\0') return true;
    return (strcasestr(d.name, s_filter) != nullptr)
        || (strcasestr(d.type, s_filter) != nullptr);
}

static int sort_fn(const void *a, const void *b)
{
    return ((const ble_dev_t *)b)->rssi - ((const ble_dev_t *)a)->rssi;
}

static void draw_list(int cursor)
{
    auto &d = M5Cardputer.Display;
    ui_clear_body();

    d.setTextColor(COL_ACCENT, COL_BG);
    d.setCursor(4, BODY_Y + 2);
    d.printf("BLE %d", s_count);
    if (s_filter[0]) {
        d.setTextColor(COL_WARN, COL_BG);
        d.printf("  filter:%s", s_filter);
    }
    d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, COL_ACCENT);

    /* Filter + window. */
    int idx[BLE_MAX_DEVS];
    int n = 0;
    for (int i = 0; i < s_count; ++i)
        if (dev_matches_filter(s_devs[i])) idx[n++] = i;
    if (n == 0) {
        d.setTextColor(COL_DIM, COL_BG);
        d.setCursor(4, BODY_Y + 24);
        d.print(s_scanning ? "scanning..." : "no devices");
        return;
    }
    if (cursor < 0) cursor = 0;
    if (cursor >= n) cursor = n - 1;

    int rows = 9;
    int first = cursor - rows / 2;
    if (first < 0) first = 0;
    if (first + rows > n) first = max(0, n - rows);

    for (int r = 0; r < rows && first + r < n; ++r) {
        int i = idx[first + r];
        const ble_dev_t &x = s_devs[i];
        int y = BODY_Y + 16 + r * 11;
        bool sel = (first + r == cursor);
        uint16_t bg = sel ? 0x18C7 : COL_BG;
        if (sel) d.fillRect(0, y - 1, SCR_W, 11, bg);

        d.setTextColor(COL_ACCENT, bg);
        d.setCursor(2, y);  d.printf("%4d", x.rssi);
        d.setTextColor(x.is_public ? COL_WARN : COL_GOOD, bg);
        d.setCursor(32, y); d.printf("%-7.7s", x.type);
        d.setTextColor(sel ? COL_ACCENT : COL_FG, bg);
        d.setCursor(80, y);
        if (x.name[0]) {
            d.printf("%.20s", x.name);
        } else {
            d.printf("%02X:%02X:%02X", x.addr[3], x.addr[4], x.addr[5]);
        }
    }
}

static void start_scan(void)
{
    NimBLEScan *scan = NimBLEDevice::getScan();
    /* s_cb is static-allocated; no alloc needed. */
    scan->setAdvertisedDeviceCallbacks(s_cb, /*wantDuplicates=*/true);
    scan->setActiveScan(false);
    scan->setInterval(45);
    scan->setWindow(30);
    s_scanning = true;
    scan->start(/*duration_sec=*/6, [](NimBLEScanResults) {
        s_scanning = false;
        qsort(s_devs, s_count, sizeof(ble_dev_t), sort_fn);
    }, /*is_continue=*/false);
}

void feat_ble_scan(void)
{
    radio_switch(RADIO_BLE);
    s_count = 0;
    s_filter[0] = '\0';

    ui_draw_status(radio_name(), "scan");
    ui_draw_footer("/=filter  R=rescan  ENTER=info  `=back");
    draw_list(0);

    start_scan();

    int cursor = 0;
    uint32_t last_redraw = 0;
    while (true) {
        uint32_t now = millis();
        if (now - last_redraw > 350) {
            last_redraw = now;
            ui_draw_status(radio_name(), s_scanning ? "scan..." : "done");
            draw_list(cursor);
        }

        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) break;

        switch (k) {
        case ';': case PK_UP:    cursor = (cursor > 0) ? cursor - 1 : 0;          draw_list(cursor); break;
        case '.': case PK_DOWN:  cursor++;                                        draw_list(cursor); break;
        case 'r': case 'R':
            if (!s_scanning) { s_count = 0; start_scan(); }
            break;
        case '/':
            if (!input_line("Filter name/type:", s_filter, sizeof(s_filter)))
                s_filter[0] = '\0';
            draw_list(cursor);
            break;
        case PK_ENTER: {
            /* Show detail view for the selected device. */
            int idx[BLE_MAX_DEVS];
            int n = 0;
            for (int i = 0; i < s_count; ++i)
                if (dev_matches_filter(s_devs[i])) idx[n++] = i;
            if (n == 0 || cursor >= n) break;
            const ble_dev_t &x = s_devs[idx[cursor]];
            ui_clear_body();
            auto &d = M5Cardputer.Display;
            d.setTextColor(COL_ACCENT, COL_BG);
            d.setCursor(4, BODY_Y + 2);  d.print("BLE DEVICE");
            d.drawFastHLine(4, BODY_Y + 12, 100, COL_ACCENT);
            d.setTextColor(COL_FG, COL_BG);
            d.setCursor(4, BODY_Y + 18);
            d.printf("MAC : %02X:%02X:%02X:%02X:%02X:%02X",
                     x.addr[0], x.addr[1], x.addr[2],
                     x.addr[3], x.addr[4], x.addr[5]);
            d.setCursor(4, BODY_Y + 30); d.printf("TYPE: %s", x.type);
            d.setCursor(4, BODY_Y + 42); d.printf("NAME: %s", x.name[0] ? x.name : "(unnamed)");
            d.setCursor(4, BODY_Y + 54); d.printf("RSSI: %d dBm", x.rssi);
            d.setCursor(4, BODY_Y + 66); d.printf("ADDR: %s", x.is_public ? "public" : "random");
            ui_draw_footer("`=back");
            while (true) {
                uint16_t k2 = input_poll();
                if (k2 == PK_NONE) { delay(20); continue; }
                if (k2 == PK_ESC) break;
            }
            draw_list(cursor);
            ui_draw_footer("/=filter  R=rescan  ENTER=info  `=back");
            break;
        }
        default: break;
        }
    }

    /* Stop scan politely. */
    NimBLEScan *scan = NimBLEDevice::getScan();
    if (scan) scan->stop();
}
