/*
 * radio — lazy domain management.
 *
 * Only one radio stack runs at a time. Switching domains tears down
 * the old one first to free heap. Copied from Davey Jones's proven
 * architecture — keeps BLE init from starving out of RAM when WiFi
 * is already eating 100KB.
 */
#pragma once

#include <Arduino.h>

enum radio_domain_t {
    RADIO_NONE = 0,
    RADIO_WIFI,
    RADIO_BLE,
    RADIO_LORA,
    RADIO_SUBGHZ,
    RADIO_NRF24,
};

/* Switch domains. Tears down the current one, brings up the new one.
 * Call with RADIO_NONE to drop all radios and return heap. */
bool radio_switch(radio_domain_t target);
radio_domain_t radio_current(void);

/* Short name for status bar ("wifi", "ble", "idle"). */
const char *radio_name(void);
