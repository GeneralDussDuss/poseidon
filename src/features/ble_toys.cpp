/*
 * ble_toys — "The Salty Deep": wireless sex toy scanner + controller.
 *
 * Scans for known toy brands (Lovense, WeVibe, Satisfyer, Svakom,
 * Kiiroo, We-Connect, Magic Motion). Connect to one, then Up/Down
 * arrows or number keys control intensity via the brand's protocol.
 *
 * Lovense protocol: write ASCII commands to a specific characteristic
 *   "Vibrate:<0..20>;" — vibrate at level 0 (off) through 20 (max)
 *   "Battery;"         — returns "s<percent>;"
 *   "DeviceType;"      — returns model info
 * Characteristic UUID: 53300001-0023-4bd4-bbd5-a6920e4c5653 (TX)
 * Service UUID:        53300001-0023-4bd4-bbd5-a6920e4c5653
 *
 * Other brands use proprietary binary — not implemented fully; we
 * identify and display them so the user knows to use Buttplug.io
 * instead for those.
 */
#include "app.h"
#include "../theme.h"
#include "ui.h"
#include "input.h"
#include "radio.h"
#include "ble_types.h"
#include <NimBLEDevice.h>

#define MAX_TOYS 16

struct toy_t {
    uint8_t addr[6];
    char    name[24];
    char    brand[12];    /* "Lovense", "WeVibe", "Satisfyer", ... */
    int8_t  rssi;
    uint8_t addr_type;
};

static toy_t s_toys[MAX_TOYS];
static volatile int s_toy_count = 0;

/* Known toy name prefixes / service UUIDs. */
static const char *identify_toy(const NimBLEAdvertisedDevice *d)
{
    std::string name = d->haveName() ? d->getName() : std::string();
    if (!name.empty()) {
        const char *n = name.c_str();
        if (!strncasecmp(n, "LVS-", 4)) return "Lovense";
        if (!strncasecmp(n, "LVR", 3))  return "Lovense";
        if (!strncasecmp(n, "WV-",  3)) return "WeVibe";
        if (!strncasecmp(n, "We-Vibe", 7)) return "WeVibe";
        if (!strncasecmp(n, "Satisfyer", 9)) return "Satisfyer";
        if (!strncasecmp(n, "Svakom", 6)) return "Svakom";
        if (!strncasecmp(n, "Kiiroo", 6)) return "Kiiroo";
        if (!strncasecmp(n, "KB",     2)) return "Kiiroo";
        if (!strncasecmp(n, "MagicM", 6)) return "MagicMotion";
        if (!strncasecmp(n, "Lelo",   4)) return "Lelo";
    }
    /* Lovense service UUID fallback. */
    if (d->haveServiceUUID()) {
        for (int i = 0; i < d->getServiceUUIDCount(); ++i) {
            std::string u = d->getServiceUUID(i).toString();
            if (u.find("53300001") != std::string::npos) return "Lovense";
            if (u.find("fffa") != std::string::npos && !name.empty()) return "WeVibe";
        }
    }
    return nullptr;
}

class scan_cb : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice *d) override {
        const char *brand = identify_toy(d);
        if (!brand) return;
        const uint8_t *a = d->getAddress().getBase()->val;
        for (int i = 0; i < s_toy_count; ++i)
            if (memcmp(s_toys[i].addr, a, 6) == 0) {
                s_toys[i].rssi = d->getRSSI();
                return;
            }
        if (s_toy_count >= MAX_TOYS) return;
        toy_t &t = s_toys[s_toy_count++];
        memcpy(t.addr, a, 6);
        t.addr_type = d->getAddressType();
        strncpy(t.brand, brand, sizeof(t.brand) - 1);
        t.brand[sizeof(t.brand) - 1] = '\0';
        if (d->haveName()) {
            strncpy(t.name, d->getName().c_str(), sizeof(t.name) - 1);
            t.name[sizeof(t.name) - 1] = '\0';
        } else {
            snprintf(t.name, sizeof(t.name), "%02X:%02X:%02X:%02X",
                     a[5], a[4], a[3], a[2]);
        }
        t.rssi = d->getRSSI();
    }
};
static scan_cb s_cb;

static NimBLEClient *s_client = nullptr;
static NimBLERemoteCharacteristic *s_tx_char = nullptr;
static volatile bool s_connected = false;
static int s_intensity = 0;

static bool connect_lovense(const toy_t &t)
{
    if (!s_client) s_client = NimBLEDevice::createClient();
    s_client->setConnectTimeout(6);
    uint8_t mac[6]; memcpy(mac, t.addr, 6);
    NimBLEAddress addr(mac, t.addr_type);
    if (!s_client->connect(addr)) return false;

    /* Walk services looking for a writable characteristic whose UUID
     * starts with 53300001 — that's Lovense TX. */
    const std::vector<NimBLERemoteService *> &svcs = s_client->getServices(true);
    for (auto *svc : svcs) {
        const std::vector<NimBLERemoteCharacteristic *> &chrs = svc->getCharacteristics(true);
        for (auto *chr : chrs) {
            if (chr->canWrite() || chr->canWriteNoResponse()) {
                std::string u = chr->getUUID().toString();
                if (u.find("53300001") != std::string::npos ||
                    u.find("53300002") != std::string::npos) {
                    s_tx_char = chr;
                    s_connected = true;
                    return true;
                }
            }
        }
    }
    /* Fall back: first writable characteristic. */
    for (auto *svc : svcs) {
        const std::vector<NimBLERemoteCharacteristic *> &chrs = svc->getCharacteristics(true);
        for (auto *chr : chrs) {
            if (chr->canWriteNoResponse()) {
                s_tx_char = chr;
                s_connected = true;
                return true;
            }
        }
    }
    s_client->disconnect();
    return false;
}

static void send_vibrate(int level)
{
    if (!s_tx_char || !s_connected) return;
    if (level < 0) level = 0;
    if (level > 20) level = 20;
    char cmd[24];
    snprintf(cmd, sizeof(cmd), "Vibrate:%d;", level);
    s_tx_char->writeValue((uint8_t *)cmd, strlen(cmd), false);
}

static void draw_picker(int cursor, bool scanning)
{
    auto &d = M5Cardputer.Display;
    ui_clear_body();
    d.setTextColor(0xF81F, T_BG);
    d.setCursor(4, BODY_Y + 2);
    d.print("THE SALTY DEEP");
    d.drawFastHLine(4, BODY_Y + 12, 120, 0xF81F);
    if (s_toy_count == 0) {
        d.setTextColor(T_DIM, T_BG);
        d.setCursor(4, BODY_Y + 24);
        d.print(scanning ? "searching the deep..." : "no toys found");
        d.setCursor(4, BODY_Y + 36);
        d.print("Lovense / WeVibe / Satisfyer");
        d.setCursor(4, BODY_Y + 46);
        d.print("Svakom / Kiiroo / Lelo");
        return;
    }
    int rows = 7;
    if (cursor < 0) cursor = 0;
    if (cursor >= s_toy_count) cursor = s_toy_count - 1;
    int first = cursor - rows / 2;
    if (first < 0) first = 0;
    if (first + rows > s_toy_count) first = max(0, s_toy_count - rows);
    for (int r = 0; r < rows && first + r < s_toy_count; ++r) {
        const toy_t &t = s_toys[first + r];
        int y = BODY_Y + 18 + r * 12;
        bool sel = (first + r == cursor);
        uint16_t bg = sel ? 0x3007 : T_BG;
        if (sel) d.fillRect(0, y - 1, SCR_W, 12, bg);
        d.setTextColor(sel ? 0xF81F : 0xFFFF, bg);
        d.setCursor(4, y);
        d.printf("[%s]", t.brand);
        d.setTextColor(sel ? 0xFFFF : T_FG, bg);
        d.setCursor(80, y);
        d.printf("%.18s", t.name);
        d.setTextColor(T_DIM, bg);
        d.setCursor(SCR_W - 28, y);
        d.printf("%4d", t.rssi);
    }
}

static void draw_control(void)
{
    auto &d = M5Cardputer.Display;
    ui_clear_body();
    d.setTextColor(0xF81F, T_BG);
    d.setCursor(4, BODY_Y + 2);  d.print("CONTROL");
    d.drawFastHLine(4, BODY_Y + 12, 70, 0xF81F);

    d.setTextColor(s_connected ? T_GOOD : T_BAD, T_BG);
    d.setCursor(4, BODY_Y + 22);
    d.print(s_connected ? "CONNECTED" : "DISCONNECTED");

    /* Big intensity number. */
    d.setTextColor(0xFFFF, T_BG);
    d.setTextSize(4);
    char buf[4]; snprintf(buf, sizeof(buf), "%2d", s_intensity);
    int w = d.textWidth(buf) * 4;
    d.setCursor((SCR_W - w) / 2, BODY_Y + 34);
    d.print(buf);
    d.setTextSize(1);

    /* Bar. */
    int bx = 8, by = BODY_Y + 78, bw = SCR_W - 16, bh = 8;
    d.drawRect(bx, by, bw, bh, T_DIM);
    uint16_t fill = s_intensity > 15 ? 0xF800 : s_intensity > 8 ? 0xFD20 : 0xF81F;
    d.fillRect(bx + 1, by + 1, (bw - 2) * s_intensity / 20, bh - 2, fill);

    ui_draw_footer("; . or 0-9 = level   SPC=stop   `=disc");
}

void feat_ble_toys(void)
{
    radio_switch(RADIO_BLE);
    s_toy_count = 0;

    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->setScanCallbacks(&s_cb, true);
    scan->setActiveScan(true);
    scan->setInterval(45);
    scan->setWindow(30);
    scan->start(0, false);

    ui_draw_footer(";/. = move  ENTER = connect  `=back");
    int cursor = 0;
    uint32_t last = 0;
    while (true) {
        if (millis() - last > 300) { last = millis(); draw_picker(cursor, true); }
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) { scan->stop(); return; }
        if ((k == ';' || k == PK_UP)   && cursor > 0) { cursor--; }
        if ((k == '.' || k == PK_DOWN) && cursor + 1 < s_toy_count) { cursor++; }
        if (k == PK_ENTER && s_toy_count > 0) {
            scan->stop();
            ui_toast("connecting...", T_WARN, 0);
            if (!connect_lovense(s_toys[cursor])) {
                ui_toast("connect failed", T_BAD, 1200);
                scan->start(0, false);
                continue;
            }
            /* Enter control screen. */
            s_intensity = 0;
            send_vibrate(0);
            while (true) {
                draw_control();
                uint16_t kk = input_poll();
                if (kk == PK_NONE) { delay(30); continue; }
                if (kk == PK_ESC) {
                    send_vibrate(0);
                    s_client->disconnect();
                    s_connected = false;
                    s_tx_char = nullptr;
                    break;
                }
                if (kk == PK_SPACE || kk == '0') {
                    s_intensity = 0;
                    send_vibrate(0);
                } else if (kk == ';' || kk == PK_UP) {
                    if (s_intensity < 20) s_intensity++;
                    send_vibrate(s_intensity);
                } else if (kk == '.' || kk == PK_DOWN) {
                    if (s_intensity > 0)  s_intensity--;
                    send_vibrate(s_intensity);
                } else if (kk >= '1' && kk <= '9') {
                    s_intensity = (kk - '0') * 2;
                    send_vibrate(s_intensity);
                }
            }
            scan->start(0, false);
        }
    }
}
