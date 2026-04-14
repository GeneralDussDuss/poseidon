/*
 * ble_clone — rebroadcast a scanned device's identity.
 *
 * Sets the adapter's random MAC to the target's MAC, advertises with
 * the target's name + a generic flags byte. Target devices elsewhere
 * in range may see two "instances" of the same MAC, causing dedup
 * collisions and pairing prompts on subscribers.
 *
 * Requires g_ble_target_valid (set by ENTER on a scan result).
 */
#include "app.h"
#include "ui.h"
#include "input.h"
#include "radio.h"
#include "ble_types.h"
#include <NimBLEDevice.h>

void feat_ble_clone(void)
{
    if (!g_ble_target_valid) {
        ui_toast("scan + select first", COL_WARN, 1200);
        return;
    }
    radio_switch(RADIO_BLE);

    /* Try to set the random static address. NimBLE-Arduino doesn't
     * expose setRandomAddr directly; use the ble_hs API. */
    uint8_t mac[6];
    /* Copy target MAC (NimBLE uses little-endian internally). */
    for (int i = 0; i < 6; ++i) mac[i] = g_ble_target.addr[5 - i];
    mac[5] |= 0xC0;  /* static random bits */
    ble_hs_id_set_rnd(mac);

    NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_RANDOM);

    NimBLEAdvertisementData data;
    if (g_ble_target.name[0]) data.setName(g_ble_target.name);
    uint8_t flags = 0x06;
    std::string flags_str;
    flags_str.push_back((char)0x02);
    flags_str.push_back((char)0x01);
    flags_str.push_back((char)flags);
    data.addData(flags_str);

    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    adv->setAdvertisementData(data);
    adv->setAdvertisementType(BLE_GAP_CONN_MODE_NON);
    adv->setMinInterval(0x20);
    adv->setMaxInterval(0x30);
    adv->start();

    ui_clear_body();
    auto &d = M5Cardputer.Display;
    d.setTextColor(COL_BAD, COL_BG);
    d.setCursor(4, BODY_Y + 2); d.print("BLE CLONE");
    d.drawFastHLine(4, BODY_Y + 12, 80, COL_BAD);
    d.setTextColor(COL_FG, COL_BG);
    d.setCursor(4, BODY_Y + 22);
    d.printf("MAC : %02X:%02X:%02X:%02X:%02X:%02X",
             g_ble_target.addr[0], g_ble_target.addr[1], g_ble_target.addr[2],
             g_ble_target.addr[3], g_ble_target.addr[4], g_ble_target.addr[5]);
    d.setCursor(4, BODY_Y + 34); d.printf("NAME: %.30s",
        g_ble_target.name[0] ? g_ble_target.name : "(none)");
    d.setTextColor(COL_GOOD, COL_BG);
    d.setCursor(4, BODY_Y + 54); d.print("BROADCASTING");

    ui_draw_footer("`=stop");
    ui_draw_status(radio_name(), "clone");

    uint32_t start = millis();
    while (true) {
        if ((millis() - start) / 400 % 2 == 0) {
            d.fillRect(120, BODY_Y + 54, 20, 10, COL_BG);
            d.setTextColor(COL_GOOD, COL_BG);
            d.setCursor(120, BODY_Y + 54); d.print("...");
        } else {
            d.fillRect(120, BODY_Y + 54, 20, 10, COL_BG);
        }
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(40); continue; }
        if (k == PK_ESC) break;
    }
    adv->stop();
}
