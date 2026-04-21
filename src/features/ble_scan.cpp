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
#include "../theme.h"
#include "ui.h"
#include "input.h"
#include "radio.h"
#include "menu.h"
#include "ble_types.h"
#include "ble_db.h"
#include "sd_helper.h"
#include <NimBLEDevice.h>
#include <SD.h>

/* Shared with other BLE features. */
ble_target_t g_ble_target = {};
bool         g_ble_target_valid = false;

extern void feat_ble_spam(void);
extern void feat_ble_hid(void);
extern void feat_ble_clone(void);
extern void feat_ble_gatt(void);
extern void feat_ble_flood(void);
extern void feat_ble_whisperpair_from_target(void);  /* skips own scan phase */

struct ble_dev_t {
    uint8_t  addr[6];
    char     name[20];
    char     type[24];   /* "AirPods Pro 2 (USB-C)", "Samsung", etc. */
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

/* Best-effort type classification using the full ble_db tables. */
static void classify(const NimBLEAdvertisedDevice *d, char *out, size_t out_sz)
{
    /* Raw manufacturer data for the db to parse. */
    std::string md;
    if (d->haveManufacturerData()) md = d->getManufacturerData();

    /* NimBLE stores the MAC little-endian. Reverse for display/OUI.
     * Crucial: bind the NimBLEAddress to a named local first —
     * getAddress() returns by value and getNative() hands out a
     * pointer into that temporary. Using it after the full
     * expression is a dangling read and crashes randomly. */
    NimBLEAddress addr = d->getAddress();
    const uint8_t *raw = addr.getBase()->val;
    uint8_t mac_be[6];
    for (int i = 0; i < 6; ++i) mac_be[i] = raw[5 - i];

    /* Only pass OUI if address is PUBLIC — random MACs have no OUI. */
    const uint8_t *oui_ptr = (d->getAddressType() == BLE_ADDR_PUBLIC) ? mac_be : nullptr;

    bool got = ble_db_identify(oui_ptr,
                               md.empty() ? nullptr : (const uint8_t *)md.data(),
                               (int)md.size(),
                               out, out_sz);
    if (got) return;

    /* Service UUID fallback. NimBLE stringifies 16-bit UUIDs as the
     * full 128-bit form "0000XXXX-0000-1000-8000-00805f9b34fb" where
     * XXXX is the 16-bit value (chars 4-8), NOT the trailing 4 chars. */
    if (d->haveServiceUUID()) {
        for (int i = 0; i < d->getServiceUUIDCount(); ++i) {
            NimBLEUUID u = d->getServiceUUID(i);
            std::string s = u.toString();
            uint16_t u16 = 0;
            bool got16 = false;
            if (s.size() == 4) {
                /* Already bare "XXXX" form. */
                sscanf(s.c_str(), "%4hx", &u16);
                got16 = true;
            } else if (s.size() >= 8) {
                /* Full 128-bit form: chars 4..7. */
                sscanf(s.c_str() + 4, "%4hx", &u16);
                got16 = true;
            }
            if (got16) {
                const char *nm = ble_db_svc_uuid(u16);
                if (nm) { snprintf(out, out_sz, "%s", nm); return; }
            }
        }
    }
    snprintf(out, out_sz, "BLE");
}

class scan_cb : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice *d) override {
        /* Dedup by address. */
        NimBLEAddress addr = d->getAddress();
        for (int i = 0; i < s_count; ++i) {
            if (memcmp(s_devs[i].addr, addr.getBase()->val, 6) == 0) {
                if (d->getRSSI() > s_devs[i].rssi)
                    s_devs[i].rssi = d->getRSSI();
                return;
            }
        }
        if (s_count >= BLE_MAX_DEVS) return;
        ble_dev_t &x = s_devs[s_count++];
        memcpy(x.addr, addr.getBase()->val, 6);
        x.rssi = d->getRSSI();
        x.is_public = (addr.getType() == BLE_ADDR_PUBLIC);
        x.name[0] = '\0';
        if (d->haveName()) sanitize(d->getName().c_str(), x.name, sizeof(x.name));
        classify(d, x.type, sizeof(x.type));
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

    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2);
    d.printf("BLE %d", s_count);
    if (s_filter[0]) {
        d.setTextColor(T_WARN, T_BG);
        d.printf("  filter:%s", s_filter);
    }
    d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT);

    /* Filter + window. */
    int idx[BLE_MAX_DEVS];
    int n = 0;
    for (int i = 0; i < s_count; ++i)
        if (dev_matches_filter(s_devs[i])) idx[n++] = i;
    if (n == 0) {
        d.setTextColor(T_DIM, T_BG);
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
        uint16_t bg = sel ? 0x18C7 : T_BG;
        if (sel) d.fillRect(0, y - 1, SCR_W, 11, bg);

        d.setTextColor(T_ACCENT, bg);
        d.setCursor(2, y);  d.printf("%4d", x.rssi);
        d.setTextColor(x.is_public ? T_WARN : T_GOOD, bg);
        d.setCursor(30, y); d.printf("%-14.14s", x.type);
        d.setTextColor(sel ? T_ACCENT : T_FG, bg);
        d.setCursor(124, y);
        if (x.name[0]) {
            d.printf("%.19s", x.name);
        } else {
            d.printf("%02X:%02X:%02X", x.addr[3], x.addr[4], x.addr[5]);
        }
    }
}

static void start_scan(void)
{
    NimBLEScan *scan = NimBLEDevice::getScan();
    /* s_cb is static-allocated; no alloc needed. */
    scan->setScanCallbacks(s_cb, /*wantDuplicates=*/true);
    scan->setActiveScan(false);
    scan->setInterval(45);
    scan->setWindow(30);
    s_scanning = true;
    /* NimBLE 2.x: start(duration_ms, is_continue). Callback on completion
     * is gone — we poll s_scanning from the UI loop which calls
     * scan->isScanning() directly via the poll on line ~240. */
    scan->start(6000, false);  /* 6000ms = 6s, matches pre-migration behaviour */
}

void feat_ble_scan(void)
{
    radio_switch(RADIO_BLE);
    s_count = 0;
    s_filter[0] = '\0';

    ui_draw_status(radio_name(), "scan");
    ui_draw_footer("/=flt S=save R=rescan ENTER=info `=back");
    draw_list(0);

    start_scan();

    int cursor = 0;
    uint32_t last_redraw = 0;
    /* Track last painted state — only redraw the list on actual change. */
    int  last_count   = -1;
    int  last_cursor  = -1;
    bool last_running = !s_scanning;
    while (true) {
        /* NimBLE 2.x has no scan-completed callback — poll isScanning()
         * and run the qsort when the scan finishes. */
        if (s_scanning) {
            NimBLEScan *scan = NimBLEDevice::getScan();
            if (scan && !scan->isScanning()) {
                s_scanning = false;
                qsort(s_devs, s_count, sizeof(ble_dev_t), sort_fn);
            }
        }

        bool state_changed = (s_count != last_count)
                          || (cursor != last_cursor)
                          || (s_scanning != last_running);

        /* Only redraw on state change — or every 2 s while scanning, in
         * case new devices were discovered without us otherwise noticing. */
        if (state_changed || (s_scanning && millis() - last_redraw > 2000)) {
            last_redraw = millis();
            ui_draw_status(radio_name(), s_scanning ? "scan..." : "done");
            draw_list(cursor);
            last_count   = s_count;
            last_cursor  = cursor;
            last_running = s_scanning;
        }
        /* Tiny radar sweep in the top-right corner while scanning. */
        if (s_scanning) ui_radar(SCR_W - 18, BODY_Y + 10, 8, 0x07FF);

        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) break;

        switch (k) {
        /* Handlers only update state — gate at top of loop redraws once. */
        case ';': case PK_UP:    cursor = (cursor > 0) ? cursor - 1 : 0; break;
        case '.': case PK_DOWN:  cursor++; break;
        case 'r': case 'R':
            if (!s_scanning) { s_count = 0; start_scan(); }
            break;
        case '?':
            ui_show_current_help();
            last_count = -1;     /* force redraw after help modal */
            ui_draw_footer("/=flt S=save R=rescan ENTER=info `=back");
            break;
        case '/':
            if (!input_line("Filter name/type:", s_filter, sizeof(s_filter)))
                s_filter[0] = '\0';
            last_count = -1;     /* filter changed — force redraw */
            break;
        case 's': case 'S': {
            if (s_count == 0) { ui_toast("no results", T_WARN, 800); break; }
            if (!sd_mount()) { ui_toast("SD needed", T_BAD, 1000); break; }
            SD.mkdir("/poseidon");
            char path[48];
            snprintf(path, sizeof(path), "/poseidon/blescan-%lu.csv",
                     (unsigned long)(millis() / 1000));
            File f = SD.open(path, FILE_WRITE);
            if (!f) { ui_toast("open failed", T_BAD, 1000); break; }
            f.println("mac,addr_type,type,name,rssi");
            int wrote = 0;
            for (int i = 0; i < s_count; ++i) {
                if (!dev_matches_filter(s_devs[i])) continue;
                const ble_dev_t &x = s_devs[i];
                f.printf("%02X:%02X:%02X:%02X:%02X:%02X,%s,\"%s\",\"%s\",%d\n",
                         x.addr[0], x.addr[1], x.addr[2],
                         x.addr[3], x.addr[4], x.addr[5],
                         x.is_public ? "public" : "random",
                         x.type, x.name, (int)x.rssi);
                wrote++;
            }
            f.close();
            char msg[32];
            snprintf(msg, sizeof(msg), "saved %d devs", wrote);
            ui_toast(msg, T_GOOD, 1000);
            Serial.printf("[ble_scan] saved %s (%d rows)\n", path, wrote);
            break;
        }
        case PK_ENTER: {
            int idx[BLE_MAX_DEVS];
            int n = 0;
            for (int i = 0; i < s_count; ++i)
                if (dev_matches_filter(s_devs[i])) idx[n++] = i;
            if (n == 0 || cursor >= n) break;
            const ble_dev_t &x = s_devs[idx[cursor]];

            /* Populate shared target for C/F/H hotkeys. */
            memcpy(g_ble_target.addr, x.addr, 6);
            strncpy(g_ble_target.name, x.name, sizeof(g_ble_target.name) - 1);
            g_ble_target.name[sizeof(g_ble_target.name) - 1] = '\0';
            strncpy(g_ble_target.type, x.type, sizeof(g_ble_target.type) - 1);
            g_ble_target.type[sizeof(g_ble_target.type) - 1] = '\0';
            g_ble_target.rssi = x.rssi;
            g_ble_target.is_public = x.is_public;
            g_ble_target.have_adv = false;
            g_ble_target.adv_len = 0;
            g_ble_target_valid = true;

            /* Detail view with signal bar + action hotkeys. */
            auto &d = M5Cardputer.Display;
            ui_clear_body();
            d.setTextColor(T_ACCENT, T_BG);
            d.setCursor(4, BODY_Y + 2); d.print("BLE DEVICE");
            d.drawFastHLine(4, BODY_Y + 12, 100, T_ACCENT);
            d.setTextColor(T_FG, T_BG);
            d.setCursor(4, BODY_Y + 18);
            d.printf("MAC  : %02X:%02X:%02X:%02X:%02X:%02X",
                     x.addr[0], x.addr[1], x.addr[2],
                     x.addr[3], x.addr[4], x.addr[5]);
            d.setCursor(4, BODY_Y + 30); d.printf("TYPE : %s", x.type);
            d.setCursor(4, BODY_Y + 42); d.printf("NAME : %.28s", x.name[0] ? x.name : "(unnamed)");
            d.setCursor(4, BODY_Y + 54); d.printf("ADDR : %s", x.is_public ? "public" : "random");

            /* Signal strength bar: -100 dBm → empty, -30 dBm → full. */
            int bar_w = 120;
            int pct = (x.rssi + 100) * 100 / 70;
            if (pct < 0) pct = 0; if (pct > 100) pct = 100;
            d.setCursor(4, BODY_Y + 66); d.printf("RSSI : %d dBm", x.rssi);
            d.drawRect(90, BODY_Y + 66, bar_w, 7, T_DIM);
            uint16_t col = (x.rssi > -60) ? T_GOOD : (x.rssi > -80) ? T_WARN : T_BAD;
            d.fillRect(91, BODY_Y + 67, (bar_w - 2) * pct / 100, 5, col);

            ui_draw_footer("G=gatt C=clone H=hid X=flood P=spam W=whisper `=back");
            while (true) {
                uint16_t k2 = input_poll();
                if (k2 == PK_NONE) { delay(20); continue; }
                if (k2 == PK_ESC) break;
                char ch = (char)tolower((int)k2);
                if (ch == 'g') { feat_ble_gatt();  break; }
                if (ch == 'c') { feat_ble_clone(); break; }
                if (ch == 'h') { feat_ble_hid();   break; }
                if (ch == 'x') { feat_ble_flood(); break; }
                if (ch == 'p') { feat_ble_spam();  break; }
                if (ch == 'w') { feat_ble_whisperpair_from_target(); break; }
            }
            last_count = -1;  /* returning from sub-feature — force fresh paint */
            ui_draw_footer("/=flt S=save R=rescan ENTER=info `=back");
            break;
        }
        default: break;
        }
    }

    /* Stop scan politely. */
    NimBLEScan *scan = NimBLEDevice::getScan();
    if (scan) scan->stop();
}
