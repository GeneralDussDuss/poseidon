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
#include <esp_bt.h>
#include <NimBLEDevice.h>

static radio_domain_t s_active = RADIO_NONE;

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
        WiFi.disconnect(true, true);
        WiFi.mode(WIFI_OFF);
        esp_wifi_stop();
        esp_wifi_deinit();
        break;
    case RADIO_BLE:
        /* Only deinit if NimBLE is actually initialized — features that
         * explicitly deinit on exit (ble_hid) leave a dangling state
         * flag, and a second deinit crashes on some NimBLE builds. */
        if (NimBLEDevice::getInitialized()) NimBLEDevice::deinit(true);
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
        WiFi.mode(WIFI_STA);
        /* Prior code called WiFi.disconnect(true, true) here which disables
         * STA via enableSTA(false) — meaning subsequent scanNetworks has
         * to re-init the WiFi driver. On a freshly-booted device with
         * plenty of heap this was silent, but after a few features use
         * memory, esp_wifi_init fails with ENOMEM. Use (false, true) so
         * STA stays up and credentials are erased without cycling the
         * whole driver. */
        WiFi.disconnect(false, true);
        break;
    case RADIO_BLE:
        NimBLEDevice::init("POSEIDON");
        NimBLEDevice::setPower(ESP_PWR_LVL_P9);
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
