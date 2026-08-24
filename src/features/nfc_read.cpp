/*
 * nfc_read — read ISO14443A NFC tags with the T-Embed CC1101 Plus's onboard
 * PN532. Shows UID / type / ATQA / SAK, and on a Mifare Classic card ENTER
 * dumps all sectors (default-key auth) to /poseidon/nfc/<uid>.mfd on SD.
 * PN532 hardware bring-up + on-card validation still pending.
 */
#include "app.h"
#include "../theme.h"
#include "ui.h"
#include "input.h"
#include "../nfc_hw.h"
#include "../sd_helper.h"
#include <SD.h>
#include <string.h>
#include <stdio.h>

/* Common Mifare Classic keys, tried KEY A first per sector. */
static const uint8_t MIFARE_KEYS[][6] = {
    { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF },
    { 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5 },
    { 0xD3, 0xF7, 0xD3, 0xF7, 0xD3, 0xF7 },
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5 },
    { 0x4D, 0x3A, 0x99, 0xC3, 0x51, 0xDD },
    { 0x1A, 0x98, 0x2C, 0x7E, 0x45, 0x9A },
    { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF },
};
static const uint8_t MIFARE_KEY_COUNT = sizeof(MIFARE_KEYS) / 6;

static bool is_mifare_classic(uint8_t sak)
{
    return sak == 0x08 || sak == 0x18 || sak == 0x09; /* 1K / 4K / Mini */
}

/* Dump the first 16 sectors (full 1K; a 4K card's first 1K). */
static void dump_mifare(const NfcTag &tag)
{
    auto &d = M5Cardputer.Display;
    const uint8_t SECTORS = 16;
    const uint8_t BLOCKS = SECTORS * 4; /* 64 */
    static uint8_t dump[64 * 16];
    memset(dump, 0, sizeof(dump));
    int got = 0;

    d.fillRect(0, BODY_Y + 20, SCR_W, BODY_H - 22, T_BG);
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 22); d.print("Dumping Mifare Classic...");

    for (uint8_t s = 0; s < SECTORS; ++s) {
        const uint8_t first = (uint8_t)(s * 4);
        int keyIdx = -1;
        for (uint8_t k = 0; k < MIFARE_KEY_COUNT; ++k) {
            /* A failed auth deactivates the tag on the PN532 — re-select first. */
            NfcTag re;
            if (!nfc_poll_tag(&re, 120)) continue;
            if (nfc_mifare_auth(first, 0, MIFARE_KEYS[k], tag.uid, tag.uid_len)) { keyIdx = (int)k; break; }
        }
        if (keyIdx >= 0) {
            for (uint8_t b = 0; b < 4; ++b) {
                const uint8_t blk = (uint8_t)(first + b);
                if (nfc_mifare_read(blk, &dump[blk * 16])) ++got;
            }
        }
        d.fillRect(0, BODY_Y + 40, SCR_W, 12, T_BG);
        d.setTextColor(T_DIM, T_BG);
        d.setCursor(4, BODY_Y + 40); d.printf("sector %u/%u   blocks %d/%u", s + 1, SECTORS, got, BLOCKS);
        if (input_poll() == PK_ESC) break;
    }

    char uidhex[24] = { 0 };
    for (uint8_t i = 0; i < tag.uid_len && i < 10; ++i) sprintf(uidhex + i * 2, "%02X", tag.uid[i]);
    char path[64];
    snprintf(path, sizeof(path), "/poseidon/nfc/%s.mfd", uidhex);

    bool saved = false;
    if (got > 0 && sd_mount()) {
        SD.mkdir("/poseidon");
        SD.mkdir("/poseidon/nfc");
        File f = SD.open(path, FILE_WRITE);
        if (f) { f.write(dump, (size_t)BLOCKS * 16); f.close(); saved = true; }
    }

    d.fillRect(0, BODY_Y + 20, SCR_W, BODY_H - 22, T_BG);
    d.setTextColor(got == BLOCKS ? T_GOOD : T_WARN, T_BG);
    d.setCursor(4, BODY_Y + 24); d.printf("Read %d/%u blocks", got, BLOCKS);
    d.setTextColor(T_DIM, T_BG);
    d.setCursor(4, BODY_Y + 40);
    if (saved)         d.printf("Saved %s", path);
    else if (got > 0)  d.print("SD save failed");
    else               d.print("No default key worked");
    d.setCursor(4, BODY_Y + 58); d.print("Press any key...");
    ui_toast(saved ? "Dump saved" : "Dump done", saved ? T_GOOD : T_WARN, 1200);
    while (input_poll() == PK_NONE) delay(20);
}

void feat_nfc_read(void)
{
    auto &d = M5Cardputer.Display;
    ui_clear_body();
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2); d.print("NFC READ");
    ui_draw_footer("ENTER=dump  ESC=exit");

    if (!nfc_begin()) {
        d.setTextColor(T_BAD, T_BG);
        d.setCursor(4, BODY_Y + 24); d.print("PN532 not found (I2C 0x24)");
        d.setTextColor(T_DIM, T_BG);
        d.setCursor(4, BODY_Y + 38); d.print("T-Embed CC1101 Plus only");
        while (input_poll() != PK_ESC) delay(20);
        nfc_end();
        return;
    }

    const uint32_t fw = nfc_firmware_version();
    d.setTextColor(T_DIM, T_BG);
    d.setCursor(SCR_W - 100, BODY_Y + 2);
    d.printf("PN532 v%u.%u", (unsigned)((fw >> 16) & 0xFF), (unsigned)((fw >> 8) & 0xFF));

    d.setTextColor(T_FG, T_BG);
    d.setCursor(4, BODY_Y + 24); d.print("Tap a tag to the reader...");

    NfcTag last;
    memset(&last, 0, sizeof(last));
    bool shown = false;

    for (;;) {
        const uint16_t k = input_poll();
        if (k == PK_ESC) break;
        if (k == PK_ENTER && shown && is_mifare_classic(last.sak)) {
            dump_mifare(last);
            shown = false; /* re-arm scanning */
            d.fillRect(0, BODY_Y + 20, SCR_W, BODY_H - 22, T_BG);
            d.setTextColor(T_FG, T_BG);
            d.setCursor(4, BODY_Y + 24); d.print("Tap a tag to the reader...");
            continue;
        }

        NfcTag t;
        if (nfc_poll_tag(&t, 150)) {
            const bool changed = !shown || t.uid_len != last.uid_len ||
                                 memcmp(t.uid, last.uid, t.uid_len) != 0;
            if (changed) {
                last = t; shown = true;

                d.fillRect(0, BODY_Y + 20, SCR_W, BODY_H - 22, T_BG);
                d.setTextColor(T_GOOD, T_BG);
                d.setCursor(4, BODY_Y + 24); d.print(nfc_tag_type(&t));

                d.setTextColor(T_ACCENT, T_BG);
                d.setCursor(4, BODY_Y + 42); d.print("UID:");
                d.setTextColor(T_FG, T_BG);
                d.setCursor(38, BODY_Y + 42);
                for (uint8_t i = 0; i < t.uid_len; ++i) d.printf("%02X ", t.uid[i]);

                d.setTextColor(T_DIM, T_BG);
                d.setCursor(4, BODY_Y + 58); d.printf("ATQA %04X   SAK %02X   %u-byte UID",
                                                      t.atqa, t.sak, (unsigned)t.uid_len);
                if (is_mifare_classic(t.sak)) {
                    d.setTextColor(T_ACCENT2, T_BG);
                    d.setCursor(4, BODY_Y + 74); d.print("ENTER = dump sectors to SD");
                }
                ui_toast("Tag read", T_GOOD, 700);
            }
        } else {
            shown = false; /* field cleared — allow the same tag to re-trigger */
        }
        delay(40);
    }

    nfc_end();
}
