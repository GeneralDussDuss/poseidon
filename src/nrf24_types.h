/*
 * nrf24_types — shared target struct so the sniffer can hand off a
 * discovered ESB device to MouseJack injection / device-specific
 * attacks without forcing the user to re-scan from scratch.
 *
 * Producer: nrf24_suite's promiscuous sniffer.
 * Consumer: MouseJack feature (preload address + type).
 *
 * Radio mutex serialises access — no synchronisation needed.
 */
#pragma once

#include <Arduino.h>

struct nrf24_target_t {
    uint8_t  addr[5];
    uint8_t  channel;
    char     type[12];   /* "MS Mouse", "Logi KB", "Unknown", ... */
    uint16_t packet_count;
};

extern nrf24_target_t g_nrf24_last_device;
extern bool           g_nrf24_last_valid;
