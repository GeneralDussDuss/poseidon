/*
 * ble_gatt — connect to a scanned BLE device, enumerate services +
 * characteristics, read/write values. Pocket nRF-Connect for the
 * Cardputer.
 *
 * Flow:
 *   1. Target comes from g_ble_target (set by scan's ENTER key).
 *   2. Connect, discover services, drill into characteristics.
 *   3. Browse a tree with UUIDs + properties (R/W/N).
 *   4. ENTER on a characteristic reads it; W writes a typed hex string.
 *
 * Useful for: grabbing device names / serial numbers, probing writable
 * characteristics on smart locks / IoT devices, dumping manufacturer
 * data, testing auth bypass.
 */
#include "app.h"
#include "../theme.h"
#include "ui.h"
#include "input.h"
#include "radio.h"
#include "menu.h"
#include "ble_types.h"
#include <NimBLEDevice.h>

#define MAX_FLAT 48

struct gatt_node_t {
    bool     is_svc;
    uint16_t handle;
    NimBLEUUID uuid;
    uint8_t  props;   /* NIMBLE_PROPERTY bits */
    uint8_t  depth;   /* 0 = service, 1 = char */
    NimBLERemoteCharacteristic *chr;
    NimBLERemoteService        *svc;
};

static gatt_node_t s_flat[MAX_FLAT];
static int s_flat_n = 0;

static NimBLEClient *s_client = nullptr;
static volatile bool s_connected = false;

static void add_node(bool is_svc, uint8_t depth, NimBLEUUID u, uint8_t props,
                     NimBLERemoteService *svc, NimBLERemoteCharacteristic *chr)
{
    if (s_flat_n >= MAX_FLAT) return;
    gatt_node_t &n = s_flat[s_flat_n++];
    n.is_svc = is_svc;
    n.depth  = depth;
    n.uuid   = u;
    n.props  = props;
    n.svc    = svc;
    n.chr    = chr;
}

static void enumerate_services(void)
{
    s_flat_n = 0;
    const std::vector<NimBLERemoteService *> &svcs = s_client->getServices(true);
    for (auto *svc : svcs) {
        add_node(true, 0, svc->getUUID(), 0, svc, nullptr);
        const std::vector<NimBLERemoteCharacteristic *> &chrs = svc->getCharacteristics(true);
        for (auto *chr : chrs) {
            uint8_t props = 0;
            if (chr->canRead())            props |= 0x01;
            if (chr->canWrite())           props |= 0x02;
            if (chr->canWriteNoResponse()) props |= 0x04;
            if (chr->canNotify())          props |= 0x08;
            if (chr->canIndicate())        props |= 0x10;
            add_node(false, 1, chr->getUUID(), props, svc, chr);
        }
    }
}

static bool hex_parse(const char *s, uint8_t *out, int max, int *out_len)
{
    int n = 0;
    while (*s && n < max) {
        while (*s == ' ' || *s == ':') s++;
        if (!*s) break;
        char buf[3] = { s[0], s[1], 0 };
        if (!isxdigit(buf[0]) || !isxdigit(buf[1])) return false;
        out[n++] = (uint8_t)strtoul(buf, nullptr, 16);
        s += 2;
    }
    *out_len = n;
    return true;
}

static void print_hex(const uint8_t *buf, size_t n)
{
    auto &d = M5Cardputer.Display;
    d.setCursor(4, BODY_Y + 54);
    int limit = n < 8 ? (int)n : 8;
    for (int i = 0; i < limit; ++i) d.printf("%02X ", buf[i]);
    if ((int)n > limit) d.printf("+%d", (int)n - limit);
    /* ASCII preview row below. */
    d.setCursor(4, BODY_Y + 66);
    d.setTextColor(T_DIM, T_BG);
    for (int i = 0; i < limit; ++i) {
        uint8_t c = buf[i];
        d.printf("%c", (c >= 32 && c < 127) ? c : '.');
    }
    d.setTextColor(T_FG, T_BG);
}

static void show_characteristic(int idx)
{
    auto &d = M5Cardputer.Display;
    gatt_node_t &n = s_flat[idx];
    if (n.is_svc || !n.chr) return;

    ui_clear_body();
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2); d.print("CHAR");
    d.drawFastHLine(4, BODY_Y + 12, 40, T_ACCENT);
    d.setTextColor(T_FG, T_BG);
    d.setCursor(4, BODY_Y + 16); d.printf("uuid: %.30s", n.uuid.toString().c_str());

    char props_str[20] = "";
    if (n.props & 0x01) strcat(props_str, "R ");
    if (n.props & 0x02) strcat(props_str, "W ");
    if (n.props & 0x04) strcat(props_str, "w ");
    if (n.props & 0x08) strcat(props_str, "N ");
    if (n.props & 0x10) strcat(props_str, "I ");
    d.setCursor(4, BODY_Y + 28); d.printf("prop: %s", props_str);

    ui_draw_footer("R=read  W=write  `=back");

    std::string last_val;
    while (true) {
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) return;

        char ch = (char)tolower((int)k);
        if (ch == 'r' && n.chr->canRead()) {
            last_val = n.chr->readValue();
            d.fillRect(0, BODY_Y + 42, SCR_W, 40, T_BG);
            d.setTextColor(T_GOOD, T_BG);
            d.setCursor(4, BODY_Y + 42);
            d.printf("READ %d bytes", (int)last_val.size());
            print_hex((const uint8_t *)last_val.data(), last_val.size());
        } else if (ch == 'w' && (n.chr->canWrite() || n.chr->canWriteNoResponse())) {
            char hex[128];
            if (!input_line("hex to write (AA BB CC):", hex, sizeof(hex))) continue;
            uint8_t buf[48];
            int len = 0;
            if (!hex_parse(hex, buf, sizeof(buf), &len) || len == 0) {
                ui_toast("bad hex", T_BAD, 800);
                continue;
            }
            bool ok = n.chr->writeValue(buf, len, n.chr->canWriteNoResponse() ? false : true);
            d.fillRect(0, BODY_Y + 42, SCR_W, 40, T_BG);
            d.setTextColor(ok ? T_GOOD : T_BAD, T_BG);
            d.setCursor(4, BODY_Y + 42);
            d.printf("WRITE %d → %s", len, ok ? "OK" : "FAIL");
        }
    }
}

static void draw_tree(int cursor)
{
    auto &d = M5Cardputer.Display;
    ui_clear_body();
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2);
    d.printf("GATT  %d nodes  %s", s_flat_n, s_connected ? "LIVE" : "disc");
    d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT);

    if (s_flat_n == 0) {
        d.setTextColor(T_DIM, T_BG);
        d.setCursor(4, BODY_Y + 24);
        d.print("enumerating...");
        return;
    }
    int rows = 8;
    if (cursor < 0) cursor = 0;
    if (cursor >= s_flat_n) cursor = s_flat_n - 1;
    int first = cursor - rows / 2;
    if (first < 0) first = 0;
    if (first + rows > s_flat_n) first = max(0, s_flat_n - rows);

    for (int r = 0; r < rows && first + r < s_flat_n; ++r) {
        const gatt_node_t &n = s_flat[first + r];
        int y = BODY_Y + 18 + r * 11;
        bool sel = (first + r == cursor);
        if (sel) d.fillRect(0, y - 1, SCR_W, 11, 0x18C7);
        uint16_t bg = sel ? 0x18C7 : T_BG;
        if (n.is_svc) {
            d.setTextColor(T_WARN, bg);
            d.setCursor(4, y);
            d.printf("SVC %.28s", n.uuid.toString().c_str());
        } else {
            d.setTextColor(sel ? T_ACCENT : T_FG, bg);
            d.setCursor(12, y);
            d.printf("%.22s", n.uuid.toString().c_str());
            d.setTextColor(T_DIM, bg);
            d.setCursor(SCR_W - 48, y);
            if (n.props & 0x01) d.print("R");
            if (n.props & 0x02) d.print("W");
            if (n.props & 0x04) d.print("w");
            if (n.props & 0x08) d.print("N");
        }
    }
}

static bool try_connect(NimBLEAddress addr)
{
    if (!s_client) s_client = NimBLEDevice::createClient();
    s_client->setConnectTimeout(5);
    bool ok = s_client->connect(addr);
    s_connected = ok;
    return ok;
}

void feat_ble_gatt(void)
{
    if (!g_ble_target_valid) {
        ui_toast("scan + select first", T_WARN, 1200);
        return;
    }
    radio_switch(RADIO_BLE);

    /* Connecting UI. */
    ui_clear_body();
    auto &d = M5Cardputer.Display;
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2); d.print("GATT EXPLORER");
    d.drawFastHLine(4, BODY_Y + 12, 100, T_ACCENT);
    d.setTextColor(T_FG, T_BG);
    d.setCursor(4, BODY_Y + 22); d.printf("target %02X:%02X:%02X:%02X:%02X:%02X",
        g_ble_target.addr[0], g_ble_target.addr[1], g_ble_target.addr[2],
        g_ble_target.addr[3], g_ble_target.addr[4], g_ble_target.addr[5]);
    d.setTextColor(T_WARN, T_BG);
    d.setCursor(4, BODY_Y + 36); d.print("connecting...");
    ui_draw_footer("`=abort");
    ui_draw_status(radio_name(), "gatt");

    NimBLEAddress addr(g_ble_target.addr, g_ble_target.is_public ? BLE_ADDR_PUBLIC : BLE_ADDR_RANDOM);
    if (!try_connect(addr)) {
        d.fillRect(0, BODY_Y + 36, SCR_W, 16, T_BG);
        d.setTextColor(T_BAD, T_BG);
        d.setCursor(4, BODY_Y + 36); d.print("CONNECT FAILED");
        ui_draw_footer("`=back");
        while (true) {
            uint16_t k = input_poll();
            if (k == PK_NONE) { delay(50); continue; }
            if (k == PK_ESC) break;
        }
        if (s_client) s_client->disconnect();
        return;
    }

    d.fillRect(0, BODY_Y + 36, SCR_W, 16, T_BG);
    d.setTextColor(T_GOOD, T_BG);
    d.setCursor(4, BODY_Y + 36); d.print("connected, enumerating...");
    enumerate_services();

    int cursor = 0;
    ui_draw_footer(";/.=move  ENTER=open  `=back");
    while (true) {
        draw_tree(cursor);
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(40); continue; }
        if (k == PK_ESC) break;
        if (k == ';' || k == PK_UP)   { if (cursor > 0) cursor--; }
        if (k == '.' || k == PK_DOWN) { if (cursor + 1 < s_flat_n) cursor++; }
        if (k == '?') {
            ui_show_current_help();
            ui_draw_footer(";/.=move  ENTER=open  `=back");
        }
        if (k == PK_ENTER) {
            if (cursor < s_flat_n && !s_flat[cursor].is_svc) {
                show_characteristic(cursor);
                ui_draw_footer(";/.=move  ENTER=open  `=back");
            }
        }
    }

    s_client->disconnect();
    s_connected = false;
}
