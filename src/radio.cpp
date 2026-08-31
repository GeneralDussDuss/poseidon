/*
 * radio.cpp — lazy domain switcher.
 */
#include "radio.h"
#include "lora_hw.h"
#include "cc1101_hw.h"
#include "nrf24_hw.h"
#include "gps.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <esp_bt.h>
#include <NimBLEDevice.h>

static radio_domain_t s_active = RADIO_NONE;

void wifi_force_clean_sta(void)
{
    /* Probe whether WiFi was ever inited this session. esp_wifi_get_mode
     * returns ESP_ERR_WIFI_NOT_INIT until esp_wifi_init has run; in
     * that case there's nothing to reset and the subsequent IDF calls
     * would themselves return errors and leave the driver in a
     * half-state that breaks Arduino's later WiFi.mode() init. */
    wifi_mode_t cur = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&cur) != ESP_OK) return;
    esp_wifi_set_promiscuous(false);
    esp_wifi_disconnect();
    esp_wifi_set_mode(WIFI_MODE_STA);
    delay(30);
}

bool wifi_lean_sta_init(void)
{
    /* If WiFi is already inited (we got here from another feature in
     * the same session), just ensure mode is STA + started.
     * esp_wifi_get_mode returns ESP_OK only after esp_wifi_init has run.
     *
     * On-device repro fix 2026-06-06: after POS-AUDIT-008 the
     * teardown(RADIO_WIFI) leaves the driver stopped-but-inited.
     * Previous code here just returned true on the inited check —
     * subsequent esp_wifi_set_promiscuous / esp_wifi_set_channel /
     * esp_wifi_80211_tx then all failed with ESP_ERR_WIFI_NOT_STARTED
     * and the WiFi task's coex path eventually panicked, freezing the
     * device and forcing a reset. Symptom seen as "Deauth All freezes\n* and restarts". Now we explicitly esp_wifi_start() — a second
     * start on an already-started driver returns ESP_ERR_WIFI_STATE
     * (harmless), but covers the stopped-but-inited case which is the
     * only one POS-AUDIT-008 introduced. */
    wifi_mode_t cur = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&cur) == ESP_OK) {
        if (cur != WIFI_MODE_STA) {
            esp_wifi_set_promiscuous(false);
            esp_wifi_disconnect();
            esp_wifi_set_mode(WIFI_MODE_STA);
            delay(30);
        }
        (void)esp_wifi_start();
        return true;
    }
    /* Fresh init — raw IDF with shrunk buffers to fit in fragmented
     * DMA RAM (M5GFX framebuffer holds ~60 KB at boot, leaves no
     * room for default 32-buffer Arduino init). */
    esp_netif_init();
    esp_event_loop_create_default();
    /* Repro fix 2026-06-06: a previous AP-mode feature (Portal,
     * Evil Twin, CIW, AP Signal Test, SaltyJack rogue-DHCP) may have
     * left a default AP netif resident. Creating a STA netif on top
     * conflicts and panic-restarts during the next esp_wifi_init.
     * Destroy any leftover AP netif first. */
    esp_netif_t *ap_if = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_if) esp_netif_destroy_default_wifi(ap_if);
    /* The static cache below tracked "we already created the STA netif\n* this boot". After the AP cleanup above, the STA netif may also
     * have been collateral-destroyed by a prior call — always probe
     * the handle directly instead of trusting the cache. */
    if (!esp_netif_get_handle_from_ifkey("WIFI_STA_DEF")) {
        esp_netif_create_default_wifi_sta();
    }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    /* TIGHT buffer config. Cardputer-Adv has ~52 KB DMA-capable RAM
     * after M5GFX framebuffer at boot. WiFi init grabs most of it for
     * its tx_buf + rx_buf pools (each ~1.7 KB). At 8/8 we hit ~14 KB
     * of TX + ~14 KB of RX = 28 KB consumed, leaving ~1 KB DMA free
     * for runtime allocations — raw 802.11 TX then OOMs after one
     * frame. 4/4 leaves ~14 KB DMA-free which is plenty for sustained
     * raw TX bursts with the 2 ms inter-frame delay. Sacrifice: brief
     * RX bursts may drop occasional frames (capture rate ~halved at
     * very high traffic), but TX reliability is far more important. */
    cfg.static_tx_buf_num  = 0;
    cfg.dynamic_tx_buf_num = 4;
    cfg.tx_buf_type        = 1;
    cfg.cache_tx_buf_num   = 4;
    cfg.static_rx_buf_num  = 4;
    cfg.dynamic_rx_buf_num = 4;
    cfg.ampdu_tx_enable    = 0;
    cfg.ampdu_rx_enable    = 0;
    cfg.amsdu_tx_enable    = 0;
    esp_err_t ie = esp_wifi_init(&cfg);
    if (ie != ESP_OK) return false;
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_err_t se = esp_wifi_start();
    if (se != ESP_OK) return false;
    /* #A5 / 2026-06-09: bump TX power once at session entry — 84 is
     * the driver max in 0.25 dBm units (21 dBm ~126 mW). Per the
     * wifi_probe.cpp comment, setting this per-burst destabilises the
     * driver; setting it once after esp_wifi_start is the supported
     * pattern. Benefits every STA-mode feature that goes through
     * wifi_lean_sta_init: Deauth All, Scan, Probe, PMKID, Beacon
     * Spam, Wardrive. */
    esp_wifi_set_max_tx_power(84);
    return true;
}

const char *radio_name(void)
{
    switch (s_active) {
    case RADIO_WIFI:   return "wifi";
    case RADIO_BLE:    return "ble";
    case RADIO_LORA:   return "lora";
    case RADIO_SUBGHZ: return "subghz";
    case RADIO_NRF24:  return "nrf24";
    default:           return "idle";
    }
}

radio_domain_t radio_current(void) { return s_active; }

static void teardown_current(void)
{
    switch (s_active) {
    case RADIO_WIFI:
        /* POS-AUDIT-008: PORKCHOP pattern. WiFi.mode(WIFI_OFF) is BANNED
         * — combined with esp_wifi_deinit, repeated session churn
         * fragments the heap and the next esp_wifi_init returns
         * ESP_ERR_NO_MEM (257), eventually deadlocking the driver.
         * esp_wifi_stop alone leaves the driver structures resident so
         * the next feature's WiFi.mode(STA/AP) + start is clean off a
         * hot driver. Heap-aware deinit gate (only deinit when heap is
         * healthy AND largest-free > kMinHeapForTls) lands with
         * POS-AUDIT-118. WiFi.disconnect(false,false) just drops the
         * association without driver-state poke. */
        WiFi.disconnect(false, false);
        esp_wifi_stop();
        break;
    case RADIO_BLE:
        /* Only deinit if NimBLE is actually initialized — features that
         * explicitly deinit on exit (ble_hid) leave a dangling state
         * flag, and a second deinit crashes on some NimBLE builds. */
        /* Deinit to hand back ~62 KB. This is REQUIRED, not optional: leaving
         * NimBLE resident makes wifi_lean_sta_init() fail outright (measured -
         * WiFi could not start on any run with the stack held).
         *
         * Known limitation, surfaced by the radio self-test suite: once deinit
         * has run, NimBLE cannot be re-initialised in the same boot
         * (esp_bt_controller_init -> ESP_ERR_INVALID_STATE 259, controller stuck
         * at status 2, later scans fail rc=30). Walking the controller down by
         * hand does not recover it. So BLE -> other radio -> BLE needs a reboot.
         * Fixing that properly means finding a working NimBLE re-init sequence;
         * keeping BLE resident instead is NOT a fix, it just breaks WiFi. */
        if (NimBLEDevice::isInitialized()) NimBLEDevice::deinit(true);
        /* Finish the job NimBLE skips on IDF 5.x. NimBLEDevice::deinit() runs
         * nimble_port_stop() + nimble_port_deinit(), but its controller teardown
         * (esp_nimble_hci_and_controller_deinit) is gated on
         * ESP_IDF_VERSION < 5.0 (NimBLEDevice.cpp:1040-1046) and this build is
         * IDF 5.5.4 - so the controller was left ENABLED forever. The next
         * init() then hit esp_bt_controller_init() -> ESP_ERR_INVALID_STATE
         * (259) and BLE was dead for the rest of the boot: any
         * BLE -> other radio -> BLE sequence silently stopped working.
         * Walking it back to IDLE ourselves also returns the controller's own
         * memory, which the heap budget wants before WiFi comes up. */
        if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED)
            esp_bt_controller_disable();
        if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_INITED)
            esp_bt_controller_deinit();
        break;
    case RADIO_LORA:
        if (lora_is_up()) lora_end();
        break;
    case RADIO_SUBGHZ:
        cc1101_end();
        gps_begin();  /* re-enable GPS UART on pin 13 after CC1101 releases it */
        break;
    case RADIO_NRF24:
        nrf24_end();
        break;
    default: break;
    }
    s_active = RADIO_NONE;
    delay(100);
}

bool radio_switch(radio_domain_t target)
{
    if (target == s_active) return true;
    teardown_current();
    if (target == RADIO_NONE) return true;

    switch (target) {
    case RADIO_WIFI:
        /* State-only switch — don't touch WiFi here. Calling WiFi.mode(WIFI_STA)
         * left the driver in a half-init state ("STA not started!" warning
         * on disconnect), then any later WiFi.mode(WIFI_AP) crashed in
         * ieee80211_hostap_attach (+0x2c null deref) because the AP-side
         * driver structures were never allocated. Features now bring up
         * WiFi themselves from a clean state (WiFi.mode(WIFI_STA) for
         * scan/wardrive, WiFi.mode(WIFI_AP) for portal/spam). Country
         * code is still applied lazily on first esp_wifi_init by the
         * driver default, and esp_wifi_set_country can be called by
         * features that hop ch12-14. */
        break;
    case RADIO_BLE:
        Serial.printf("[radio] enter RADIO_BLE setup. bt_ctrl_status=%d\n",
                      (int)esp_bt_controller_get_status());
        Serial.flush();
        if (!NimBLEDevice::isInitialized()) {
            Serial.println("[radio] NimBLEDevice::init() begin"); Serial.flush();
            bool ok = NimBLEDevice::init("");
            /* Recover a controller that is already up. On a warm reset the BT
             * controller can still be at status 2 while NimBLE believes it owns
             * nothing, so esp_bt_controller_init() returns ESP_ERR_INVALID_STATE
             * (259), init() reports failure, and every later scan dies with
             * rc=30 - BLE silently does nothing for the rest of the session.
             * Tear the controller down and take it once more. */
            if (!ok) {
                Serial.printf("[radio] NimBLE init failed (bt_ctrl=%d) - "
                              "deinit and retry\n",
                              (int)esp_bt_controller_get_status());
                Serial.flush();
                /* Recover in the order the failures actually demand. Two causes
                 * stack up and the sequence is what matters:
                 *   1. MEMORY. The FIRST real error is esp_nimble_hci_init() ->
                 *      ESP_ERR_NO_MEM (257) with only ~15 KB free.
                 *      teardown_current() deliberately leaves the WiFi driver
                 *      resident (esp_wifi_stop without deinit, POS-AUDIT-008),
                 *      parking memory exactly where NimBLE needs it.
                 *   2. CONTROLLER STATE. Every failed init leaves the controller
                 *      INITED/ENABLED, so the NEXT attempt reports
                 *      ESP_ERR_INVALID_STATE (259) instead - a different error
                 *      for the same underlying problem.
                 * Free the memory FIRST, then walk the controller back to IDLE,
                 * then try once. Doing it the other way round merely converts a
                 * NO_MEM failure into an INVALID_STATE one. */
                NimBLEDevice::deinit(true);
                esp_wifi_stop();
                esp_wifi_deinit();
                delay(150);

                if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED)
                    esp_bt_controller_disable();
                if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_INITED)
                    esp_bt_controller_deinit();
                delay(150);

                Serial.printf("[radio] recovered: bt_ctrl=%d heap=%u\n",
                              (int)esp_bt_controller_get_status(),
                              (unsigned)ESP.getFreeHeap());
                Serial.flush();
                ok = NimBLEDevice::init("");
            }
            Serial.printf("[radio] NimBLEDevice::init() -> %d bt_ctrl_status=%d\n",
                          (int)ok, (int)esp_bt_controller_get_status());
            Serial.flush();
        } else {
            Serial.println("[radio] NimBLE already initialized"); Serial.flush();
        }
        break;
    case RADIO_LORA:
        break;
    case RADIO_SUBGHZ:
        gps_end();  /* release pin 13 — GPS UART TX shares with CC1101 CS */
        break;
    case RADIO_NRF24:
        break;
    default: break;
    }
    s_active = target;
    return true;
}
