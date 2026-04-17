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
