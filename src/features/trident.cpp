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
#include "trident.h"
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
    if (!strcmp(s, "up"))    return PK_UP;
    if (!strcmp(s, "down"))  return PK_DOWN;
    if (!strcmp(s, "left"))  return PK_LEFT;
    if (!strcmp(s, "right")) return PK_RIGHT;
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
