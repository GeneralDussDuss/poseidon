/*
 * POSEIDON main — boot + splash + menu.
 */
#include "app.h"
#include "ui.h"
#include "input.h"
#include "menu.h"
#include "radio.h"
#include "sd_helper.h"
#include "gps.h"
#include "sfx.h"
#include "theme.h"
#include "c5_cmd.h"
#include "version.h"
#include "serial_test.h"
#include "heap_budget.h"
#include "utility/Keyboard/KeyboardReader/TCA8418.h"
#include "board/panel_tembed.h"

/* Strong override: tell Arduino-ESP32 core that BT is in use. Without
 * this the core calls esp_bt_controller_mem_release(ESP_BT_MODE_BTDM)
 * at boot (before app_main), releasing ~36KB of BT memory. After that
 * any esp_bt_controller_enable() will fail silently — NimBLE init
 * looks successful but the controller stays stuck at status 2 (INITED)
 * and BLE scans return zero devices. The weak default implementation
 * in esp32-hal-bt.c returns _btLibraryInUse which is only set to true
 * if a BT library includes esp32-hal-bt-mem.h — NimBLE-Arduino 2.5.0
 * does NOT include it, so on our stack we must opt in manually. */
extern "C" bool btInUse(void) { return true; }

/* Background GPS poller moved into gps.cpp as gps_ensure_running()
 * so that menu features can spawn it lazily when the user opens
 * GPS / Wardrive after boot. (See sys-015 OPSEC default-off + the
 * post-refactor regression where opening the GPS page showed
 * "bytes=0" because main.cpp's boot gate had skipped the task.) */

/* IR LED watchdog. Periodically forces GPIO 44 HIGH to keep the
 * active-LOW IR LED OFF — defends against any code path that leaves
 * pin LOW or in disabled/pulled state. SKIPS when an IR feature is
 * currently active (uses menu.cpp's g_current_feature_item label).
 * 50 ms rate keeps stuck-on duty cycle below 5% so phone cameras see
 * it as off, while still being slow enough not to interfere with
 * IR TX (frames are <50 ms total). */
static bool ir_feature_active(void)
{
    extern const menu_node_t *g_current_feature_item;
    const menu_node_t *it = g_current_feature_item;
    if (!it || !it->label) return false;
    const char *L = it->label;
    if ((L[0] == 'I' && L[1] == 'R') ||
        (L[0] == 'i' && L[1] == 'r') ||
        strstr(L, "Samsung") || strstr(L, "TV-B-Gone") ||
        strstr(L, "Remote") || strstr(L, "Clone") || strstr(L, "Prank")) {
        return true;
    }
    return false;
}
static void ir_watchdog_task(void *_)
{
    pinMode(44, OUTPUT);
    digitalWrite(44, HIGH);
    while (1) {
        if (!ir_feature_active()) {
            digitalWrite(44, HIGH);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

extern bool kerb_boot_key_mode(void);
extern void kerb_boot_sync(void);
extern void feat_kerberos(void);

void setup()
{
    /* FIRST THING: park IR LED OFF. Cardputer-Adv IR LED is ACTIVE-LOW
     * on GPIO 44 (U0RXD by default). At boot the pin floats / has
     * pulldown and the LED visible-glows on phone camera. Hard-park
     * HIGH before anything else (M5GFX board autodetect, libraries,
     * etc.) so the pin is firmly OFF from the very first instruction. */
    pinMode(44, OUTPUT);
    digitalWrite(44, HIGH);

    /* Re-assert KERBEROS key mode from NVS after a cold boot (the OS FIDO reset
     * flow makes the user unplug + reinsert, which wipes the RTC personality
     * flag). May reboot once so the FIDO USB interface enumerates. */
    kerb_boot_sync();

    /* Cardputer-Adv hat compatibility: the CAP-LoRa1262 wires SX1262
     * NSS=G5, BUSY=G6, DIO1=G4, RST=G3. At power-on the SX1262 drives
     * BUSY HIGH until its ready, which breaks M5GFX's board autodetect
     * (it sets G5/G6 to input_pulldown and reads them to distinguish
     * Adv vs K126 vs VAMeter). Holding RST LOW keeps every SX1262
     * output in high-Z, so the Advs native pull-ups on G5/G6 decide
     * the probe and the Adv branch is taken correctly. We also park
     * G5 HIGH afterwards so the chip stays NSS-deselected on the
     * shared SPI bus (same bus as SD). The feature module will drive
     * G3 HIGH when LoRa is actually opened. */
    pinMode(3, OUTPUT);
    digitalWrite(3, LOW);
    delay(5);

#if defined(POSEIDON_BOARD_TEMBED)
    {
        auto cfg = M5.config();
        cfg.internal_spk = false;
        cfg.internal_mic = false;
        M5.begin(cfg);
        tembed_display_init();
    }
#else
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    /* Safety belt: force the I2C (TCA8418) keyboard reader even if
     * autodetect picked K126, so the driver never grabs G3-G7. */
    M5Cardputer.Keyboard.begin(std::make_unique<TCA8418KeyboardReader>());
    /* Release pin 5 — LoRa hat uses it as NSS (needs HIGH to deselect),
     * Hydra hat uses it as CC1101 GDO0 (input). Since we don't know
     * which hat is attached, set INPUT_PULLUP which safely deselects
     * LoRa NSS (pulled high) and won't fight CC1101 GDO0 (input). */
    pinMode(5, INPUT_PULLUP);
    M5Cardputer.Display.setRotation(1);  /* landscape, keyboard at the bottom */
#endif
    Serial.begin(115200);
    hb_install_esp_query();
    heap_census();
    delay(100);
    Serial.printf("\n[POSEIDON] %s (%s) boot\n",
                  poseidon_version(), poseidon_build_date());
    /* bcn_spam_dump_crashtrace removed — only existed in stash's
     * wifi_beacon_spam.cpp which we reverted to HEAD. */
    /* Confirm board detection — Adv uses TCA8418 I2C keyboard (G3-G7 free
     * for hats); original K126 uses GPIO matrix keyboard (G3-G7 claimed). */
    auto board = M5.getBoard();
    Serial.printf("[POSEIDON] board=%d (%s)\n", (int)board,
                  board == m5::board_t::board_M5CardputerADV ? "Cardputer-Adv" :
                  board == m5::board_t::board_M5Cardputer    ? "Cardputer K126" :
                                                               "UNKNOWN");

    /* Mount SD on boot if a card is present. Non-fatal if absent. */
    if (sd_mount()) Serial.println("[POSEIDON] sd mounted");
    else            Serial.println("[POSEIDON] sd absent");

    /* Load persisted sound settings + set speaker volume. */
    sfx_init();
    /* Load persisted theme before UI draws anything — otherwise the
     * splash + first menu render in POSEIDON's default instead of the
     * user's last pick. */
    theme_init();

    Serial.printf("[POSEIDON] boot heap free=%u KB\n",
                  (unsigned)(ESP.getFreeHeap() / 1024));

    /* Re-park IR LED HIGH after all the library inits in case any of
     * them poked GPIO 44. */
    pinMode(44, OUTPUT);
    digitalWrite(44, HIGH);

    /* sys-015 / OPSEC: GPS is OFF by default. The user must opt in via
     * the menu before WiFi captures / wardrive logs carry observer
     * coordinates. Without the gate, WhisperPair / drone RID /
     * surveillance JSONL would silently embed our position the moment
     * a fix arrived. gps_user_enabled() reads NVS once and caches;
     * gps_ensure_running() does the begin + task spawn. */
    if (gps_user_enabled()) {
        gps_ensure_running();
    }

    /* IR LED watchdog — keeps GPIO 44 forced HIGH whenever no IR feature
     * is active. Catches stuck-on bugs from any code path. */
    xTaskCreate(ir_watchdog_task, "ir_park", 1536, nullptr, 1, nullptr);

    /* Test harness — accepts K<hex>/S/R/? over USB serial to drive
     * scripts/test_all_features.py. No-op for normal users. */
    serial_test_init();

    ui_init();

    /* Deferred boot: c5_begin() used to fire here so the satellite
     * badge would light up before any feature was opened. But starting
     * WiFi at boot claims the coex slot before BLE gets a chance to
     * init — esp_bt_controller_enable then fails on first BLE scan
     * (status stuck at 2/INITED). Bruce doesn't run WiFi at boot
     * either. c5_begin is idempotent and called from every WiFi
     * feature entry, so moving it out of boot costs us only the
     * pre-feature satellite badge. */
#ifdef POSEIDON_AUTO_DEAUTH_TEST
    {
        extern void poseidon_autotest_show_last_crash(void);
        poseidon_autotest_show_last_crash();
    }
#endif

#ifdef POSEIDON_AUTO_DEAUTH_TEST
    /* No-UI deauth self-test: 3 stages (basic, burst, triton-hop).
     * Marks progress to NVS so a reboot trace is visible on next boot
     * without serial. Skip splash so we get feedback fast. */
    {
        extern void poseidon_deauth_autotest(void);
        poseidon_deauth_autotest();
    }
#endif

    /* In KERBEROS key mode, skip the splash keypress-wait so the FIDO
     * transport is serviced immediately after enumeration. */
    if (!kerb_boot_key_mode()) ui_splash();
}

void loop()
{
    /* KERBEROS key mode: run the FIDO key directly. It owns the loop and
     * reboots back to normal mode on exit, so this does not fall through. */
    if (kerb_boot_key_mode()) feat_kerberos();
    menu_run();
    /* menu_run only returns on a quit — rare. Fall through to a
     * quiescent poll loop so the device doesn't deadlock. */
    ui_clear_body();
    ui_toast("menu exited", COL_WARN, 800);
    delay(200);
}
