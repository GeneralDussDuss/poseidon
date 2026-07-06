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
#include <host/ble_gap.h>

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

/* Cooperative tick — caller polls each UI iteration. Rotates identity
 * every `dwell` ms (60s SINGLE, 3s FLOCK) and increments the visible
 * counter at ~1 Hz between rotations. Replaces the broken xTaskCreate
 * path (failed silently rc=-1 because NimBLE init left heap ~2.5 KB,
 * not enough for a 4 KB task stack — same bug as Sour Apple was). */
static void fm_tick(void)
{
    static uint32_t next_rotate = 0;
    static uint32_t last_count_bump = 0;
    /* Fetch fresh each tick: a cached singleton dangles across a BLE
     * deinit/reinit (radio_switch away and back) -> UAF. */
    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    if (!adv) return;

    uint32_t now = millis();
    if (now >= next_rotate) {
        uint8_t key[22];
        uint8_t mac[6];
        for (int i = 0; i < 22; ++i) key[i] = (uint8_t)esp_random();
        for (int i = 0; i < 6;  ++i) mac[i] = (uint8_t)esp_random();
        /* Static-random address flag bits MUST be 11. Original code
         * borrowed the top 2 bits from key[0] which were random — 75%
         * of the time the address ended up resolvable-private or
         * non-resolvable-private flagged, which the controller can
         * silently reject. Force them to 11 unconditionally. */
        mac[5] = (mac[5] & 0x3F) | 0xC0;
        ble_hs_id_set_rnd(mac);
        NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_RANDOM);
        s_fm_rotated++;

        uint8_t pkt[31];   /* builder writes idx 0..30 = 31 bytes (0x1E len + 30 following) */
        build_findmy(pkt, key, 0xE0 /* OWNED */, 0x00);

        adv->stop();
        /* Direct IDF call — NimBLE-Arduino's setAdvertisementData wrapper
         * rejects Apple-mfr-ID packets (0x004C). Verified 2026-05-24 via
         * Sour Apple where the same wrapper returned false for all Apple
         * subtypes, leaving stale data on-air. Apple Find My uses 0x4C 0x12
         * subtype which would hit the same wrapper bug. */
        int data_rc = ble_gap_adv_set_data(pkt, 31);
        (void)data_rc;
        adv->setConnectableMode(BLE_GAP_CONN_MODE_NON);
        adv->setMinInterval(0x0640);
        adv->setMaxInterval(0x0780);
        adv->start();

        uint32_t dwell = (s_fm_tags <= 1) ? 60000 : 3000;
        next_rotate = now + dwell;
    }
    if (now - last_count_bump >= 1000) {
        last_count_bump = now;
        s_fm_count++;
    }
}

static void fm_teardown(void)
{
    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    if (adv) adv->stop();
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

    ui_clear_body();
    d.setTextColor(T_BAD, T_BG);
    d.setCursor(4, BODY_Y + 2);
    d.printf("FIND MY x%d", s_fm_tags);
    d.drawFastHLine(4, BODY_Y + 12, 130, T_BAD);
    ui_draw_footer("`=stop");
    d.setTextColor(T_DIM, T_BG);
    d.setCursor(4, BODY_Y + 48);
    d.print("passing iPhones will relay");
    d.setCursor(4, BODY_Y + 58);
    d.print("these to icloud location svc");

    uint32_t last = 0;
    uint32_t last_count = 0xFFFFFFFF, last_rot = 0xFFFFFFFF;
    while (true) {
        if (millis() - last > 300) {
            last = millis();
            if (s_fm_count != last_count) {
                ui_text_w(4, BODY_Y + 20, 150, T_FG, "broadcasts: %lu", (unsigned long)s_fm_count);
                last_count = s_fm_count;
            }
            if (s_fm_rotated != last_rot) {
                ui_text_w(4, BODY_Y + 32, 150, T_GOOD, "identities: %lu", (unsigned long)s_fm_rotated);
                last_rot = s_fm_rotated;
            }
            ui_draw_status(radio_name(), "findmy");
        }
        ui_matrix_rain(160, BODY_Y + 18, SCR_W - 160, BODY_H - 20, 0xF81F);
        if (s_fm_alive) fm_tick();
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(40); continue; }
        if (k == PK_ESC) break;
    }
    s_fm_alive = false;
    fm_teardown();
}
