/*
 * ble_db — BLE / Bluetooth device identification database.
 *
 * Lookups:
 *   ble_db_oui(mac24)           : MAC OUI → vendor string
 *   ble_db_apple(subtype, sub2) : Apple Continuity subtype → model
 *   ble_db_fastpair(model_id)   : Google Fast Pair 24-bit model ID → name
 *   ble_db_svc_uuid(uuid16)     : Bluetooth SIG service UUID → name
 *   ble_db_chr_uuid(uuid16)     : BT SIG characteristic UUID → name
 *
 * All return nullptr on miss.
 */
#pragma once
#include <stdint.h>

const char *ble_db_oui(uint32_t oui24);
const char *ble_db_apple(uint8_t subtype, uint8_t sub2);
const char *ble_db_fastpair(uint32_t model24);
const char *ble_db_svc_uuid(uint16_t uuid16);
const char *ble_db_chr_uuid(uint16_t uuid16);

/* Combined best-effort identification for a scanned device.
 *   addr:    6-byte MAC (big-endian, [0] is MSB in Bluetooth display order)
 *   mfg:     manufacturer data (from BLE adv) or nullptr
 *   mfg_len: length of mfg
 * Writes up to `out_sz` bytes into `out` with the best label found.
 * Returns true if ID was populated, false otherwise.
 */
bool ble_db_identify(const uint8_t *addr,
                     const uint8_t *mfg, int mfg_len,
                     char *out, int out_sz);
