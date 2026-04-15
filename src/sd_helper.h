#pragma once

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
