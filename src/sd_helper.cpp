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
#include <new>
#include <esp_vfs_fat.h>
#include <diskio_impl.h>
#include <sdmmc_cmd.h>

#define SD_SCK   40
#define SD_MISO  39
#define SD_MOSI  14
#define SD_CS    12
#define SD_FREQ  20000000  /* 20 MHz — reliable on 5cm ribbon in the Cardputer */

/* Dedicated SPI bus for SD. The M5Cardputer display (M5GFX) claims
 * FSPI/SPI2 for the TFT — confirmed by checking M5GFX init paths.
 * That leaves HSPI/SPI3 free for us. */
static SPIClass sd_spi(HSPI);

static bool s_mounted = false;

bool sd_is_mounted(void) { return s_mounted; }

static bool try_mount(int hz, bool fmt_if_fail, const char *tag)
{
    SD.end();
    sd_spi.end();
    sd_spi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    bool ok = SD.begin(SD_CS, sd_spi, hz, "/sd", 5, fmt_if_fail);
    Serial.printf("[sd] %-12s @ %d Hz fmt=%d -> %s\n", tag, hz, fmt_if_fail, ok ? "OK" : "FAIL");
    return ok;
}

bool sd_mount(void)
{
    if (s_mounted) return true;

    /* Tier 1: HSPI fast (most reliable on the Cardputer). */
    if (try_mount(SD_FREQ,    false, "HSPI fast"))   { s_mounted = true; return true; }
    if (try_mount(10000000,   false, "HSPI half"))   { s_mounted = true; return true; }
    if (try_mount( 4000000,   false, "HSPI slow"))   { s_mounted = true; return true; }

    /* Tier 2: try the OTHER SPI bus in case display is using HSPI on
     * this build. Rebind sd_spi to FSPI. */
    sd_spi.end();
    new (&sd_spi) SPIClass(FSPI);
    if (try_mount(SD_FREQ,    false, "FSPI fast"))   { s_mounted = true; return true; }
    if (try_mount( 4000000,   false, "FSPI slow"))   { s_mounted = true; return true; }

    /* Tier 3: card has no FAT or is corrupted — let the driver format
     * it. Last resort, destructive. */
    sd_spi.end();
    new (&sd_spi) SPIClass(HSPI);
    if (try_mount( 4000000,   true,  "HSPI format")) { s_mounted = true; return true; }
    sd_spi.end();
    new (&sd_spi) SPIClass(FSPI);
    if (try_mount( 4000000,   true,  "FSPI format")) { s_mounted = true; return true; }

    return false;
}

bool sd_format(void)
{
    /* SD must be known to the FAT layer. Mount with format-on-fail =
     * true so even a totally-fresh card gets a filesystem. */
    SD.end();
    sd_spi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    if (!SD.begin(SD_CS, sd_spi, SD_FREQ, "/sd", 5, true)) {
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
