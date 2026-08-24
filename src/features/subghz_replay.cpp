/*
 * subghz_replay.cpp — replay .sub files (Flipper + Bruce compatible).
 *
 * Reads RAW_Data pulse timings from .sub files on SD, replays them
 * through CC1101 TX using precise RMT timing.
 */
#include "../app.h"
#include "../theme.h"
#include "../ui.h"
#include "../ui_subghz.h"
#include "../input.h"
#include "../radio.h"
#include "../cc1101_hw.h"
#include "../cc1101_rmt.h"
#include "../sd_helper.h"
#include "../menu.h"
#include <ELECHOUSE_CC1101_SRC_DRV.h>
#include <SD.h>

#define MAX_REPLAY_PULSES 4096

struct sub_file_t {
    float    freq_mhz;
    int16_t *raw;
    int      raw_len;
    char     preset[48];
};

static bool parse_sub_file(const char *path, sub_file_t *out)
{
    File f = SD.open(path, FILE_READ);
    if (!f) return false;

    out->freq_mhz = 433.92f;
    out->raw_len = 0;
    out->preset[0] = '\0';

    /* A RAW_Data line holds up to 512 values (see subghz_record.cpp), i.e.
     * ~2.8 KB — far more than this buffer. readBytesUntil() stops at the buffer
     * limit WITHOUT consuming the newline, so the rest of that physical line
     * arrives as further chunks that do NOT start with "RAW_Data:".
     *
     * The old code re-tested the prefix on every chunk, so those continuations
     * failed the strncmp and were silently dropped: only ~105 of every 512
     * pulses survived and replay/broadcast keyed the radio with a ~20% skeleton
     * of the captured waveform. `in_raw` latches instead: once a RAW_Data line
     * starts, every following chunk keeps feeding the same pulse array until we
     * actually see a newline-terminated chunk.
     *
     * A chunk that filled the buffer is detectable as n == sizeof(line)-1; a
     * short read means the newline was reached and the record is complete. */
    char line[600];
    bool in_raw = false;
    while (f.available()) {
        int n = f.readBytesUntil('\n', line, sizeof(line) - 1);
        line[n] = '\0';
        const bool chunk_full = (n == (int)sizeof(line) - 1);

        const char *p = nullptr;
        if (strncmp(line, "Frequency:", 10) == 0) {
            in_raw = false;
            unsigned long hz = strtoul(line + 10, nullptr, 10);
            out->freq_mhz = hz / 1000000.0f;
        } else if (strncmp(line, "Preset:", 7) == 0) {
            in_raw = false;
            strncpy(out->preset, line + 8, sizeof(out->preset) - 1);
        } else if (strncmp(line, "RAW_Data:", 9) == 0) {
            in_raw = true;
            p = line + 9;
        } else if (in_raw) {
            p = line;              /* continuation of the RAW_Data line */
        }

        if (p) {
            char *q = (char *)p;
            while (*q && out->raw_len < MAX_REPLAY_PULSES) {
                while (*q == ' ') q++;
                if (!*q) break;
                /* A buffer-full cut can slice a number in half ("1234" -> "12").
                 * Drop that trailing partial token; the next chunk re-reads the
                 * whole value. Only applies when we know the line continues. */
                char *start = q;
                long v = strtol(q, &q, 10);
                if (q == start) break;      /* consumed nothing — stray char */
                if (chunk_full && *q == '\0') break;   /* partial trailing number */
                if (v != 0) out->raw[out->raw_len++] = (int16_t)v;
            }
        }
        /* A chunk that did not fill the buffer reached the newline, so whatever
         * record we were in is complete. */
        if (!chunk_full) in_raw = false;
    }
    f.close();
    return out->raw_len > 0;
}

/* TX lives in cc1101_rmt.cpp — call site below. */

/* Simple SD file picker for .sub files. */
static bool pick_sub_file(char *out_path, int max_len)
{
    auto &d = M5Cardputer.Display;

    char names[20][96];   /* recorder emits paths up to 96 chars */
    int count = 0;

    /* Scan a directory for .sub files, appending into names[]. */
    auto scan_dir = [&](const char *path) {
        File dir = SD.open(path);
        if (!dir) return;
        File f;
        while ((f = dir.openNextFile()) && count < 20) {
            if (!f.isDirectory()) {
                String nm = f.name();
                if (nm.endsWith(".sub")) {
                    snprintf(names[count], sizeof(names[count]), "%s", f.path());
                    count++;
                }
            }
            f.close();
        }
        dir.close();
    };

    /* Every recorder writes to /poseidon/signals/<category>/ (see
     * subghz_record.cpp and subghz_scan.cpp), but this picker only ever looked
     * in /poseidon -- so a signal you had just captured could not be replayed,
     * which made Record look broken. Scan /poseidon for loose files AND descend
     * one level into /poseidon/signals' category folders. */
    scan_dir("/poseidon");
    File sig = SD.open("/poseidon/signals");
    if (sig) {
        File cat;
        while ((cat = sig.openNextFile()) && count < 20) {
            if (cat.isDirectory()) {
                char sub[64];
                snprintf(sub, sizeof(sub), "%s", cat.path());
                cat.close();
                scan_dir(sub);
            } else {
                cat.close();
            }
        }
        sig.close();
    }

    if (count == 0) { ui_toast("no .sub files", T_WARN, 1000); return false; }

    int sel = 0;
    int last_sel = -1;

    /* Static chrome painted once. */
    ui_force_clear_body();
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2); d.printf("SELECT .sub (%d)", count);
    d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT);
    ui_draw_footer(";/.=move  ENTER=sel  ESC=back");

    while (true) {
        if (sel != last_sel) {
            last_sel = sel;
            for (int i = 0; i < 7; ++i) {
                int y = BODY_Y + 18 + i * 13;
                d.fillRect(0, y - 1, SCR_W, 12, T_BG);
                int di = (sel < 4) ? i : sel - 3 + i;
                if (di >= count) continue;
                bool s = (di == sel);
                if (s) d.fillRoundRect(2, y - 1, SCR_W - 4, 12, 2, 0x3007);
                d.setTextColor(s ? T_ACCENT2 : T_FG, s ? 0x3007 : T_BG);
                d.setCursor(6, y);
                const char *base = strrchr(names[di], '/');
                d.printf("%s", base ? base + 1 : names[di]);
            }
        }
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) return false;
        if (k == ';' || k == PK_UP)   sel = (sel - 1 + count) % count;
        if (k == '.' || k == PK_DOWN) sel = (sel + 1) % count;
        if (k == PK_ENTER) {
            snprintf(out_path, (size_t)max_len, "%s", names[sel]);
            return true;
        }
    }
}

void feat_subghz_replay(void)
{
    if (!sd_mount()) { ui_toast("SD needed", T_BAD, 1500); return; }

    char path[96];        /* must match the picker width */
    if (!pick_sub_file(path, sizeof(path))) return;

    int16_t *raw = (int16_t *)malloc(MAX_REPLAY_PULSES * sizeof(int16_t));
    if (!raw) { ui_toast("OOM", T_BAD, 1000); return; }

    sub_file_t sub = { .raw = raw, .raw_len = 0 };
    if (!parse_sub_file(path, &sub)) {
        ui_toast("parse fail", T_BAD, 1000);
        free(raw); return;
    }

    radio_switch(RADIO_SUBGHZ);
    cc1101_begin(sub.freq_mhz);

    auto &d = M5Cardputer.Display;
    uint32_t plays = 0;
    uint32_t last_plays = (uint32_t)-1;
    bool chrome_dirty = true;
    while (true) {
        if (chrome_dirty) {
            chrome_dirty = false;
            ui_force_clear_body();
            ui_draw_status(radio_name(), "replay");

            /* Band widget — same as record so user knows band at a glance. */
            ui_draw_freq_band(4, BODY_Y + 2, SCR_W - 8, 10, sub.freq_mhz);

            const char *base = strrchr(path, '/');
            d.setTextColor(T_FG, T_BG);
            d.setCursor(4, BODY_Y + 26); d.printf("%.32s", base ? base + 1 : path);

            /* Waveform preview — no playhead, static full-signal view. */
            ui_draw_pulse_wave(4, BODY_Y + 52, SCR_W - 8, 32,
                               sub.raw, sub.raw_len, -1);

            ui_draw_footer("ENTER=tx ESC=stop");
        }

        if (plays != last_plays) {
            last_plays = plays;
            ui_text_w(4, BODY_Y + 38, SCR_W - 8, T_DIM, "%d pulses  plays %lu",
                      sub.raw_len, (unsigned long)plays);
        }

        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) break;
        if (k == '?') { ui_show_current_help(); }
        if (k == PK_ENTER) {
            /* Fire the radio first, then show the splash — ensures the
             * RF actually hits air at t=0 and user sees visual feedback
             * while CC1101 settles back to RX. */
            cc1101_set_tx();
            pinMode(CC1101_GDO0, OUTPUT);
            /* Static ON AIR badge painted BEFORE the timing critical
             * cc1101_rmt_tx (no animation possible during RMT TX). */
            {
                int by = BODY_Y;
                d.fillRect(0, by, SCR_W, 4, T_BAD);
                d.fillRect(0, by, 4, BODY_H, T_BAD);
                d.fillRect(SCR_W - 4, by, 4, BODY_H, T_BAD);
                int cx = SCR_W / 2;
                int cy = by + 40;
                d.fillCircle(cx - 50, cy, 8, T_BAD);
                d.setTextColor(0xFFFF, T_BAD);
                d.fillRoundRect(cx - 36, cy - 10, 86, 22, 4, T_BAD);
                d.setTextSize(2);
                d.setCursor(cx - 32, cy - 7);
                d.print("ON AIR");
                d.setTextSize(1);
                d.setTextColor(T_FG, T_BG);
                d.setCursor(cx - 44, cy + 22);
                d.printf("TX %.3f MHz", sub.freq_mhz);
            }
            cc1101_rmt_tx(sub.raw, sub.raw_len);
            cc1101_set_rx();
            /* Restore GDO0 to INPUT so subsequent scans / digitalRead
             * see fresh edge data instead of the stale OUTPUT level we
             * left behind. Without this, the next Scan or Record reads
             * a constant value and looks dead. */
            pinMode(CC1101_GDO0, INPUT);
            plays++;

            /* Cinematic LIVE TX splash — concentric expanding rings,
             * rotating spokes, freq + payload readout. ~900 ms. */
            uint32_t payload = 0;
            for (int i = 0; i < sub.raw_len && i < 32; ++i) {
                payload = (payload << 1) | (sub.raw[i] > 0 ? 1 : 0);
            }
            ui_subghz_live_tx_splash(sub.freq_mhz, "RAW REPLAY",
                                     payload, 900);
            /* Splash took over the body — repaint static chrome. */
            chrome_dirty = true;
            last_plays = (uint32_t)-1;
        }
    }

    free(raw);
    cc1101_end();
    radio_switch(RADIO_NONE);
}
