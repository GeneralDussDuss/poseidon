/*
 * subghz_broadcast.cpp — categorized .sub file browser + transmitter.
 *
 * Browses /poseidon/signals/ on SD, organized by category:
 *   cars/     — garage doors, key fobs, gates
 *   pranks/   — doorbells, pagers, fans, outlets
 *   tesla/    — charge port opener
 *   custom/   — user recordings
 *
 * Reads Flipper + Bruce .sub format, transmits via CC1101 + RMT.
 */
#include "../app.h"
#include "../theme.h"
#include "../ui.h"
#include "../input.h"
#include "../radio.h"
#include "../cc1101_hw.h"
#include "../sd_helper.h"
#include <ELECHOUSE_CC1101_SRC_DRV.h>
#include <SD.h>
/* driver/rmt.h (legacy API) removed — IDF 5.5 + Arduino 3.3.8 uses
 * driver_ng via rmt_tx.h / rmt_rx.h and aborts at boot if both are
 * linked. Migration to the new API is pending; rmt_tx() below is a
 * no-op stub until then. */

#define MAX_FILES 30
#define MAX_PULSES 2048
#define SIGNALS_DIR "/poseidon/signals"

struct sig_category_t {
    const char *name;
    const char *path;
    const char *desc;
};

static const sig_category_t CATS[] = {
    { "Cars & Garages", "cars",    "Garage doors, gates, key fobs" },
    { "Pranks & Fun",   "pranks",  "Doorbells, pagers, fans, outlets" },
    { "Tesla",          "tesla",   "Charge port, frunk openers" },
    { "Home Auto",      "home",    "Smart plugs, switches, alarms" },
    { "Custom",         "custom",  "Your recorded signals" },
    { "All files",      "",        "Browse everything in signals/" },
};
#define CAT_COUNT (sizeof(CATS)/sizeof(CATS[0]))

static char s_files[MAX_FILES][48];
static int  s_file_count = 0;

static bool scan_dir(const char *subdir)
{
    char path[80];
    if (subdir[0])
        snprintf(path, sizeof(path), "%s/%s", SIGNALS_DIR, subdir);
    else
        snprintf(path, sizeof(path), "%s", SIGNALS_DIR);

    File dir = SD.open(path);
    if (!dir) return false;

    s_file_count = 0;
    File f;
    while ((f = dir.openNextFile()) && s_file_count < MAX_FILES) {
        String nm = f.name();
        if (nm.endsWith(".sub")) {
            strncpy(s_files[s_file_count], f.path(), 47);
            s_files[s_file_count][47] = '\0';
            s_file_count++;
        } else if (f.isDirectory() && subdir[0] == '\0') {
            /* Recurse one level for "All files" mode */
            File sf;
            while ((sf = f.openNextFile()) && s_file_count < MAX_FILES) {
                String sn = sf.name();
                if (sn.endsWith(".sub")) {
                    strncpy(s_files[s_file_count], sf.path(), 47);
                    s_files[s_file_count][47] = '\0';
                    s_file_count++;
                }
                sf.close();
            }
        }
        f.close();
    }
    dir.close();
    return true;
}

static float parse_sub_freq(const char *path)
{
    File f = SD.open(path, FILE_READ);
    if (!f) return 433.92f;
    char line[128];
    float freq = 433.92f;
    while (f.available()) {
        int n = f.readBytesUntil('\n', line, sizeof(line) - 1);
        line[n] = '\0';
        if (strncmp(line, "Frequency:", 10) == 0) {
            unsigned long hz = strtoul(line + 10, nullptr, 10);
            freq = hz / 1000000.0f;
            break;
        }
    }
    f.close();
    return freq;
}

static int parse_sub_raw(const char *path, int16_t *raw, int max_pulses)
{
    File f = SD.open(path, FILE_READ);
    if (!f) return 0;
    int count = 0;
    char line[600];
    while (f.available() && count < max_pulses) {
        int n = f.readBytesUntil('\n', line, sizeof(line) - 1);
        line[n] = '\0';
        if (strncmp(line, "RAW_Data:", 9) != 0) continue;
        char *p = line + 9;
        while (*p && count < max_pulses) {
            while (*p == ' ') p++;
            if (!*p) break;
            int16_t v = (int16_t)strtol(p, &p, 10);
            if (v != 0) raw[count++] = v;
        }
    }
    f.close();
    return count;
}

static void rmt_tx(const int16_t *raw, int len)
{
    (void)raw; (void)len;
    /* TX temporarily disabled on the v0.4 platform migration — the
     * legacy driver/rmt.h API conflicts with IDF 5.5's driver_ng and
     * aborts the whole device at boot. Migration to the new rmt_tx.h
     * API is pending for v0.4.x. The feature still renders its menu
     * and signal list so the code path is exercised. */
}

static int pick_category(void)
{
    auto &d = M5Cardputer.Display;
    int sel = 0;
    while (true) {
        ui_clear_body();
        d.setTextColor(T_ACCENT2, T_BG);
        d.setCursor(4, BODY_Y + 2); d.print("BROADCAST");
        d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT2);
        for (int i = 0; i < (int)CAT_COUNT; ++i) {
            int y = BODY_Y + 18 + i * 14;
            bool s = (i == sel);
            if (s) d.fillRoundRect(2, y - 2, SCR_W - 4, 13, 2, 0x3007);
            d.setTextColor(s ? T_ACCENT : T_FG, s ? 0x3007 : T_BG);
            d.setCursor(8, y); d.printf("%s", CATS[i].name);
            d.setTextColor(T_DIM, s ? 0x3007 : T_BG);
            d.setCursor(140, y); d.printf("%s", CATS[i].desc);
        }
        ui_draw_footer(";/.=sel  ENTER=open  ESC=back");
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) return -1;
        if (k == ';' || k == PK_UP)   sel = (sel - 1 + CAT_COUNT) % CAT_COUNT;
        if (k == '.' || k == PK_DOWN) sel = (sel + 1) % CAT_COUNT;
        if (k == PK_ENTER) return sel;
    }
}

static int pick_file(const char *cat_name)
{
    auto &d = M5Cardputer.Display;
    int sel = 0;
    while (true) {
        ui_clear_body();
        d.setTextColor(T_ACCENT, T_BG);
        d.setCursor(4, BODY_Y + 2); d.printf("%s (%d)", cat_name, s_file_count);
        d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT);
        int first = sel < 4 ? 0 : sel - 3;
        if (first + 7 > s_file_count) first = s_file_count > 7 ? s_file_count - 7 : 0;
        for (int r = 0; r < 7 && first + r < s_file_count; ++r) {
            int i = first + r;
            int y = BODY_Y + 18 + r * 13;
            bool s = (i == sel);
            if (s) d.fillRoundRect(2, y - 1, SCR_W - 4, 12, 2, 0x3007);
            d.setTextColor(s ? T_ACCENT2 : T_FG, s ? 0x3007 : T_BG);
            d.setCursor(6, y);
            const char *base = strrchr(s_files[i], '/');
            d.printf("%s", base ? base + 1 : s_files[i]);
        }
        ui_draw_footer(";/.=sel  ENTER=TX  ESC=back");

        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) return -1;
        if (k == ';' || k == PK_UP)   { if (s_file_count) sel = (sel - 1 + s_file_count) % s_file_count; }
        if (k == '.' || k == PK_DOWN) { if (s_file_count) sel = (sel + 1) % s_file_count; }
        if (k == PK_ENTER) return sel;
    }
}

void feat_subghz_broadcast(void)
{
    if (!sd_mount()) { ui_toast("SD needed", T_BAD, 1500); return; }

    /* Create default directory structure. */
    SD.mkdir(SIGNALS_DIR);
    SD.mkdir(SIGNALS_DIR "/cars");
    SD.mkdir(SIGNALS_DIR "/pranks");
    SD.mkdir(SIGNALS_DIR "/tesla");
    SD.mkdir(SIGNALS_DIR "/home");
    SD.mkdir(SIGNALS_DIR "/custom");

    while (true) {
        int cat = pick_category();
        if (cat < 0) return;

        scan_dir(CATS[cat].path);
        if (s_file_count == 0) {
            ui_toast("no .sub files in category", T_WARN, 1200);
            continue;
        }

        int file = pick_file(CATS[cat].name);
        if (file < 0) continue;

        /* Parse and transmit the selected file. */
        radio_switch(RADIO_SUBGHZ);
        if (!cc1101_begin(433.92f)) {
            ui_toast("CC1101 not found", T_BAD, 1500);
            radio_switch(RADIO_NONE);
            return;
        }

        float freq = parse_sub_freq(s_files[file]);
        ELECHOUSE_cc1101.setMHZ(freq);

        int16_t *raw = (int16_t *)malloc(MAX_PULSES * sizeof(int16_t));
        if (!raw) { ui_toast("OOM", T_BAD, 1000); cc1101_end(); radio_switch(RADIO_NONE); return; }

        int plen = parse_sub_raw(s_files[file], raw, MAX_PULSES);

        auto &d = M5Cardputer.Display;
        const char *base = strrchr(s_files[file], '/');
        uint32_t plays = 0;

        while (true) {
                ui_clear_body();
            ui_draw_status(radio_name(), "broadcast");
            d.setTextColor(T_ACCENT2, T_BG);
            d.setCursor(4, BODY_Y + 2); d.print("BROADCAST");
            d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT2);
            d.setTextColor(T_FG, T_BG);
            d.setCursor(4, BODY_Y + 20); d.printf("file: %s", base ? base + 1 : s_files[file]);
            d.setCursor(4, BODY_Y + 32); d.printf("freq: %.3f MHz", freq);
            d.setCursor(4, BODY_Y + 44); d.printf("pulses: %d", plen);
            d.setTextColor(T_GOOD, T_BG);
            d.setCursor(4, BODY_Y + 60); d.printf("plays: %lu", (unsigned long)plays);

            /* Mini waveform preview. */
            int mid = BODY_Y + 85;
            d.drawFastHLine(4, mid, SCR_W - 8, T_DIM);
            int x = 4;
            for (int i = 0; i < plen && x < SCR_W - 4; ++i) {
                int w = abs(raw[i]) / 100;
                if (w < 1) w = 1; if (w > 15) w = 15;
                uint16_t c = raw[i] > 0 ? T_ACCENT : T_ACCENT2;
                d.fillRect(x, raw[i] > 0 ? mid - 8 : mid + 1, w, 8, c);
                x += w;
            }

            ui_draw_footer("ENTER=TX  ESC=back to list");
    
            uint16_t k = input_poll();
            if (k == PK_NONE) { delay(20); continue; }
            if (k == PK_ESC) break;
            if (k == PK_ENTER) {
                d.setTextColor(T_BAD, T_BG);
                d.setCursor(4, BODY_Y + 98); d.print("TX...");
                ELECHOUSE_cc1101.SetTx();
                rmt_tx(raw, plen);
                ELECHOUSE_cc1101.setSidle();
                ELECHOUSE_cc1101.SetRx();
                plays++;
            }
        }

        free(raw);
        cc1101_end();
        radio_switch(RADIO_NONE);
    }
}
