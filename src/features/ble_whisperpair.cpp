/*
 * ble_whisperpair — CVE-2025-36911 detector.
 *
 * Passive-first scanner for Google Fast Pair accessories that identifies
 * devices vulnerable to the WhisperPair attack, where a Fast Pair provider
 * honors Key-Based Pairing (KBP) requests outside of pairing mode.
 *
 * Protocol primer
 * ---------------
 *   Service UUID:   0xFE2C  (Google Fast Pair)
 *   Advertisement:
 *     - 3 bytes  service data           → DISCOVERABLE (in pairing mode)
 *                                         [24-bit big-endian model ID]
 *     - 4+ bytes service data, 1st=0x00 → NON-DISCOVERABLE (in use)
 *                                         [version || LT || account filter || salt]
 *   KBP char:       FE2C1234-8366-4814-8EB0-01DE32100BEA  (Write + Notify)
 *
 * Spec, FastPair Provider requirements, §Key-based Pairing:
 *   "If the device is not in pairing mode, ignore the write and exit."
 *
 * Vulnerable firmware lacks that guard — it will still decrypt (or at
 * least respond with an error notify) to a KBP write on the GATT surface
 * even though advertising has left pairing mode. A compliant accessory
 * silently drops the write and disconnects.
 *
 * What this module does
 * ---------------------
 *   1. BLE scan for service-data UUID 0xFE2C, classify each hit.
 *   2. Optionally let user pick a non-discoverable (interesting) target.
 *   3. Connect, discover FE2C service, locate KBP characteristic.
 *   4. Subscribe for notify, then write a deliberately bogus 80-byte blob
 *      (16B ciphertext placeholder || 64B random "ephemeral pubkey").
 *      We do NOT carry out real ECDH — we rely on DIY_WhisperPair's
 *      observation that vulnerable firmware still responds (even with a
 *      malformed-response notify) rather than silent drop.
 *   5. 3-second notify window → VULNERABLE if any notify received,
 *      LIKELY PATCHED if silent drop.
 *   6. Log the verdict + model ID + BLE addr to SD as CSV.
 *
 * We do NOT complete the attack. The ESP32-S3 has no Classic BT radio;
 * the BR/EDR bond + A2DP/HFP hijack stages need external hardware
 * (original ESP32, Pi, laptop). This module is a CVE demonstration +
 * "is my gear vulnerable?" check — nothing more.
 *
 * Credits
 * -------
 *   CVE-2025-36911 disclosed by COSIC @ KU Leuven (Preneel, Singelée,
 *   Antonijević, Duttagupta, Wyns), Jan 2026. PoC references:
 *   aalex954/whisperpair-poc-tool, SpectrixDev/DIY_WhisperPair. Model-ID
 *   mirror: DiamondRoPlayz/FastPair-Models.
 */
#include "app.h"
#include "../theme.h"
#include "ui.h"
#include "input.h"
#include "radio.h"
#include "ble_types.h"
#include "sd_helper.h"
#include <NimBLEDevice.h>
#include <SD.h>
#include <esp_random.h>

#define WP_MAX_TARGETS    24
#define WP_FE2C_UUID      "0000fe2c-0000-1000-8000-00805f9b34fb"
#define WP_KBP_UUID       "fe2c1234-8366-4814-8eb0-01de32100bea"
#define WP_PROBE_WAIT_MS  3000

enum wp_mode_t { WP_MODE_DISCOVERABLE, WP_MODE_NONDISCOVERABLE, WP_MODE_UNKNOWN };

struct wp_target_t {
    uint8_t  addr[6];
    uint8_t  addr_type;
    int8_t   rssi;
    wp_mode_t mode;
    uint32_t model_id;    /* 24-bit, only valid if DISCOVERABLE */
    uint8_t  sd_len;      /* raw service-data length */
};

static wp_target_t s_tgt[WP_MAX_TARGETS];
static volatile int s_tgt_n = 0;

/* --- Model ID lookup ---------------------------------------------------
 * Subset of the public Fast Pair model ID table. Unknown IDs fall back to
 * "0xXXXXXX" display. Keep the table small — just the devices casual
 * users recognize. The full table is maintained at
 * https://github.com/DiamondRoPlayz/FastPair-Models. */
struct wp_model_t { uint32_t id; const char *name; };
static const wp_model_t s_models[] = {
    { 0x00000D, "Pixel Buds A-Series" },
    { 0x0002F0, "Pixel Buds"          },
    { 0x000A70, "Pixel Buds Pro"      },
    { 0x4E8CE2, "Sony WH-1000XM4"     },
    { 0x55D500, "Sony WH-1000XM5"     },
    { 0x2A3964, "Sony LinkBuds S"     },
    { 0x4A4E9B, "Jabra Elite 85t"     },
    { 0x4A9C45, "Jabra Elite 75t"     },
    { 0xB35BDF, "JBL Flip 5"          },
    { 0x0AE2F5, "JBL Tune 230NC"      },
    { 0xD9B4D3, "Marshall Emberton"   },
    { 0x01EA04, "OnePlus Buds Pro"    },
    { 0x1FAF06, "Nothing Ear (a)"     },
    { 0x5E35FE, "Anker Soundcore"     },
    { 0x0BB8E2, "Logitech Zone"       },
};

static const char *model_name(uint32_t id)
{
    for (size_t i = 0; i < sizeof(s_models) / sizeof(s_models[0]); ++i)
        if (s_models[i].id == id) return s_models[i].name;
    return nullptr;
}

/* --- Scan callback ----------------------------------------------------- */
class wp_scan_cb : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice *d) override {
        if (!d->haveServiceData()) return;
        NimBLEUUID fp(WP_FE2C_UUID);
        std::string sd = d->getServiceData(fp);
        if (sd.empty()) return;

        const uint8_t *a = d->getAddress().getBase()->val;

        /* Dedup by MAC. */
        for (int i = 0; i < s_tgt_n; ++i) {
            if (memcmp(s_tgt[i].addr, a, 6) == 0) {
                s_tgt[i].rssi = d->getRSSI();
                return;
            }
        }
        if (s_tgt_n >= WP_MAX_TARGETS) return;

        wp_target_t &t = s_tgt[s_tgt_n];
        memcpy(t.addr, a, 6);
        t.addr_type = d->getAddressType();
        t.rssi = d->getRSSI();
        t.sd_len = (uint8_t)sd.size();
        t.model_id = 0;

        if (sd.size() == 3) {
            /* Discoverable: 24-bit model ID big-endian. */
            t.mode = WP_MODE_DISCOVERABLE;
            t.model_id = ((uint32_t)(uint8_t)sd[0] << 16) |
                         ((uint32_t)(uint8_t)sd[1] << 8)  |
                          (uint32_t)(uint8_t)sd[2];
        } else if (sd.size() >= 4 && (uint8_t)sd[0] == 0x00) {
            /* Non-discoverable: version&flags == 0x00, account-key filter follows. */
            t.mode = WP_MODE_NONDISCOVERABLE;
        } else {
            t.mode = WP_MODE_UNKNOWN;
        }
        s_tgt_n++;
    }
};
static wp_scan_cb s_cb;

/* --- Probe state ------------------------------------------------------- */
static volatile bool s_notify_received = false;
static volatile int  s_notify_len      = 0;
static uint8_t       s_notify_buf[32];

static void kbp_notify_cb(NimBLERemoteCharacteristic *chr, uint8_t *data,
                          size_t len, bool isNotify)
{
    (void)chr; (void)isNotify;
    s_notify_received = true;
    s_notify_len = (int)len;
    size_t n = len < sizeof(s_notify_buf) ? len : sizeof(s_notify_buf);
    memcpy(s_notify_buf, data, n);
}

enum wp_verdict_t { WP_VULNERABLE, WP_PATCHED, WP_NO_SERVICE, WP_CONNECT_FAIL };

static wp_verdict_t run_probe(const wp_target_t &t)
{
    NimBLEClient *c = NimBLEDevice::createClient();
    c->setConnectTimeout(6);

    NimBLEAddress addr((uint8_t *)t.addr, t.addr_type);
    if (!c->connect(addr)) {
        NimBLEDevice::deleteClient(c);
        return WP_CONNECT_FAIL;
    }

    NimBLEUUID svc_uuid(WP_FE2C_UUID);
    NimBLEUUID kbp_uuid(WP_KBP_UUID);
    NimBLERemoteService *svc = c->getService(svc_uuid);
    if (!svc) { c->disconnect(); NimBLEDevice::deleteClient(c); return WP_NO_SERVICE; }

    NimBLERemoteCharacteristic *kbp = svc->getCharacteristic(kbp_uuid);
    if (!kbp || !kbp->canWrite()) {
        c->disconnect();
        NimBLEDevice::deleteClient(c);
        return WP_NO_SERVICE;
    }

    /* Arm notify listener. */
    s_notify_received = false;
    s_notify_len = 0;
    if (kbp->canNotify()) kbp->subscribe(true, kbp_notify_cb);

    /* Build deliberately bogus 80-byte KBP blob:
     *   [0..15]  random "ciphertext"    — no valid account key, no ECDH
     *   [16..79] random 64-byte pubkey  — not a real secp256r1 point
     * Compliant provider: AES decrypt fails → silent drop.
     * Vulnerable provider: still sends a notify (error or malformed response)
     *                      because the pairing-mode gate is missing. */
    uint8_t blob[80];
    for (int i = 0; i < 80; ++i) blob[i] = (uint8_t)esp_random();

    bool wrote = kbp->writeValue(blob, 80, false);
    if (!wrote) {
        /* Some stacks fail write-without-response if MTU too small; retry with response. */
        kbp->writeValue(blob, 80, true);
    }

    uint32_t start = millis();
    while (millis() - start < WP_PROBE_WAIT_MS) {
        if (s_notify_received) break;
        delay(50);
    }

    bool vuln = s_notify_received;
    if (kbp->canNotify()) kbp->unsubscribe();
    c->disconnect();
    NimBLEDevice::deleteClient(c);

    return vuln ? WP_VULNERABLE : WP_PATCHED;
}

/* --- Logging ----------------------------------------------------------- */
static void log_verdict(const wp_target_t &t, wp_verdict_t v)
{
    if (!sd_mount()) return;
    SD.mkdir("/poseidon");
    File f = SD.open("/poseidon/whisperpair.csv", FILE_APPEND);
    if (!f) return;
    const char *v_str = (v == WP_VULNERABLE)   ? "VULNERABLE"
                      : (v == WP_PATCHED)      ? "PATCHED"
                      : (v == WP_NO_SERVICE)   ? "NO_FE2C_SVC"
                                               : "CONNECT_FAIL";
    const char *m_str = (t.mode == WP_MODE_DISCOVERABLE)    ? "pairable"
                      : (t.mode == WP_MODE_NONDISCOVERABLE) ? "in-use"
                                                            : "unknown";
    const char *name = model_name(t.model_id);
    char line[160];
    snprintf(line, sizeof(line),
             "%lu,%02X:%02X:%02X:%02X:%02X:%02X,%s,0x%06lX,%s,%s,%d\n",
             (unsigned long)millis(),
             t.addr[5], t.addr[4], t.addr[3], t.addr[2], t.addr[1], t.addr[0],
             m_str, (unsigned long)t.model_id,
             name ? name : "unknown",
             v_str, (int)t.rssi);
    f.print(line);
    f.close();
    Serial.printf("[wp] %s", line);
}

/* --- UI ---------------------------------------------------------------- */
static void draw_picker(int cursor, bool scanning)
{
    auto &d = M5Cardputer.Display;
    ui_clear_body();
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2); d.print("WHISPERPAIR  CVE-2025-36911");
    d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT);

    if (s_tgt_n == 0) {
        d.setTextColor(T_DIM, T_BG);
        d.setCursor(4, BODY_Y + 24);
        d.print(scanning ? "scanning FE2C..." : "no Fast Pair devices");
        d.setCursor(4, BODY_Y + 38); d.print("airpods / buds / cans");
        d.setCursor(4, BODY_Y + 48); d.print("in range = shown here");
        return;
    }

    int rows = 7;
    if (cursor < 0) cursor = 0;
    if (cursor >= s_tgt_n) cursor = s_tgt_n - 1;
    int first = cursor - rows / 2;
    if (first < 0) first = 0;
    if (first + rows > s_tgt_n) first = max(0, s_tgt_n - rows);

    for (int r = 0; r < rows && first + r < s_tgt_n; ++r) {
        const wp_target_t &t = s_tgt[first + r];
        int y = BODY_Y + 18 + r * 12;
        bool sel = (first + r == cursor);
        uint16_t bg = sel ? 0x3007 : T_BG;
        if (sel) d.fillRect(0, y - 1, SCR_W, 12, bg);

        /* Mode tag. */
        uint16_t tag_col = (t.mode == WP_MODE_NONDISCOVERABLE) ? T_WARN : T_DIM;
        d.setTextColor(tag_col, bg);
        d.setCursor(4, y);
        d.print(t.mode == WP_MODE_DISCOVERABLE    ? "PAIR"
              : t.mode == WP_MODE_NONDISCOVERABLE ? "USE "
                                                  : "???");

        /* Name. */
        const char *nm = model_name(t.model_id);
        d.setTextColor(sel ? 0xFFFF : T_FG, bg);
        d.setCursor(34, y);
        if (nm) d.printf("%.22s", nm);
        else if (t.mode == WP_MODE_DISCOVERABLE)
             d.printf("0x%06lX",      (unsigned long)t.model_id);
        else d.printf("%02X:%02X:%02X:%02X", t.addr[5], t.addr[4], t.addr[3], t.addr[2]);

        d.setTextColor(T_DIM, bg);
        d.setCursor(SCR_W - 28, y);
        d.printf("%4d", t.rssi);
    }
}

static void draw_probing(const wp_target_t &t)
{
    auto &d = M5Cardputer.Display;
    ui_clear_body();
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2); d.print("PROBING");
    d.drawFastHLine(4, BODY_Y + 12, 60, T_ACCENT);

    d.setTextColor(T_FG, T_BG);
    d.setCursor(4, BODY_Y + 20);
    const char *nm = model_name(t.model_id);
    d.printf("target: %s", nm ? nm : "unknown model");

    d.setCursor(4, BODY_Y + 32);
    d.printf("mac: %02X:%02X:%02X:%02X:%02X:%02X",
             t.addr[5], t.addr[4], t.addr[3], t.addr[2], t.addr[1], t.addr[0]);

    d.setTextColor(T_DIM, BODY_Y);
    d.setCursor(4, BODY_Y + 46); d.print("connect -> FE2C -> KBP");
    d.setCursor(4, BODY_Y + 56); d.print("write bogus 80B -> listen 3s");
}

static void draw_verdict(const wp_target_t &t, wp_verdict_t v)
{
    auto &d = M5Cardputer.Display;
    ui_clear_body();
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2); d.print("VERDICT");
    d.drawFastHLine(4, BODY_Y + 12, 60, T_ACCENT);

    uint16_t col = (v == WP_VULNERABLE) ? T_BAD
                : (v == WP_PATCHED)     ? T_GOOD
                                        : T_WARN;
    const char *txt = (v == WP_VULNERABLE) ? "VULNERABLE"
                    : (v == WP_PATCHED)    ? "LIKELY PATCHED"
                    : (v == WP_NO_SERVICE) ? "NO FE2C SVC"
                                           : "CONNECT FAIL";

    d.setTextSize(2);
    d.setTextColor(col, T_BG);
    d.setCursor(4, BODY_Y + 22);
    d.print(txt);
    d.setTextSize(1);

    d.setTextColor(T_FG, T_BG);
    const char *nm = model_name(t.model_id);
    d.setCursor(4, BODY_Y + 50);
    if (nm) d.printf("%s", nm);
    else    d.printf("Model 0x%06lX", (unsigned long)t.model_id);

    d.setTextColor(T_DIM, T_BG);
    d.setCursor(4, BODY_Y + 62);
    d.printf("notify=%dB  rssi=%d", s_notify_len, t.rssi);
    d.setCursor(4, BODY_Y + 72);
    d.print("logged to /poseidon/whisperpair.csv");
}

/* --- Entry point ------------------------------------------------------- */
void feat_ble_whisperpair(void)
{
    radio_switch(RADIO_BLE);
    s_tgt_n = 0;

    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->setScanCallbacks(&s_cb, true);
    scan->setActiveScan(true);
    scan->setInterval(45);
    scan->setWindow(30);
    scan->start(0, false);

    ui_draw_footer(";/.=move  ENTER=probe  L=log  `=back");
    ui_draw_status(radio_name(), "whisper");
    int cursor = 0;
    uint32_t last = 0;
    while (true) {
        if (millis() - last > 300) { last = millis(); draw_picker(cursor, true); }
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) { scan->stop(); return; }
        if ((k == ';' || k == PK_UP)   && cursor > 0) cursor--;
        if ((k == '.' || k == PK_DOWN) && cursor + 1 < s_tgt_n) cursor++;

        if (k == PK_ENTER && s_tgt_n > 0) {
            scan->stop();
            wp_target_t t = s_tgt[cursor];
            draw_probing(t);
            wp_verdict_t v = run_probe(t);
            log_verdict(t, v);
            draw_verdict(t, v);
            ui_draw_footer("any key = back to list");
            while (true) {
                uint16_t kk = input_poll();
                if (kk != PK_NONE) break;
                delay(30);
            }
            scan->start(0, false);
            ui_draw_footer(";/.=move  ENTER=probe  L=log  `=back");
        }
    }
}
