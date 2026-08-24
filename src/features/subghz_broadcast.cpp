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
#include "../cc1101_rmt.h"
#include "../sd_helper.h"
#include "subghz_signals_data.h"
#include <ELECHOUSE_CC1101_SRC_DRV.h>
#include <SD.h>

#define MAX_FILES 64
#define MAX_PULSES 2048
#define SIGNALS_DIR "/poseidon/signals"

struct sig_category_t {
    const char *name;
    const char *path;
    const char *desc;
};

static const sig_category_t CATS[] = {
    { "Cars & Garages", "cars",     "Garage doors, gates, key fobs" },
    { "Pranks & Fun",   "pranks",   "Doorbells, pagers, fans, outlets" },
    { "Tesla",          "tesla",    "Charge port, frunk openers" },
    { "Home Auto",      "home",     "Smart plugs, switches, alarms" },
    { "Gas Station",    "gas",      "Price-sign controllers" },
    { "Drug Store",     "drugstore","CVS / Walgreens call buttons" },
    { "Custom",         "custom",   "Your recorded signals" },
    { "All files",      "",         "Browse everything in signals/" },
};
#define CAT_COUNT (sizeof(CATS)/sizeof(CATS[0]))

static char s_files[MAX_FILES][96];   /* recorder emits paths up to 96 chars */
static int  s_file_count = 0;
/* Per-entry "is this a baked signal?" tag. Index into SUBGHZ_BAKED[]
 * when true; otherwise s_files[i] is an SD path. Baked entries are
 * appended AFTER any SD scan results so SD recordings appear at the
 * top of each category list. */
static bool    s_is_baked[MAX_FILES];
static uint8_t s_baked_idx[MAX_FILES];

/* Append every SUBGHZ_BAKED[] entry whose category matches the given
 * subdir name. Called after scan_dir() so baked signals show up under
 * their natural category (Cars / Pranks / Tesla / Home / All files). */
static void append_baked_for_category(const char *subdir)
{
    bool all = (subdir[0] == '\0');
    for (uint16_t i = 0; i < SUBGHZ_BAKED_N && s_file_count < MAX_FILES; ++i) {
        if (!all && strcmp(SUBGHZ_BAKED[i].category, subdir) != 0) continue;
        snprintf(s_files[s_file_count], sizeof(s_files[0]),
                 "* %s", SUBGHZ_BAKED[i].name);
        s_is_baked[s_file_count] = true;
        s_baked_idx[s_file_count] = (uint8_t)i;
        ++s_file_count;
    }
}

static bool scan_dir(const char *subdir)
{
    char path[80];
    if (subdir[0])
        snprintf(path, sizeof(path), "%s/%s", SIGNALS_DIR, subdir);
    else
        snprintf(path, sizeof(path), "%s", SIGNALS_DIR);

    /* Reset list + baked tags. Even if SD open fails (no /signals dir
     * yet), the baked-append step downstream still populates entries. */
    s_file_count = 0;
    for (int i = 0; i < MAX_FILES; ++i) { s_is_baked[i] = false; s_baked_idx[i] = 0; }

    File dir = SD.open(path);
    if (!dir) return false;
    File f;
    while ((f = dir.openNextFile()) && s_file_count < MAX_FILES) {
        String nm = f.name();
        if (nm.endsWith(".sub")) {
            snprintf(s_files[s_file_count], sizeof(s_files[s_file_count]), "%s", f.path());
            s_is_baked[s_file_count] = false;
            s_file_count++;
        } else if (f.isDirectory() && subdir[0] == '\0') {
            /* Recurse one level for "All files" mode */
            File sf;
            while ((sf = f.openNextFile()) && s_file_count < MAX_FILES) {
                String sn = sf.name();
                if (sn.endsWith(".sub")) {
                    snprintf(s_files[s_file_count], sizeof(s_files[s_file_count]), "%s", sf.path());
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
    /* Same continuation-latch as subghz_replay.cpp's parser -- see the long
     * comment there. A RAW_Data line carries up to 512 values (~2.8 KB), so it
     * does not fit this buffer; readBytesUntil() returns it in chunks and only
     * the FIRST chunk starts with "RAW_Data:". Re-testing the prefix per chunk
     * discarded ~80% of every captured waveform and transmitted a skeleton of
     * the real signal. Latch instead. */
    char line[600];
    bool in_raw = false;
    while (f.available() && count < max_pulses) {
        int n = f.readBytesUntil('\n', line, sizeof(line) - 1);
        line[n] = '\0';
        const bool chunk_full = (n == (int)sizeof(line) - 1);

        const char *p = nullptr;
        if (strncmp(line, "RAW_Data:", 9) == 0) { in_raw = true; p = line + 9; }
        else if (in_raw)                        { p = line; }

        if (p) {
            char *q = (char *)p;
            while (*q && count < max_pulses) {
                while (*q == ' ') q++;
                if (!*q) break;
                char *start = q;
                long v = strtol(q, &q, 10);
                if (q == start) break;                  /* stray char */
                if (chunk_full && *q == '\0') break;    /* sliced number */
                if (v != 0) raw[count++] = (int16_t)v;
            }
        }
        if (!chunk_full) in_raw = false;
    }
    f.close();
    return count;
}

/* TX lives in cc1101_rmt.cpp — call site below. */

static int pick_category(void)
{
    auto &d = M5Cardputer.Display;
    int sel = 0;
    int last_sel = -1;

    /* Static chrome painted once. */
    ui_force_clear_body();
    d.setTextColor(T_ACCENT2, T_BG);
    d.setCursor(4, BODY_Y + 2); d.print("BROADCAST");
    d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT2);
    ui_draw_footer(";/.=sel  ENTER=open  ESC=back");

    while (true) {
        if (sel != last_sel) {
            last_sel = sel;
            for (int i = 0; i < (int)CAT_COUNT; ++i) {
                int y = BODY_Y + 14 + i * 13;
                bool s = (i == sel);
                uint16_t row_bg = s ? 0x3007 : T_BG;
                d.fillRect(0, y - 2, SCR_W, 12, T_BG);
                if (s) d.fillRoundRect(2, y - 2, SCR_W - 4, 12, 2, 0x3007);
                d.setTextColor(s ? T_ACCENT : T_FG, row_bg);
                d.setCursor(8, y); d.printf("%s", CATS[i].name);
                d.setTextColor(T_DIM, row_bg);
                d.setCursor(140, y); d.printf("%s", CATS[i].desc);
            }
        }
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
    int last_sel = -1, last_first = -1;

    /* Static chrome painted once. */
    ui_force_clear_body();
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2); d.printf("%s (%d)", cat_name, s_file_count);
    d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT);
    ui_draw_footer(";/.=sel  ENTER=TX  ESC=back");

    while (true) {
        int first = sel < 4 ? 0 : sel - 3;
        if (first + 7 > s_file_count) first = s_file_count > 7 ? s_file_count - 7 : 0;
        if (sel != last_sel || first != last_first) {
            last_sel = sel; last_first = first;
            for (int r = 0; r < 7; ++r) {
                int y = BODY_Y + 18 + r * 13;
                d.fillRect(0, y - 1, SCR_W, 12, T_BG);
                int i = first + r;
                if (i >= s_file_count) continue;
                bool s = (i == sel);
                if (s) d.fillRoundRect(2, y - 1, SCR_W - 4, 12, 2, 0x3007);
                d.setTextColor(s ? T_ACCENT2 : T_FG, s ? 0x3007 : T_BG);
                d.setCursor(6, y);
                const char *base = strrchr(s_files[i], '/');
                d.printf("%s", base ? base + 1 : s_files[i]);
            }
        }

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

        /* Scan SD first (any user recordings show at the top of the list),
         * then append every matching baked signal. A category with zero
         * SD files still shows the baked entries — that's the whole
         * point of baking them in. */
        scan_dir(CATS[cat].path);
        append_baked_for_category(CATS[cat].path);
        if (s_file_count == 0) {
            ui_toast("no .sub files in category", T_WARN, 1200);
            continue;
        }

        int file = pick_file(CATS[cat].name);
        if (file < 0) continue;

        int16_t *raw = (int16_t *)malloc(MAX_PULSES * sizeof(int16_t));
        if (!raw) { ui_toast("OOM", T_BAD, 1000); return; }

        float freq = 433.92f;
        int   plen = 0;
        const char *display_name = nullptr;

        radio_switch(RADIO_SUBGHZ);
        if (!cc1101_begin(s_is_baked[file] ? SUBGHZ_BAKED[s_baked_idx[file]].freq_mhz
                                           : 433.92f)) {
            ui_toast("CC1101 not found", T_BAD, 1500);
            free(raw); radio_switch(RADIO_NONE); return;
        }

        if (s_is_baked[file]) {
            const subghz_baked_t &bk = SUBGHZ_BAKED[s_baked_idx[file]];
            freq = bk.freq_mhz;
            plen = bk.pulse_count;
            if (plen > MAX_PULSES) plen = MAX_PULSES;
            /* Copy from flash .rodata into RAM buffer — TX path expects
             * int16_t* it can read sequentially without flash-cache
             * stalls during the timing-critical RMT write. */
            for (int i = 0; i < plen; ++i) raw[i] = bk.pulses[i];
            display_name = bk.name;
        } else {
            freq = parse_sub_freq(s_files[file]);
            cc1101_set_freq(freq);
            plen = parse_sub_raw(s_files[file], raw, MAX_PULSES);
            const char *b = strrchr(s_files[file], '/');
            display_name = b ? b + 1 : s_files[file];
        }

        auto &d = M5Cardputer.Display;
        uint32_t plays = 0;
        uint32_t last_plays = (uint32_t)-1;
        bool chrome_dirty = true;

        while (true) {
            if (chrome_dirty) {
                chrome_dirty = false;
                ui_force_clear_body();
                ui_draw_status(radio_name(), "broadcast");
                d.setTextColor(T_ACCENT2, T_BG);
                d.setCursor(4, BODY_Y + 2); d.print("BROADCAST");
                d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT2);
                d.setTextColor(T_FG, T_BG);
                d.setCursor(4, BODY_Y + 20); d.printf("file: %s", display_name);
                d.setCursor(4, BODY_Y + 32); d.printf("freq: %.3f MHz", freq);
                d.setCursor(4, BODY_Y + 44); d.printf("pulses: %d", plen);

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
            }

            if (plays != last_plays) {
                last_plays = plays;
                d.setTextColor(T_GOOD, T_BG);
                ui_text_w(4, BODY_Y + 60, 120, T_GOOD, "plays: %lu",
                          (unsigned long)plays);
            }

            uint16_t k = input_poll();
            if (k == PK_NONE) { delay(20); continue; }
            if (k == PK_ESC) break;
            if (k == PK_ENTER) {
                /* Big unmistakable ON-AIR indicator. The frame fills
                 * before cc1101_rmt_tx blocks; the panel still shows
                 * it for the full TX duration (no animation possible
                 * while RMT is timing-critical) and we wipe it after. */
                int by = BODY_Y;
                /* Red border slab around the body — impossible to miss. */
                d.fillRect(0, by, SCR_W,           4, T_BAD);
                d.fillRect(0, FOOTER_Y - 4, SCR_W, 4, T_BAD);
                d.fillRect(0, by, 4,           BODY_H, T_BAD);
                d.fillRect(SCR_W - 4, by, 4,   BODY_H, T_BAD);
                /* Solid red badge: filled circle + "ON AIR" label,
                 * centered horizontally near the top of the body. */
                int cx = SCR_W / 2;
                int cy = by + 40;
                d.fillCircle(cx - 50, cy, 8, T_BAD);
                d.drawCircle(cx - 50, cy, 11, T_BAD);
                d.setTextColor(0xFFFF, T_BAD);
                d.fillRoundRect(cx - 36, cy - 10, 86, 22, 4, T_BAD);
                d.setTextSize(2);
                d.setCursor(cx - 32, cy - 7);
                d.print("ON AIR");
                d.setTextSize(1);
                /* Show frequency + plays underneath. */
                d.setTextColor(T_FG, T_BG);
                d.setCursor(cx - 40, cy + 22);
                d.printf("%.3f MHz", freq);
                d.setCursor(cx - 28, cy + 34);
                d.printf("play #%lu", (unsigned long)(plays + 1));

                /* Fire the radio. cc1101_rmt_tx blocks for the full
                 * pulse stream duration. */
                ELECHOUSE_cc1101.SetTx();
                pinMode(CC1101_GDO0, OUTPUT);
                cc1101_rmt_tx(raw, plen);
                ELECHOUSE_cc1101.setSidle();
                ELECHOUSE_cc1101.SetRx();
                plays++;

                /* Brief post-TX flash so the user sees the difference
                 * between "frame stuck" and "TX actually completed". */
                d.fillRect(0, by, SCR_W,           4, T_GOOD);
                d.fillRect(0, FOOTER_Y - 4, SCR_W, 4, T_GOOD);
                d.fillRect(0, by, 4,           BODY_H, T_GOOD);
                d.fillRect(SCR_W - 4, by, 4,   BODY_H, T_GOOD);
                delay(120);
                /* ON-AIR badge overwrote the body — repaint static chrome. */
                chrome_dirty = true;
                last_plays = (uint32_t)-1;
            }
        }

        free(raw);
        cc1101_end();
        radio_switch(RADIO_NONE);
    }
}
