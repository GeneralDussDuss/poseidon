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
#include <driver/sdspi_host.h>
#include <driver/spi_common.h>

#define SD_SCK   40
#define SD_MISO  39
#define SD_MOSI  14
/* SD_CS now lives in sd_helper.h so the shared-bus park logic (cc1101/nrf24)
 * has one source of truth for the SD chip-select. */
#define SD_FREQ  20000000  /* 20 MHz — reliable on 5cm ribbon in the Cardputer */

/* Dedicated SPI bus for SD. The M5Cardputer display (M5GFX) claims
 * FSPI/SPI2 for the TFT — confirmed by checking M5GFX init paths.
 * That leaves HSPI/SPI3 free for us. */
static SPIClass sd_spi(HSPI);

static bool s_mounted = false;

bool sd_is_mounted(void) { return s_mounted; }

SPIClass &sd_get_spi(void) { return sd_spi; }

bool sd_remount(void)
{
    s_mounted = false;
    SD.end();
    sd_spi.end();
    delay(10);
    return sd_mount();
}

static bool try_mount(int hz, bool fmt_if_fail, const char *tag)
{
    SD.end();
    sd_spi.end();

    /* SD cards in SPI mode require pull-ups on MISO, CS, and MOSI.
     * The Cardputer board doesn't include them and Arduino's
     * SPI.begin() doesn't enable internal pull-ups by default.
     * Force them on before SPI takes the pins over. */
    pinMode(SD_MISO, INPUT_PULLUP);
    pinMode(SD_MOSI, INPUT_PULLUP);
    pinMode(SD_CS,   INPUT_PULLUP);
    pinMode(SD_SCK,  INPUT_PULLUP);
    /* Toggle CS high so card sees idle state. */
    pinMode(SD_CS, OUTPUT);
    digitalWrite(SD_CS, HIGH);
    delay(10);

    sd_spi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    /* max_files=2 (was 5). Each handle reserves ~600 B of FATFS state +
     * a sector buffer slot. Our features only ever hold 1-2 files open
     * simultaneously (CSV log + transient read), so 5 was wasted heap.
     * Reclaiming ~2 KB matters when WiFi has already eaten the rest. */
    bool ok = SD.begin(SD_CS, sd_spi, hz, "/sd", 2, fmt_if_fail);
    Serial.printf("[sd] %-12s @ %d Hz fmt=%d -> %s\n", tag, hz, fmt_if_fail, ok ? "OK" : "FAIL");
    return ok;
}

bool sd_mount(void)
{
    if (s_mounted) return true;

    /* HSPI only. NEVER probe FSPI — that's M5GFX's bus for the TFT, and
     * rebinding sd_spi to FSPI (as the prior Tier 2/3 code did) calls
     * SPIClass::begin on a bus the display driver is actively using,
     * which trashes the display SPI state and causes subsequent
     * fillScreen/draw calls to block forever on a transaction
     * semaphore. We tolerate "no SD" gracefully — destructive format-
     * on-fail is reachable via the Tools -> SD Format menu, not boot. */
    if (try_mount(SD_FREQ,    false, "HSPI fast")) { s_mounted = true; return true; }
    if (try_mount(10000000,   false, "HSPI half")) { s_mounted = true; return true; }
    if (try_mount( 4000000,   false, "HSPI slow")) { s_mounted = true; return true; }

    return false;
}

File sdlog_open(const char *stem, const char *header_line,
                char *out_path, size_t out_path_sz)
{
    File empty;
    if (!stem || !*stem) return empty;
    if (!sd_mount()) return empty;
    SD.mkdir("/poseidon");
    char path[64];
    snprintf(path, sizeof(path), "/poseidon/%s-%lu.csv",
             stem, (unsigned long)(millis() / 1000));
    File f = SD.open(path, FILE_WRITE);
    if (!f) return empty;
    if (header_line && *header_line) {
        f.println(header_line);
    }
    if (out_path && out_path_sz) {
        strncpy(out_path, path, out_path_sz - 1);
        out_path[out_path_sz - 1] = '\0';
    }
    return f;
}

void sd_rotate_on_size(const char *path, size_t max_bytes)
{
    if (!path || !*path) return;
    if (!sd_mount()) return;
    if (!SD.exists(path)) return;
    File f = SD.open(path, FILE_READ);
    if (!f) return;
    size_t sz = f.size();
    f.close();
    if (sz < max_bytes) return;

    /* Build "<path>.1" — single-generation rotation. */
    char rolled[80];
    snprintf(rolled, sizeof(rolled), "%s.1", path);
    if (SD.exists(rolled)) SD.remove(rolled);
    SD.rename(path, rolled);
}

/* POS-AUDIT-284 / sys-014: recursive nuke helper for sd_format. The
 * previous sd_format only walked the top level and called SD.rmdir
 * on directories — but Arduino SD.rmdir REQUIRES the directory to be
 * empty, so /poseidon and its subdirs survived. tools.cpp had its
 * own recursive nuke duplicating this responsibility. Consolidate
 * here, called from sd_format below. */
/* depth caps the recursion: each level is a real stack frame plus a 192-byte
 * `sub` buffer, so an arbitrarily deep (or maliciously crafted) directory tree
 * would otherwise overflow the task stack. FAT nesting this deep is already
 * pathological; stop rather than crash. */
#define SD_DELETE_MAX_DEPTH 16
static void sd_recursive_delete(const char *path, int depth)
{
    if (depth > SD_DELETE_MAX_DEPTH) return;
    File f = SD.open(path);
    if (!f) return;
    if (f.isDirectory()) {
        File c = f.openNextFile();
        while (c) {
            char sub[192];
            snprintf(sub, sizeof(sub), "%s/%s",
                     strcmp(path, "/") == 0 ? "" : path, c.name());
            bool child_is_dir = c.isDirectory();
            c.close();
            sd_recursive_delete(sub, depth + 1);
            if (!child_is_dir) {
                /* file already removed by recursive call's else branch */
            }
            c = f.openNextFile();
        }
        f.close();
        if (strcmp(path, "/") != 0) SD.rmdir(path);
    } else {
        f.close();
        SD.remove(path);
    }
}

bool sd_format(void)
{
    /* POS-AUDIT-284: this is the destructive wipe entry point. CALLERS
     * MUST CONFIRM with the user before invoking — there is no internal
     * confirmation prompt because the helper is reused from tools menu,
     * format-on-mount-fail paths, and potentially future setup wizards.
     * feat_tool_sd_format is the canonical UI surface; anyone else
     * calling this must own their own confirmation contract.
     *
     * Use try_mount with format-on-fail = true so we hit the same
     * pull-up + CS-toggle init sequence as the regular mount path.
     * Without those, post-format the bus is left in a half-configured
     * state and subsequent SD.open() calls from features (e.g.
     * wardrive's CSV write) fail intermittently. */
    s_mounted = false;
    if (!try_mount(SD_FREQ, true,  "format fast"))
        if (!try_mount(4000000, true,  "format slow")) return false;
    /* Full recursive nuke — Arduino SD has no FAT format API, so a
     * complete content wipe is the closest user-meaningful equivalent. */
    sd_recursive_delete("/", 0);
    s_mounted = true;
    return true;
}

/* True FAT reformat (FAT32 for <=32 GB cards, FAT16 for tiny ones) via
 * the ESP-IDF SDSPI + FATFS f_mkfs path. Unlike sd_format()'s content
 * wipe, this runs a real f_mkfs even on a perfectly mountable card —
 * Arduino's SD.begin(format_if_mount_failed=true) only formats when the
 * mount FAILS, so a healthy card never gets a fresh filesystem. Use
 * this to recover an exFAT / oddly-formatted / corrupt card to clean
 * FAT32 on-device. Returns true on success and leaves the card
 * re-mounted via the normal Arduino path. */
bool sd_force_format(void)
{
    s_mounted = false;
    SD.end();
    sd_spi.end();
    delay(30);

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI3_HOST;            /* HSPI — same bus sd_spi uses */
    host.max_freq_khz = 20000;

    spi_bus_config_t bus = {};
    bus.mosi_io_num     = SD_MOSI;
    bus.miso_io_num     = SD_MISO;
    bus.sclk_io_num     = SD_SCK;
    bus.quadwp_io_num   = -1;
    bus.quadhd_io_num   = -1;
    bus.max_transfer_sz = 4096;
    esp_err_t be = spi_bus_initialize((spi_host_device_t)host.slot, &bus, SPI_DMA_CH_AUTO);
    /* ESP_ERR_INVALID_STATE = bus already initialised — fine, continue. */
    if (be != ESP_OK && be != ESP_ERR_INVALID_STATE) {
        Serial.printf("[sd] force-fmt bus init rc=%d\n", (int)be);
        sd_mount();
        return false;
    }

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs  = (gpio_num_t)SD_CS;
    slot.host_id  = (spi_host_device_t)host.slot;

    esp_vfs_fat_mount_config_t mcfg = {};
    mcfg.format_if_mount_failed = true;
    mcfg.max_files              = 2;
    mcfg.allocation_unit_size   = 16 * 1024;

    sdmmc_card_t *card = nullptr;
    esp_err_t me = esp_vfs_fat_sdspi_mount("/sdfmt", &host, &slot, &mcfg, &card);
    if (me != ESP_OK || !card) {
        Serial.printf("[sd] force-fmt mount rc=%d\n", (int)me);
        spi_bus_free((spi_host_device_t)host.slot);
        delay(20);
        sd_mount();
        return false;
    }

    /* Force the real format even though the mount succeeded. */
    esp_err_t fe = esp_vfs_fat_sdcard_format("/sdfmt", card);
    Serial.printf("[sd] f_mkfs rc=%d\n", (int)fe);
    esp_vfs_fat_sdcard_unmount("/sdfmt", card);
    spi_bus_free((spi_host_device_t)host.slot);
    delay(30);

    /* Re-mount through the normal Arduino path so the rest of the
     * firmware keeps using SD.* as usual. */
    bool ok = sd_mount();
    return (fe == ESP_OK) && ok;
}
