#pragma once

#include <SPI.h>

/*
 * sd_helper — one place that knows the M5Cardputer SD pins + SPI
 * bus config. Every feature that wants SD should call sd_mount()
 * instead of SD.begin() directly. Idempotent: safe to call every
 * feature entry; returns immediately if already mounted.
 */
bool sd_mount(void);
bool sd_is_mounted(void);

/* FAT-format the inserted card. Re-mounts on success. Returns true
 * on success. BLOCKING, ~3-10s depending on card size. */
bool sd_format(void);

/* Shared SPI bus — the CAP-LoRa1262 SX1262 and the SD card sit on
 * the SAME physical pins (SCK=40 MISO=39 MOSI=14). Features that
 * need SPI to the LoRa chip should pass this bus to RadioLib rather
 * than constructing a separate SPIClass. Features are mutually
 * exclusive so no mutex is needed. */
SPIClass &sd_get_spi(void);

/* Force a fresh remount — use after another SPI peripheral (CC1101)
 * has stolen the GPIO matrix from HSPI. */
bool sd_remount(void);

#include <FS.h>

/*
 * sdlog_open — canonical POSEIDON logger bootstrap.
 *
 * Opens /poseidon/<stem>-<uptime_s>.csv for write, creating the
 * /poseidon directory if missing and writing the optional CSV header
 * line as the first record. Returns an open File the caller writes
 * rows to directly, and (optionally) fills `out_path` with the exact
 * path used so the feature can toast it back to the user.
 *
 * Returns an invalid File (! operator returns true) if:
 *   - SD isn't mountable,
 *   - /poseidon/<stem>-<ts>.csv couldn't be opened for write.
 *
 * Caller owns the File — call .close() when done.
 *
 *   File f = sdlog_open("wifiscan", "ssid,bssid,channel,rssi,auth");
 *   if (f) { f.printf("...\n"); f.close(); }
 */
File sdlog_open(const char *stem,
                const char *header_line = nullptr,
                char *out_path = nullptr,
                size_t out_path_sz = 0);
