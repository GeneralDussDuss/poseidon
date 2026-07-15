#pragma once

#include <SPI.h>

/* SD chip-select on the shared HSPI bus. Exposed here (not buried in the .cpp)
 * so CC1101/nRF24 bus-parking can deselect it via one canonical macro. */
#define SD_CS 12

/*
 * sd_helper — one place that knows the M5Cardputer SD pins + SPI
 * bus config. Every feature that wants SD should call sd_mount()
 * instead of SD.begin() directly. Idempotent: safe to call every
 * feature entry; returns immediately if already mounted.
 */
bool sd_mount(void);
bool sd_is_mounted(void);

/* Content wipe: recursive delete of every file/dir. Only does a real
 * f_mkfs if the card is UNmountable to begin with. Re-mounts on
 * success. BLOCKING. */
bool sd_format(void);

/* True FAT32 reformat via ESP-IDF f_mkfs — forces a fresh filesystem
 * even on a mountable card (recovers exFAT / corrupt cards on-device).
 * Re-mounts on success. BLOCKING, ~3-15s. */
bool sd_force_format(void);

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

/*
 * sd_rotate_on_size — net-009 / POS-AUDIT-269.
 *
 * Rotation helper for APPEND-mode credential / capture logs that would
 * otherwise grow unbounded across long sessions. Call BEFORE opening
 * `path` for APPEND; if the existing file exceeds `max_bytes`, the
 * current file is moved to `<path>.1` (overwriting any prior .1) and
 * a fresh `path` is started on the next open.
 *
 * Single-generation rotation only — pre-existing .1 is overwritten.
 * That's deliberate: the only legitimate reason a capture file got
 * large is a long session in the field; the most recent rotation is
 * the one the operator cares about. Multi-gen rotation would burn SD
 * write cycles to no real benefit.
 */
void sd_rotate_on_size(const char *path, size_t max_bytes);
