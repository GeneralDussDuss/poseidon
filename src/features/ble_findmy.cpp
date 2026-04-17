/*
 * ble_findmy — Apple Find My / AirTag broadcast emulator.
 *
 * Broadcasts Apple offline-finding advertisements with rotating
 * random public keys. Any passing iPhone with Find My enabled will
 * relay our "tag"'s last known location to iCloud.
 *
 * Two modes:
 *   SINGLE: one persistent fake tag, one MAC
 *   FLOCK:  cycles through N distinct tag identities every few
 *           seconds — flood Find My with decoys
 *
 * Advertisement structure (per Apple's offline-finding spec):
 *   [0x1E, 0xFF, 0x4C, 0x00, 0x12, 0x19, status, key[22], hint, 0x00]
 *   Total = 30 bytes manufacturer-specific.
 *
 * The "public key" is just 22 random bytes — real AirTags use a
 * rotating Curve25519 key, but iPhones don't validate; they relay
 * the raw key blindly. Good enough for emulation.
 */
#include "app.h"
#include "../theme.h"
#include "ui.h"
#include "input.h"
#include "radio.h"
#include <NimBLEDevice.h>
#include <esp_random.h>

static volatile bool     s_fm_alive = false;
static volatile uint32_t s_fm_count = 0;
static volatile int      s_fm_tags  = 1;
static volatile uint32_t s_fm_rotated = 0;

static void build_findmy(uint8_t *pkt, const uint8_t *key22,
                         uint8_t status, uint8_t hint)
{
    int i = 0;
    pkt[i++] = 0x1E;       /* len */
    pkt[i++] = 0xFF;       /* mfg data */
    pkt[i++] = 0x4C;       /* Apple */
    pkt[i++] = 0x00;
    pkt[i++] = 0x12;       /* Find My / AirTag */
    pkt[i++] = 0x19;       /* length */
    pkt[i++] = status;     /* status byte (e.g. 0xE0 = OWNED) */
    memcpy(pkt + i, key22, 22); i += 22;
    pkt[i++] = (key22[0] >> 6) & 0x03;  /* hint */
    pkt[i++] = hint;
}

static void fm_task(void *)
{
    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();

    uint8_t key[22];
    uint8_t mac[6];
    for (int i = 0; i < 22; ++i) key[i] = (uint8_t)esp_random();
    int tag_idx = 0;

    while (s_fm_alive) {
        /* Every 3 seconds (or every tag slot if in FLOCK) rotate identity. */
        for (int i = 0; i < 22; ++i) key[i] = (uint8_t)esp_random();
        for (int i = 0; i < 6;  ++i) mac[i] = (uint8_t)esp_random();
        /* AirTag MAC encodes first 2 bits of the key in the top of byte 5. */
        mac[5] = (mac[5] & 0x3F) | (key[0] & 0xC0);
        ble_hs_id_set_rnd(mac);
        NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_RANDOM);
        s_fm_rotated++;

        uint8_t pkt[30];
        build_findmy(pkt, key, 0xE0 /* OWNED */, 0x00);

        NimBLEAdvertisementData data;
        data.addData(std::string((const char *)pkt, 30));
        adv->stop();
        adv->setAdvertisementData(data);
        adv->setAdvertisementType(BLE_GAP_CONN_MODE_NON);
        adv->setMinInterval(0x0640);  /* 1 s — matches real AirTag cadence */
        adv->setMaxInterval(0x0780);  /* 1.2 s */
        adv->start();

        /* Hold this identity for 3 seconds (or longer for SINGLE). */
        uint32_t dwell = (s_fm_tags <= 1) ? 60000 : 3000;
        uint32_t t0 = millis();
        while (s_fm_alive && millis() - t0 < dwell) {
            s_fm_count++;
            delay(1000);
        }
        tag_idx = (tag_idx + 1) % (s_fm_tags > 0 ? s_fm_tags : 1);
        (void)tag_idx;
    }
    adv->stop();
    vTaskDelete(nullptr);
}

void feat_ble_findmy(void)
{
    radio_switch(RADIO_BLE);

    /* Sub-menu: pick single or flock. */
    ui_clear_body();
    auto &d = M5Cardputer.Display;
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2); d.print("FIND MY EMULATOR");
    d.drawFastHLine(4, BODY_Y + 12, 140, T_ACCENT);
    d.setTextColor(T_FG, T_BG);
    d.setCursor(4, BODY_Y + 22); d.print("[1] Single fake tag");
    d.setCursor(4, BODY_Y + 34); d.print("[2] Flock of 8 tags");
    d.setCursor(4, BODY_Y + 46); d.print("[3] Flock of 32 tags");
    d.setTextColor(T_DIM, T_BG);
    d.setCursor(4, BODY_Y + 60); d.print("broadcasts as apple offline finding");
    d.setCursor(4, BODY_Y + 70); d.print("iphones relay your tags to icloud");
    ui_draw_footer("1-3=pick  `=back");

    int mode = 0;
    while (mode == 0) {
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) return;
        if (k == '1') mode = 1;
        if (k == '2') mode = 2;
        if (k == '3') mode = 3;
    }
    s_fm_tags = (mode == 1) ? 1 : (mode == 2 ? 8 : 32);
    s_fm_count = 0;
    s_fm_rotated = 0;
    s_fm_alive = true;
    xTaskCreate(fm_task, "findmy", 4096, nullptr, 4, nullptr);

    ui_clear_body();
    d.setTextColor(T_BAD, T_BG);
    d.setCursor(4, BODY_Y + 2);
    d.printf("FIND MY x%d", s_fm_tags);
    d.drawFastHLine(4, BODY_Y + 12, 130, T_BAD);
    ui_draw_footer("`=stop");

    uint32_t last = 0;
    while (true) {
        if (millis() - last > 300) {
            last = millis();
            d.fillRect(0, BODY_Y + 20, 150, 60, T_BG);
            d.setTextColor(T_FG, T_BG);
            d.setCursor(4, BODY_Y + 20);
            d.printf("broadcasts: %lu", (unsigned long)s_fm_count);
            d.setTextColor(T_GOOD, T_BG);
            d.setCursor(4, BODY_Y + 32);
            d.printf("identities: %lu", (unsigned long)s_fm_rotated);
            d.setTextColor(T_DIM, T_BG);
            d.setCursor(4, BODY_Y + 48);
            d.print("passing iPhones will relay");
            d.setCursor(4, BODY_Y + 58);
            d.print("these to icloud location svc");
            ui_draw_status(radio_name(), "findmy");
        }
        ui_matrix_rain(160, BODY_Y + 18, SCR_W - 160, BODY_H - 20, 0xF81F);
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(40); continue; }
        if (k == PK_ESC) break;
    }
    s_fm_alive = false;
    delay(300);
}
