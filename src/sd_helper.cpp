/*
 * sd_helper.cpp — single point of truth for M5Cardputer SD access.
 *
 * Cardputer SD wiring (K126):
 *   SCK  GPIO 40
 *   MISO GPIO 39
 *   MOSI GPIO 14
 *   CS   GPIO 12
 * Uses the FSPI peripheral. Plain SPI.begin() with default pins
 * doesn't work — that's why SD.begin() silently fails everywhere.
 */
#include "sd_helper.h"
#include <SD.h>
#include <SPI.h>
#include <FS.h>
#include <esp_vfs_fat.h>
#include <diskio_impl.h>
#include <sdmmc_cmd.h>

#define SD_SCK   40
#define SD_MISO  39
#define SD_MOSI  14
#define SD_CS    12
#define SD_FREQ  20000000  /* 20 MHz — reliable on 5cm ribbon in the Cardputer */

static bool s_mounted = false;

bool sd_is_mounted(void) { return s_mounted; }

bool sd_mount(void)
{
    if (s_mounted) return true;

    /* End any previous (failed) session first. */
    SD.end();

    /* Bring up SPI with Cardputer's pin mux. */
    SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);

    /* SD.begin(cs, spi, freq, mountpoint, max_files, format_if_mount_failed) */
    if (!SD.begin(SD_CS, SPI, SD_FREQ, "/sd", 5, false)) {
        /* Retry once at half speed for flaky cards. */
        SD.end();
        SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
        if (!SD.begin(SD_CS, SPI, 10000000, "/sd", 5, false)) {
            return false;
        }
    }
    s_mounted = true;
    return true;
}

bool sd_format(void)
{
    /* SD must be known to the FAT layer. Mount with format-on-fail =
     * true so even a totally-fresh card gets a filesystem. */
    SD.end();
    SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    if (!SD.begin(SD_CS, SPI, SD_FREQ, "/sd", 5, true)) {
        s_mounted = false;
        return false;
    }
    /* Nuke contents: walk root and delete everything. Arduino SD
     * doesn't expose FAT format directly, so a full clean is the
     * closest user-meaningful equivalent. */
    File root = SD.open("/");
    if (root) {
        File f;
        while ((f = root.openNextFile())) {
            String path = f.path();
            bool is_dir = f.isDirectory();
            f.close();
            if (is_dir) SD.rmdir(path.c_str());
            else        SD.remove(path.c_str());
        }
        root.close();
    }
    s_mounted = true;
    return true;
}
