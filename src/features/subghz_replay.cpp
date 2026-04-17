/*
 * subghz_replay.cpp — replay .sub files (Flipper + Bruce compatible).
 *
 * Reads RAW_Data pulse timings from .sub files on SD, replays them
 * through CC1101 TX using precise RMT timing.
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
#include <driver/rmt.h>

#define MAX_REPLAY_PULSES 4096
#define RMT_TX_CHANNEL    RMT_CHANNEL_0
#define RMT_TX_GPIO       CC1101_GDO0

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

    char line[600];
    while (f.available()) {
        int n = f.readBytesUntil('\n', line, sizeof(line) - 1);
        line[n] = '\0';
        if (strncmp(line, "Frequency:", 10) == 0) {
            unsigned long hz = strtoul(line + 10, nullptr, 10);
            out->freq_mhz = hz / 1000000.0f;
        } else if (strncmp(line, "Preset:", 7) == 0) {
            strncpy(out->preset, line + 8, sizeof(out->preset) - 1);
        } else if (strncmp(line, "RAW_Data:", 9) == 0) {
            char *p = line + 9;
            while (*p && out->raw_len < MAX_REPLAY_PULSES) {
                while (*p == ' ') p++;
                if (!*p) break;
                int16_t v = (int16_t)strtol(p, &p, 10);
                if (v != 0) out->raw[out->raw_len++] = v;
            }
        }
    }
    f.close();
    return out->raw_len > 0;
}

static void rmt_tx_raw(const int16_t *raw, int len)
{
    rmt_config_t cfg = RMT_DEFAULT_CONFIG_TX((gpio_num_t)RMT_TX_GPIO, RMT_TX_CHANNEL);
    cfg.clk_div = 80;  /* 1 us */
    cfg.tx_config.loop_en = false;
    cfg.tx_config.carrier_en = false;
    if (rmt_config(&cfg) != ESP_OK) return;
    if (rmt_driver_install(RMT_TX_CHANNEL, 0, 0) != ESP_OK) return;

    /* Convert pulse pairs into RMT items. */
    int items_n = (len + 1) / 2;
    rmt_item32_t *items = (rmt_item32_t *)calloc(items_n, sizeof(rmt_item32_t));
    if (!items) { rmt_driver_uninstall(RMT_TX_CHANNEL); return; }

    for (int i = 0; i < len; i += 2) {
        int idx = i / 2;
        items[idx].duration0 = abs(raw[i]);
        items[idx].level0    = raw[i] > 0 ? 1 : 0;
        if (i + 1 < len) {
            items[idx].duration1 = abs(raw[i + 1]);
            items[idx].level1    = raw[i + 1] > 0 ? 1 : 0;
        }
    }

    rmt_write_items(RMT_TX_CHANNEL, items, items_n, true);
    free(items);
    rmt_driver_uninstall(RMT_TX_CHANNEL);
}

/* Simple SD file picker for .sub files. */
static bool pick_sub_file(char *out_path, int max_len)
{
    auto &d = M5Cardputer.Display;
    File dir = SD.open("/poseidon");
    if (!dir) { ui_toast("cant open /poseidon", T_BAD, 1000); return false; }

    char names[20][48];
    int count = 0;
    File f;
    while ((f = dir.openNextFile()) && count < 20) {
        String nm = f.name();
        if (nm.endsWith(".sub")) {
            strncpy(names[count], f.path(), 47);
            names[count][47] = '\0';
            count++;
        }
        f.close();
    }
    dir.close();
    if (count == 0) { ui_toast("no .sub files", T_WARN, 1000); return false; }

    int sel = 0;
    while (true) {
        ui_clear_body();
        d.setTextColor(T_ACCENT, T_BG);
        d.setCursor(4, BODY_Y + 2); d.printf("SELECT .sub (%d)", count);
        d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT);
        for (int i = 0; i < count && i < 7; ++i) {
            int di = (sel < 4) ? i : sel - 3 + i;
            if (di >= count) break;
            int y = BODY_Y + 18 + i * 13;
            bool s = (di == sel);
            if (s) d.fillRoundRect(2, y - 1, SCR_W - 4, 12, 2, 0x3007);
            d.setTextColor(s ? T_ACCENT2 : T_FG, s ? 0x3007 : T_BG);
            d.setCursor(6, y);
            const char *base = strrchr(names[di], '/');
            d.printf("%s", base ? base + 1 : names[di]);
        }
        ui_draw_footer(";/.=move  ENTER=sel  ESC=back");
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) return false;
        if (k == ';' || k == PK_UP)   sel = (sel - 1 + count) % count;
        if (k == '.' || k == PK_DOWN) sel = (sel + 1) % count;
        if (k == PK_ENTER) {
            strncpy(out_path, names[sel], max_len - 1);
            return true;
        }
    }
}

void feat_subghz_replay(void)
{
    if (!sd_mount()) { ui_toast("SD needed", T_BAD, 1500); return; }

    char path[64];
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
    while (true) {
        ui_clear_body();
        ui_draw_status(radio_name(), "replay");
        d.setTextColor(T_ACCENT2, T_BG);
        d.setCursor(4, BODY_Y + 2); d.print("SUB-GHz REPLAY");
        d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT2);
        d.setTextColor(T_FG, T_BG);
        const char *base = strrchr(path, '/');
        d.setCursor(4, BODY_Y + 20); d.printf("file: %s", base ? base + 1 : path);
        d.setCursor(4, BODY_Y + 32); d.printf("freq: %.3f MHz", sub.freq_mhz);
        d.setCursor(4, BODY_Y + 44); d.printf("pulses: %d", sub.raw_len);
        d.setTextColor(T_GOOD, T_BG);
        d.setCursor(4, BODY_Y + 60); d.printf("plays: %lu", (unsigned long)plays);
        ui_draw_footer("ENTER=transmit  ESC=stop");

        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) break;
        if (k == PK_ENTER) {
            d.setTextColor(T_BAD, T_BG);
            d.setCursor(4, BODY_Y + 76); d.print("TX...");
            cc1101_set_tx();
            rmt_tx_raw(sub.raw, sub.raw_len);
            cc1101_set_rx();
            plays++;
        }
    }

    free(raw);
    cc1101_end();
    radio_switch(RADIO_NONE);
}
