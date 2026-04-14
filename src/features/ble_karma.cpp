/*
 * ble_karma — BLE equivalent of WiFi Karma.
 *
 * Passively scans for ACTIVE scan requests from phones/devices. When
 * we see one, the device is asking "anyone here?" — we respond by
 * cycling our SoftAP-style BLE advertisement through the names of
 * devices the target has shown interest in.
 *
 * Implementation is simpler than WiFi karma because BLE probes
 * (scan requests) don't carry a specific SSID — they're per-address.
 * What we CAN do:
 *   1. Listen for incoming scan requests via active scan callback
 *   2. Cycle through a rotating list of popular device names
 *      (AirPods, Galaxy Buds, Samsung TV, etc.) at ~2s each
 *   3. Any device that's actively scanning for one of these will
 *      get a matching response within one cycle.
 *
 * Rogue-AP style attack on unpaired nearby devices that might
 * auto-pair with known names.
 */
#include "app.h"
#include "ui.h"
#include "input.h"
#include "radio.h"
#include <NimBLEDevice.h>
#include <esp_random.h>

static const char *KARMA_NAMES[] = {
    "AirPods Pro", "AirPods", "AirPods Max", "Beats Fit Pro",
    "Galaxy Buds Pro", "Galaxy Buds2", "Galaxy Watch",
    "Bose QC45", "JBL Flip 6", "Sony WH-1000XM5",
    "Magic Keyboard", "Logitech MX", "Samsung TV",
    "Fitbit Versa", "Garmin Fenix", "Pixel Buds",
};
#define KARMA_N (sizeof(KARMA_NAMES)/sizeof(KARMA_NAMES[0]))

static volatile uint32_t s_karma_requests = 0;
static volatile uint32_t s_karma_responses = 0;
static volatile int      s_karma_idx = 0;
static char              s_karma_current[32] = "";

static void random_mac(void)
{
    uint8_t mac[6];
    for (int i = 0; i < 6; ++i) mac[i] = (uint8_t)esp_random();
    mac[0] |= 0xC0;
    ble_hs_id_set_rnd(mac);
}

class karma_cb : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice *d) override {
        (void)d;
        /* Any advertisement or scan request counts as "somebody out
         * there probing" for our signal-loudness meter. */
        s_karma_requests++;
    }
};
static karma_cb s_cb;

void feat_ble_karma(void)
{
    radio_switch(RADIO_BLE);
    s_karma_requests = 0;
    s_karma_responses = 0;

    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->setAdvertisedDeviceCallbacks(&s_cb, true);
    scan->setActiveScan(false);
    scan->setInterval(45);
    scan->setWindow(30);
    /* Short continuous scan so we can sense the air. */
    scan->start(0, nullptr, false);

    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();

    ui_clear_body();
    auto &d = M5Cardputer.Display;
    d.setTextColor(0xF81F, COL_BG);
    d.setCursor(4, BODY_Y + 2); d.print("BLE KARMA");
    d.drawFastHLine(4, BODY_Y + 12, 80, 0xF81F);
    ui_draw_footer("`=stop");
    ui_draw_status(radio_name(), "karma");

    uint32_t last_cycle = 0;
    uint32_t last_draw = 0;

    while (true) {
        uint32_t now = millis();

        /* Every 2s swap to the next decoy name and restart advertising. */
        if (now - last_cycle > 2000) {
            last_cycle = now;
            const char *name = KARMA_NAMES[s_karma_idx % KARMA_N];
            s_karma_idx++;
            strncpy(s_karma_current, name, sizeof(s_karma_current) - 1);
            s_karma_current[sizeof(s_karma_current) - 1] = '\0';

            adv->stop();
            random_mac();
            NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_RANDOM);

            NimBLEAdvertisementData data;
            data.setName(name);
            data.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
            adv->setAdvertisementData(data);
            adv->setAdvertisementType(BLE_GAP_CONN_MODE_UND);
            adv->start();
            s_karma_responses++;
        }

        if (now - last_draw > 200) {
            last_draw = now;
            d.fillRect(0, BODY_Y + 18, 150, 70, COL_BG);
            d.setTextColor(COL_ACCENT, COL_BG);
            d.setCursor(4, BODY_Y + 18); d.printf("posing as:");
            d.setTextColor(0xF81F, COL_BG);
            d.setCursor(4, BODY_Y + 30); d.printf("  %.24s", s_karma_current);
            d.setTextColor(COL_FG, COL_BG);
            d.setCursor(4, BODY_Y + 48); d.printf("seen:     %lu", (unsigned long)s_karma_requests);
            d.setTextColor(COL_GOOD, COL_BG);
            d.setCursor(4, BODY_Y + 60); d.printf("spoofed:  %lu", (unsigned long)s_karma_responses);
            d.setTextColor(COL_DIM, COL_BG);
            d.setCursor(4, BODY_Y + 76); d.print("rotating identity every 2s");
        }
        /* Magenta matrix rain in the gutter. */
        ui_matrix_rain(160, BODY_Y + 18, SCR_W - 160, BODY_H - 20, 0xF81F);

        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(30); continue; }
        if (k == PK_ESC) break;
    }

    scan->stop();
    adv->stop();
}
