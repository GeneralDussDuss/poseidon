/*
 * serial_test.cpp — see serial_test.h for protocol.
 */
#include "serial_test.h"
#include "input.h"
#include "menu.h"
#include "version.h"
#include <Arduino.h>

extern const menu_node_t *g_current_feature_item;  /* from menu.cpp */

static void serial_cmd_task(void *)
{
    char     buf[24];
    int      len = 0;
    uint32_t boot_ms = millis();

    Serial.println("[CMD] serial test harness ready");

    while (true) {
        while (Serial.available()) {
            int ci = Serial.read();
            if (ci < 0) break;
            char c = (char)ci;
            if (c == '\n' || c == '\r') {
                buf[len] = 0;
                if (len == 0) { /* blank line */ }
                else if (buf[0] == 'K' && len >= 2) {
                    /* Hex parse — 1 to 4 hex digits. */
                    uint16_t code = (uint16_t)strtol(buf + 1, nullptr, 16);
                    input_inject(code);
                    Serial.printf("[CMD] K%04X\n", code);
                }
                else if (buf[0] == 'S') {
                    uint32_t now = millis();
                    uint32_t idle = input_last_input_ms() ? (now - input_last_input_ms()) : 0;
                    const char *feat = g_current_feature_item
                        ? g_current_feature_item->label : "(menu)";
                    Serial.printf("[STATE] heap=%u up=%lu idle=%lu feat=\"%s\"\n",
                                  (unsigned)ESP.getFreeHeap(),
                                  (unsigned long)(now - boot_ms),
                                  (unsigned long)idle,
                                  feat);
                }
                else if (buf[0] == 'R') {
                    Serial.println("[CMD] reset");
                    delay(50);
                    ESP.restart();
                }
                else if (buf[0] == 'C') {
                    /* TEMP DIAGNOSTIC: CC1101 chip-ID + live RSSI probe. */
                    extern void cc1101_diag(void);
                    cc1101_diag();
                }
                else if (buf[0] == '?') {
                    Serial.printf("[CMD] poseidon %s harness=v1\n",
                                  poseidon_version());
                }
                else {
                    Serial.printf("[CMD] unknown: %s\n", buf);
                }
                len = 0;
            }
            else if (len < (int)sizeof(buf) - 1) {
                buf[len++] = c;
            }
            /* else: silent overflow, line gets dropped on next newline */
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void serial_test_init(void)
{
    xTaskCreate(serial_cmd_task, "ser_cmd", 4096, nullptr, 1, nullptr);
}
