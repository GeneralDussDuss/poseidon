/*
 * trident.cpp — PC Bridge for TRIDENT desktop app.
 *
 * Streams the Cardputer's framebuffer over USB-CDC and accepts
 * remote keypresses from the PC. Protocol: JSON-lines + binary
 * RGB565 frames. Same CDC exclusivity pattern as MIMIR.
 */
#include "../app.h"
#include "../ui.h"
#include "../input.h"
#include "../radio.h"
#include "../theme.h"
#include "../version.h"
#include "../sd_helper.h"
#include "../wifi_wardrive.h"
#include "trident.h"
#include <SD.h>
#include <esp_heap_caps.h>

/* Stream frame in scanline chunks — only 480 bytes of buffer needed
 * instead of 64KB. Same wire format: JSON header then raw RGB565. */
static uint16_t s_line[240];  /* one scanline = 480 bytes */
static bool s_streaming = false;
static uint32_t s_last_frame_ms = 0;
static const uint32_t FRAME_INTERVAL_MS = 100;  /* 10 fps */

bool g_trident_cdc_active = false;

static void send_frame(void)
{
    auto &d = M5Cardputer.Display;
    const int frame_bytes = 240 * 135 * 2;
    Serial.printf("{\"evt\":\"frame\",\"w\":240,\"h\":135,\"fmt\":\"rgb565\",\"len\":%d}\n", frame_bytes);
    for (int y = 0; y < 135; y++) {
        d.readRect(0, y, 240, 1, s_line);
        /* Swap to big-endian — TRIDENT expects BE, ESP32-S3 is LE. */
        for (int i = 0; i < 240; i++) s_line[i] = __builtin_bswap16(s_line[i]);
        Serial.write(reinterpret_cast<const uint8_t *>(s_line), 480);
    }
}

static uint16_t special_to_pk(const char *s)
{
    if (!s) return 0;
    if (!strcmp(s, "enter")) return PK_ENTER;
    if (!strcmp(s, "esc"))   return PK_ESC;
    if (!strcmp(s, "bksp"))  return PK_BKSP;
    if (!strcmp(s, "tab"))   return PK_TAB;
    if (!strcmp(s, "space")) return PK_SPACE;
    /* Translate to the raw-character codes the POSEIDON menu/input
     * layer actually consumes — Cardputer hardware has no arrow keys
     * and produces ';'/'.' for up/down. PK_UP/DOWN/LEFT/RIGHT exist
     * as defs but nothing in menu.cpp matches them, so navigation
     * from TRIDENT silently did nothing before this fix. */
    if (!strcmp(s, "up"))    return ';';
    if (!strcmp(s, "down"))  return '.';
    if (!strcmp(s, "left"))  return ',';
    if (!strcmp(s, "right")) return '/';
    if (!strcmp(s, "fn"))    return PK_FN;
    return 0;
}

/* Hand-rolled JSON — no ArduinoJson needed for this simple protocol. */
static bool json_val(const char *buf, const char *key, char *out, int sz)
{
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char *p = strstr(buf, pat);
    if (!p) return false;
    p += strlen(pat);
    int i = 0;
    while (*p && *p != '"' && i < sz - 1) out[i++] = *p++;
    out[i] = '\0';
    return i > 0;
}

static bool json_bool_val(const char *buf, const char *key)
{
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\":true", key);
    return strstr(buf, pat) != nullptr;
}

static void handle_line(const char *line)
{
    char cmd[16];
    if (!json_val(line, "cmd", cmd, sizeof(cmd))) return;

    if (!strcmp(cmd, "hello")) {
        Serial.printf("{\"evt\":\"hello\",\"ver\":1,\"product\":\"poseidon\",\"fw\":\"%s\"}\n",
                      poseidon_version());
    } else if (!strcmp(cmd, "key")) {
        char special[12], ch[4];
        if (json_val(line, "special", special, sizeof(special))) {
            uint16_t pk = special_to_pk(special);
            if (pk) input_inject(pk);
        } else if (json_val(line, "char", ch, sizeof(ch))) {
            if (ch[0]) input_inject((uint16_t)ch[0]);
        }
    } else if (!strcmp(cmd, "screenshot")) {
        send_frame();
    } else if (!strcmp(cmd, "stream")) {
        s_streaming = json_bool_val(line, "on");
    } else if (!strcmp(cmd, "status")) {
        /* Dump runtime state so desktop doesn't have to screen-scrape. */
        Serial.printf("{\"evt\":\"status\",\"fw\":\"%s\",\"radio\":\"%s\","
                      "\"heap\":%u,\"wdr_aps\":%d}\n",
                      poseidon_version(),
                      radio_name(),
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                      g_wdr_ap_count);
    } else if (!strcmp(cmd, "loot")) {
        /* Stream the portal credential log as JSON lines. One event per
         * row, plus a trailing loot_end so the PC knows when to stop. */
        char which[16] = {0};
        json_val(line, "which", which, sizeof(which));
        const char *path = "/poseidon/creds.log";
        if (!strcmp(which, "ntlm")) path = "/poseidon/ntlm_hashes.txt";
        else if (!strcmp(which, "responder")) path = "/poseidon/ntlm.log";
        else if (!strcmp(which, "whisperpair")) path = "/poseidon/whisperpair.csv";
        else if (!strcmp(which, "wigle")) {
            /* Wigle CSVs are timestamped — stream the freshest. */
            File dir = SD.open("/poseidon");
            String newest; uint32_t newest_ts = 0;
            if (dir) {
                File f;
                while ((f = dir.openNextFile())) {
                    String n = f.name();
                    if (n.startsWith("wigle-") && n.endsWith(".csv")) {
                        uint32_t ts = strtoul(n.substring(6).c_str(), nullptr, 10);
                        if (ts > newest_ts) { newest_ts = ts; newest = "/poseidon/" + n; }
                    }
                    f.close();
                }
                dir.close();
            }
            if (newest.length()) {
                static char buf[64];
                strncpy(buf, newest.c_str(), sizeof(buf) - 1);
                buf[sizeof(buf) - 1] = '\0';
                path = buf;
            }
        }
        if (!sd_mount()) {
            Serial.println("{\"evt\":\"loot_err\",\"reason\":\"no_sd\"}");
        } else {
            File f = SD.open(path, FILE_READ);
            if (!f) {
                Serial.printf("{\"evt\":\"loot_err\",\"reason\":\"open\",\"path\":\"%s\"}\n", path);
            } else {
                Serial.printf("{\"evt\":\"loot_begin\",\"path\":\"%s\",\"size\":%u}\n",
                              path, (unsigned)f.size());
                /* Stream in 512B chunks as base64-ish raw-text lines.
                 * Keep it simple: send the content with newlines intact
                 * inside a data event per line. */
                String row;
                while (f.available()) {
                    int c = f.read();
                    if (c < 0) break;
                    if (c == '\r') continue;
                    if (c == '\n') {
                        /* Minimal JSON-string escape: backslash + quote. */
                        row.replace("\\", "\\\\");
                        row.replace("\"", "\\\"");
                        Serial.printf("{\"evt\":\"loot_row\",\"data\":\"%s\"}\n", row.c_str());
                        row = "";
                    } else {
                        row += (char)c;
                    }
                }
                if (row.length()) {
                    row.replace("\\", "\\\\");
                    row.replace("\"", "\\\"");
                    Serial.printf("{\"evt\":\"loot_row\",\"data\":\"%s\"}\n", row.c_str());
                }
                f.close();
                Serial.println("{\"evt\":\"loot_end\"}");
            }
        }
    } else if (!strcmp(cmd, "quit")) {
        s_streaming = false;
        Serial.println("{\"evt\":\"bye\"}");
        g_trident_cdc_active = false;
    }
}

static char s_rx[512];
static int  s_rx_len = 0;

static void pump_rx(void)
{
    while (Serial.available()) {
        int c = Serial.read();
        if (c < 0) break;
        if (c == '\r') continue;
        if (c == '\n') {
            s_rx[s_rx_len] = '\0';
            if (s_rx_len > 0) handle_line(s_rx);
            s_rx_len = 0;
        } else if (s_rx_len + 1 < (int)sizeof(s_rx)) {
            s_rx[s_rx_len++] = (char)c;
        } else {
            while (Serial.available() && Serial.read() != '\n') {}
            s_rx_len = 0;
        }
    }
}

void feat_trident(void)
{
    g_trident_cdc_active = true;
    s_streaming = false;
    s_rx_len = 0;

    radio_switch(RADIO_NONE);

    /* Drain any stale boot log data from the Serial buffer so the
     * first hello/stream command from the PC isn't lost in noise. */
    while (Serial.available()) Serial.read();
    delay(50);

    /* Announce ourselves so PC knows bridge is ready. */
    Serial.printf("{\"evt\":\"hello\",\"ver\":1,\"product\":\"poseidon\",\"fw\":\"%s\"}\n",
                  poseidon_version());

    ui_clear_body();
    ui_draw_status("trident", "bridge");
    auto &d = M5Cardputer.Display;
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 20);
    d.print("TRIDENT PC Bridge active");
    d.setTextColor(T_DIM, T_BG);
    d.setCursor(4, BODY_Y + 36);
    d.print("streaming to desktop app");
    d.setCursor(4, BODY_Y + 50);
    d.print("ESC = disconnect");
    ui_draw_footer("ESC=exit  stream controlled by PC");

    while (g_trident_cdc_active) {
        pump_rx();
        if (!Serial) {
            ui_toast("USB disconnected", T_BAD, 1000);
            break;
        }
        if (s_streaming && millis() - s_last_frame_ms >= FRAME_INTERVAL_MS) {
            send_frame();
            s_last_frame_ms = millis();
        }
        uint16_t k = input_poll();
        if (k == PK_ESC) break;
    }

    s_streaming = false;
    Serial.println("{\"evt\":\"bye\"}");
    g_trident_cdc_active = false;
}
