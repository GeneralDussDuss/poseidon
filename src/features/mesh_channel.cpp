/*
 * mesh_channel — channel name / PSK / frequency config for Meshtastic.
 *
 * Lets the operator set a custom channel name (e.g. "Op0-COMNET") and
 * PSK so POSEIDON can join non-default Meshtastic channels. Settings
 * persist to NVS and are consumed by mesh_begin() on next startup.
 *
 * The channel name determines:
 *   - The Meshtastic channel hash (XOR of djb2(name) XOR XOR(psk))
 *     which goes in byte 13 of every packet header.
 *   - The TX/RX frequency via djb2(name) mod 104 within the band
 *     (903.08 + slot * 2.16 MHz for US906).
 *
 * The PSK determines:
 *   - The AES-128-CTR encryption key for all packets.
 *   - The second half of the channel hash (XOR of PSK bytes).
 *
 * Both sides of a conversation must share the same channel name AND PSK.
 */
#include "../app.h"
#include "../theme.h"
#include "../ui.h"
#include "../input.h"
#include "../mesh/meshtastic.h"
#include <Preferences.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ==================== helpers ==================== */

/* Parse a hex string into bytes. Returns number of bytes written, or
 * 0 if the string is not valid hex (odd length or non-hex chars). */
static int hex_to_bytes(const char *hex, uint8_t *out, int max_out)
{
    int len = strlen(hex);
    if (len == 0 || (len & 1) || len > max_out * 2) return 0;
    for (int i = 0; i < len; i++) {
        char c = hex[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F')))
            return 0;
    }
    int n = len / 2;
    for (int i = 0; i < n; i++) {
        char buf[3] = { hex[i*2], hex[i*2+1], '\0' };
        out[i] = (uint8_t)strtoul(buf, nullptr, 16);
    }
    return n;
}

/* Convert a PSK string to 16 bytes. If it's a valid 32-char hex string
 * it's decoded directly; otherwise djb2-hashes the text and pads. */
static void psk_from_string(const char *str, uint8_t psk[16])
{
    int n = hex_to_bytes(str, psk, 16);
    if (n == 16) return;  /* was valid 32-char hex */
    /* Hash text via djb2, spread across 16 bytes. */
    memset(psk, 0, 16);
    uint32_t h = 5381;
    for (const char *p = str; *p; p++)
        h = ((h << 5) + h) + (uint8_t)*p;
    psk[0]  = (uint8_t)(h);        psk[1]  = (uint8_t)(h >> 8);
    psk[2]  = (uint8_t)(h >> 16);  psk[3]  = (uint8_t)(h >> 24);
    /* Second pass with different seed for remaining bytes. */
    h = 5381;
    for (const char *p = str; *p; p++)
        h = ((h << 5) + h) + (uint8_t)*p + 1;
    psk[4]  = (uint8_t)(h);        psk[5]  = (uint8_t)(h >> 8);
    psk[6]  = (uint8_t)(h >> 16);  psk[7]  = (uint8_t)(h >> 24);
    h = 5381;
    for (const char *p = str; *p; p++)
        h = ((h << 5) + h) + (uint8_t)*p + 2;
    psk[8]  = (uint8_t)(h);        psk[9]  = (uint8_t)(h >> 8);
    psk[10] = (uint8_t)(h >> 16);  psk[11] = (uint8_t)(h >> 24);
    h = 5381;
    for (const char *p = str; *p; p++)
        h = ((h << 5) + h) + (uint8_t)*p + 3;
    psk[12] = (uint8_t)(h);        psk[13] = (uint8_t)(h >> 8);
    psk[14] = (uint8_t)(h >> 16);  psk[15] = (uint8_t)(h >> 24);
}

/* djb2 hash — same algorithm Meshtastic uses for channel name hashing
 * (matches SlotRouter.cpp hash_channel_name). */
static uint32_t djb2_hash(const char *s)
{
    uint32_t h = 5381;
    while (*s) h = ((h << 5) + h) + (uint8_t)*s++;
    return h;
}

/* Meshtastic channel hash: XOR of djb2(channel_name) bytes XOR'd with
 * XOR of all PSK bytes. This is byte 13 in every Meshtastic packet
 * header and determines which packets we accept. */
static uint8_t channel_hash(const char *name, const uint8_t psk[16])
{
    uint32_t dh = djb2_hash(name);
    uint8_t h = (uint8_t)dh ^ (uint8_t)(dh >> 8)
              ^ (uint8_t)(dh >> 16) ^ (uint8_t)(dh >> 24);
    for (int i = 0; i < 16; i++) h ^= psk[i];
    return h;
}

/* Meshtastic frequency from channel name. For the US906 band:
 *   base  = 903.08 MHz
 *   step  = 2.16 MHz per slot
 *   slots = 104  (covering 903.08 - 925.48 MHz)
 *   slot  = djb2(name) mod 104
 *
 * For "LongFast": djb2 = 0x879B3F75, slot = 19, freq = 906.875 MHz. */
static float freq_from_name(const char *name)
{
    uint32_t slot = djb2_hash(name) % 104;
    return 903.08f + slot * 2.16f;
}

/* ==================== screen ==================== */

void feat_mesh_channel(void)
{
    char name[32];
    char psk_hex[64];
    char freq_buf[16];

    /* Load current settings. */
    {
        Preferences p;
        p.begin("poseidon", true);
        String n = p.getString("ch_name", "");
        String k = p.getString("ch_psk", "");
        p.end();
        strlcpy(name, n.c_str(), sizeof(name));
        strlcpy(psk_hex, k.c_str(), sizeof(psk_hex));
    }

    bool has_name = (name[0] != '\0');
    bool has_psk  = (psk_hex[0] != '\0');

    /* Derive active values. */
    const char *eff_name = has_name ? name : "LongFast";
    uint8_t eff_psk[16];
    if (has_psk)
        psk_from_string(psk_hex, eff_psk);
    else
        memcpy(eff_psk, mesh_active_psk(), 16);

    uint8_t hash = channel_hash(eff_name, eff_psk);
    float   freq = freq_from_name(eff_name);
    snprintf(freq_buf, sizeof(freq_buf), "%.3f", freq);

    bool dirty = true;

    while (true) {
        if (dirty) {
            ui_clear_body();
            auto &d = M5Cardputer.Display;

            d.setTextColor(T_ACCENT, T_BG);
            d.setCursor(4, BODY_Y + 2);
            d.print("CHANNEL CONFIG");
            d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT);

            /* Channel name */
            d.setTextColor(T_DIM, T_BG);
            d.setCursor(4, BODY_Y + 16);
            d.print("Name:");
            d.setTextColor(has_name ? T_FG : T_DIM, T_BG);
            d.setCursor(36, BODY_Y + 16);
            d.print(has_name ? name : "(default: LongFast)");

            /* PSK */
            d.setTextColor(T_DIM, T_BG);
            d.setCursor(4, BODY_Y + 28);
            d.print("PSK:");
            d.setTextColor(has_psk ? T_FG : T_DIM, T_BG);
            d.setCursor(36, BODY_Y + 28);
            if (has_psk) {
                /* Show first 16 chars of hex + "..." if longer. */
                char disp[24];
                if (strlen(psk_hex) > 20) {
                    memcpy(disp, psk_hex, 16);
                    memcpy(disp + 16, "...", 4);
                } else {
                    strlcpy(disp, psk_hex, sizeof(disp));
                }
                d.print(disp);
            } else {
                d.print("(default PSK)");
            }

            /* Computed values */
            d.drawFastHLine(4, BODY_Y + 40, SCR_W - 8, T_DIM);

            d.setTextColor(T_ACCENT2, T_BG);
            d.setCursor(4, BODY_Y + 44);
            d.printf("Hash:  0x%02X", hash);

            d.setTextColor(T_ACCENT2, T_BG);
            d.setCursor(4, BODY_Y + 56);
            d.printf("Freq:  %s MHz", freq_buf);

            d.setTextColor(T_ACCENT2, T_BG);
            d.setCursor(4, BODY_Y + 68);
            d.printf("Slot:  %u", (unsigned)(djb2_hash(eff_name) % 104));

            /* Hint: mesh restart needed */
            d.setTextColor(T_DIM, T_BG);
            d.setCursor(4, BODY_Y + 82);
            d.print("Restart mesh (Chat) to apply");

            /* Menu items */
            d.setTextColor(T_GOOD, T_BG);
            d.setCursor(4, BODY_Y + 94);
            d.print("[N]ame  [P]SK  [R]eset  ESC=back");

            dirty = false;
        }

        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) return;

        if (k == 'n' || k == 'N') {
            char buf[32];
            if (input_line("channel:", buf, sizeof(buf))) {
                Preferences p;
                p.begin("poseidon", false);
                if (buf[0] == '\0') {
                    p.remove("ch_name");
                    name[0] = '\0';
                    has_name = false;
                } else {
                    p.putString("ch_name", buf);
                    strlcpy(name, buf, sizeof(name));
                    has_name = true;
                }
                p.end();
                /* Recompute. */
                const char *en = has_name ? name : "LongFast";
                uint8_t ep[16];
                if (has_psk) psk_from_string(psk_hex, ep);
                else         memcpy(ep, mesh_active_psk(), 16);
                hash = channel_hash(en, ep);
                freq = freq_from_name(en);
                snprintf(freq_buf, sizeof(freq_buf), "%.3f", freq);
                dirty = true;
                ui_toast("name saved", T_GOOD, 600);
            }
        }

        if (k == 'p' || k == 'P') {
            char buf[64];
            if (input_line("PSK hex:", buf, sizeof(buf))) {
                if (buf[0] == '\0') {
                    /* Clear custom PSK — revert to default. */
                    Preferences p;
                    p.begin("poseidon", false);
                    p.remove("ch_psk");
                    p.end();
                    psk_hex[0] = '\0';
                    has_psk = false;
                    memcpy(eff_psk, mesh_active_psk(), 16);
                    ui_toast("default PSK", T_GOOD, 600);
                } else {
                    uint8_t test[16];
                    psk_from_string(buf, test);
                    /* Accept any non-empty input. */
                    Preferences p;
                    p.begin("poseidon", false);
                    p.putString("ch_psk", buf);
                    p.end();
                    strlcpy(psk_hex, buf, sizeof(psk_hex));
                    has_psk = true;
                    memcpy(eff_psk, test, 16);
                    ui_toast("PSK saved", T_GOOD, 600);
                }
                const char *en = has_name ? name : "LongFast";
                hash = channel_hash(en, eff_psk);
                freq = freq_from_name(en);
                snprintf(freq_buf, sizeof(freq_buf), "%.3f", freq);
                dirty = true;
            }
        }

        if (k == 'r' || k == 'R') {
            Preferences p;
            p.begin("poseidon", false);
            p.remove("ch_name");
            p.remove("ch_psk");
            p.end();
            name[0] = '\0';
            psk_hex[0] = '\0';
            has_name = false;
            has_psk  = false;
            memcpy(eff_psk, mesh_active_psk(), 16);
            hash = channel_hash("LongFast", eff_psk);
            freq = freq_from_name("LongFast");
            snprintf(freq_buf, sizeof(freq_buf), "%.3f", freq);
            dirty = true;
            ui_toast("reset to defaults", T_GOOD, 600);
        }
    }
}
