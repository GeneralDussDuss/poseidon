/*
 * ble_types — shared across BLE features so scan can hand off a
 * "selected device" to clone / spoof-hid / etc.
 */
#pragma once

#include <Arduino.h>

struct ble_target_t {
    uint8_t  addr[6];
    char     name[20];
    char     type[24];
    int8_t   rssi;
    bool     is_public;
    bool     have_adv;
    uint8_t  adv_data[31];
    uint8_t  adv_len;
};

extern ble_target_t g_ble_target;
extern bool         g_ble_target_valid;
