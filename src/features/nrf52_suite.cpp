/*
 * nrf52_suite.cpp — nRF52840 BLE 5.0 attack features.
 *
 * Communicates with an Adafruit Feather nRF52840 Bluefruit over UART
 * (G3 TX, G4 RX on the top hat header). The Feather sits on the
 * Cardputer-Adv top GPIO header as a "hat" — mutually exclusive
 * with LoRa/Hydra/C5.
 *
 * The Feather's built-in NeoPixel provides cyberpunk visual feedback
 * for every function: scanning, sniffing, attacks, captures.
 *
 * Features:
 *   - BLE 5.0 Full Scan (all PHYs)
 *   - Long-Range BLE Scan (Coded PHY S=8, 4x range)
 *   - BLE Advertisement Sniffer (full decode + pcap)
 *   - 802.15.4 Zigbee Sniffer (alternative to C5 node)
 *   - BLE Connection MITM (requires nRF52 FW v2)
 */
#include "../app.h"
#include "../ui.h"
#include "../input.h"
#include "../theme.h"
#include "../sfx.h"
#include "../nrf52_hw.h"
#include "../nrf52_led.h"
#include <Arduino.h>

/* ---- Forward declarations ---- */
void feat_nrf52_scan(void);
void feat_nrf52_sniff(void);
void feat_nrf52_longrange(void);
void feat_nrf52_zigbee(void);
void feat_nrf52_mitm(void);

/* ---- Shared helper: ensure Feather is connected ---- */
static bool ensure_dongle(void)
{
    if (NRF52Hardware::is_up()) return true;

    ui_clear_body();
    ui_draw_status("nRF52", "");
    auto &d = M5Cardputer.Display;
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 10);
    d.print("Connecting to Feather...");
    ui_spinner(SCR_W - 16, BODY_Y + 14, T_ACCENT);

    if (NRF52Hardware::begin()) {
        ui_toast("nRF52840 connected!", T_GOOD, 800);
        return true;
    }

    d.setTextColor(T_BAD, T_BG);
    d.setCursor(4, BODY_Y + 24);
    d.print("Feather not found on UART.");
    d.setCursor(4, BODY_Y + 38);
    d.setTextColor(T_DIM, T_BG);
    d.print("Attach Feather to top header.");
    d.setCursor(4, BODY_Y + 52);
    d.print("G3=TX  G4=RX  3V3  GND");
    d.setCursor(4, BODY_Y + 68);
    d.setTextColor(T_ACCENT2, T_BG);
    d.print("Press any key to go back.");
    ui_draw_footer("ESC=back");

    while (true) {
        uint16_t k = input_poll();
        if (k != PK_NONE) return false;
        delay(40);
    }
}

/* ------------------------------------------------------------------ */
/* BLE 5.0 Long-Range Scanner (Coded PHY S=8)                         */
/* ------------------------------------------------------------------ */
void feat_nrf52_longrange(void)
{
    if (!ensure_dongle()) return;

    /* NeoPixel: deep blue slow throb + cyan sonar spike */
    nrf52_led_set(NRF52_LED_LONG_RANGE);

    ui_clear_body();
    ui_draw_status("nRF52", "LR Scan");
    ui_draw_footer("ESC=stop  R=rescan");

    auto &d = M5Cardputer.Display;
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2);
    d.print("BLE 5.0 Long-Range Scan");
    d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT2);

    String resp;
    NRF52Hardware::send_command("BLE_SCAN_CODED", resp, 500);

    int count = 0;
    const int max_display = 7;
    const int row_h = 13;
    const int first_y = BODY_Y + 16;

    while (true) {
        while (NRF52Hardware::available()) {
            String line = NRF52Hardware::read_line();
            if (line.startsWith("ADV:")) {
                int y = first_y + (count % max_display) * row_h;
                d.fillRect(0, y, SCR_W, row_h, T_BG);

                int c1 = line.indexOf(',', 4);
                int c2 = line.indexOf(',', c1 + 1);
                int c3 = line.indexOf(',', c2 + 1);

                String mac  = line.substring(4, c1);
                String rssi = line.substring(c1 + 1, c2);
                String name = line.substring(c2 + 1, c3 > 0 ? c3 : line.length());
                String phy  = c3 > 0 ? line.substring(c3 + 1) : "1M";

                uint16_t col = phy == "CODED" ? T_ACCENT2 : T_ACCENT;
                d.setTextColor(col, T_BG);
                d.setCursor(4, y + 1);
                d.printf("[%s]", phy.c_str());

                d.setTextColor(T_FG, T_BG);
                d.setCursor(50, y + 1);
                if (name.length() > 0 && name != "?") {
                    d.print(name.substring(0, 14));
                } else {
                    d.print(mac.substring(0, 17));
                }

                int r = abs(rssi.toInt());
                int bar_w = map(constrain(r, 30, 100), 30, 100, 40, 2);
                uint16_t bar_c = r < 50 ? T_GOOD : (r < 75 ? T_WARN : T_BAD);
                d.fillRect(SCR_W - 44, y + 2, bar_w, 8, bar_c);
                d.setTextColor(T_DIM, T_BG);
                d.setCursor(SCR_W - 44 + bar_w + 2, y + 1);
                d.printf("%s", rssi.c_str());

                count++;
            }
        }

        /* Coded PHY advertisers are sparse — animate so waiting reads live. */
        ui_scanning_indicator("long range", count);

        uint16_t k = input_poll();
        if (k == PK_ESC) break;
        if (k == 'r' || k == 'R') {
            count = 0;
            d.fillRect(0, first_y, SCR_W, max_display * row_h, T_BG);
            NRF52Hardware::send_command("BLE_SCAN_CODED", resp, 500);
        }
        delay(20);
    }

    NRF52Hardware::send_command("STOP", resp, 500);
    nrf52_led_set(NRF52_LED_IDLE);
}

/* ------------------------------------------------------------------ */
/* BLE Advertisement Sniffer (full decode, all PHYs)                   */
/* ------------------------------------------------------------------ */
void feat_nrf52_sniff(void)
{
    if (!ensure_dongle()) return;

    /* NeoPixel: rapid purple→magenta data-stream flicker */
    nrf52_led_set(NRF52_LED_BLE_SNIFF);

    ui_clear_body();
    ui_draw_status("nRF52", "BLE Sniff");
    ui_draw_footer("ESC=stop  S=save");

    auto &d = M5Cardputer.Display;
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2);
    d.print("BLE ADV Capture");
    d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT2);

    String resp;
    NRF52Hardware::send_command("BLE_SNIFF_ALL", resp, 500);

    uint32_t pkt_count = 0;
    uint32_t last_draw = 0;
    uint32_t last_pkt_drawn = 0;

    while (true) {
        while (NRF52Hardware::available()) {
            String line = NRF52Hardware::read_line();
            if (line.startsWith("PKT:")) {
                pkt_count++;

                if (millis() - last_draw > 100 && pkt_count != last_pkt_drawn) {
                    last_draw = millis();
                    last_pkt_drawn = pkt_count;

                    d.fillRect(0, BODY_Y + 16, SCR_W, 10, T_BG);
                    d.setTextColor(T_GOOD, T_BG);
                    d.setCursor(4, BODY_Y + 17);
                    d.printf("Packets: %lu", pkt_count);

                    d.fillRect(0, BODY_Y + 30, SCR_W, 40, T_BG);
                    d.setTextColor(T_FG, T_BG);
                    d.setCursor(4, BODY_Y + 32);
                    String display = line.substring(4, min((int)line.length(), 38));
                    d.print(display);
                }
            }
        }

        /* Sniffer updates only on PKT — animate so idle never looks frozen. */
        ui_scanning_indicator("sniffing", (int)pkt_count);

        uint16_t k = input_poll();
        if (k == PK_ESC) break;
        if (k == 's' || k == 'S') {
            /* Gold burst on save */
            nrf52_led_oneshot(NRF52_LED_CAPTURE, NRF52_LED_BLE_SNIFF, 1500);
            ui_toast("Saving to SD...", T_ACCENT, 500);
            NRF52Hardware::send_command("SAVE_PCAP", resp, 2000);
        }
        delay(10);
    }

    NRF52Hardware::send_command("STOP", resp, 500);
    nrf52_led_set(NRF52_LED_IDLE);
}

/* ------------------------------------------------------------------ */
/* BLE Passive Device Scanner                                          */
/* ------------------------------------------------------------------ */
void feat_nrf52_scan(void)
{
    if (!ensure_dongle()) return;

    /* NeoPixel: cyan→magenta→blue color morph sweep */
    nrf52_led_set(NRF52_LED_BLE_SCAN);

    ui_clear_body();
    ui_draw_status("nRF52", "Scan");
    ui_draw_footer("ESC=stop  ENTER=details  R=rescan");

    auto &d = M5Cardputer.Display;
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2);
    d.print("BLE 5.0 Full Scan");
    d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT2);

    String resp;
    NRF52Hardware::send_command("BLE_SCAN", resp, 500);

    int count = 0;
    int cursor = 0;
    const int max_display = 7;
    const int row_h = 13;
    const int first_y = BODY_Y + 16;

    while (true) {
        while (NRF52Hardware::available()) {
            String line = NRF52Hardware::read_line();
            if (line.startsWith("DEV:")) {
                int y = first_y + (count % max_display) * row_h;
                d.fillRect(0, y, SCR_W, row_h, T_BG);

                int c1 = line.indexOf(',', 4);
                int c2 = line.indexOf(',', c1 + 1);
                String mac  = line.substring(4, c1);
                String rssi = line.substring(c1 + 1, c2);
                String name = line.substring(c2 + 1);

                bool sel = ((count % max_display) == cursor);
                if (sel) {
                    d.fillRoundRect(2, y - 1, SCR_W - 4, 12, 2, T_SEL_BG);
                }

                uint16_t bg = sel ? T_SEL_BG : T_BG;
                d.setTextColor(T_ACCENT, bg);
                d.setCursor(4, y + 1);
                d.print(name.length() > 0 ? name.substring(0, 16) : mac);

                d.setTextColor(T_DIM, bg);
                d.setCursor(SCR_W - 30, y + 1);
                d.print(rssi);

                count++;
            }
        }

        /* Updates only on DEV — keep an animated pulse while none arrive. */
        ui_scanning_indicator("scanning", count);

        uint16_t k = input_poll();
        if (k == PK_ESC) break;
        if (k == 'r' || k == 'R') {
            count = 0;
            cursor = 0;
            d.fillRect(0, first_y, SCR_W, max_display * row_h, T_BG);
            NRF52Hardware::send_command("BLE_SCAN", resp, 500);
        }
        if (k == ';' || k == PK_UP)   { cursor = max(0, cursor - 1); }
        if (k == '.' || k == PK_DOWN) { cursor = min(max_display - 1, cursor + 1); }
        delay(20);
    }

    NRF52Hardware::send_command("STOP", resp, 500);
    nrf52_led_set(NRF52_LED_IDLE);
}

/* ------------------------------------------------------------------ */
/* 802.15.4 Zigbee Sniffer (alternative to C5 node)                    */
/* ------------------------------------------------------------------ */
void feat_nrf52_zigbee(void)
{
    if (!ensure_dongle()) return;

    /* NeoPixel: Matrix green / amber alternating */
    nrf52_led_set(NRF52_LED_ZIGBEE);

    ui_clear_body();
    ui_draw_status("nRF52", "Zigbee");
    ui_draw_footer("ESC=stop  +/-=channel");

    auto &d = M5Cardputer.Display;
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2);
    d.print("802.15.4 Zigbee Sniffer");
    d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT2);

    uint8_t channel = 15;
    String resp;
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "ZB_SNIFF %d", channel);
    NRF52Hardware::send_command(cmd, resp, 500);

    uint32_t pkt_count = 0;

    while (true) {
        while (NRF52Hardware::available()) {
            String line = NRF52Hardware::read_line();
            if (line.startsWith("ZB:")) {
                pkt_count++;
                d.fillRect(0, BODY_Y + 16, SCR_W, 10, T_BG);
                d.setTextColor(T_GOOD, T_BG);
                d.setCursor(4, BODY_Y + 17);
                d.printf("CH:%d  Pkts:%lu", channel, pkt_count);

                d.fillRect(0, BODY_Y + 30, SCR_W, 10, T_BG);
                d.setTextColor(T_FG, T_BG);
                d.setCursor(4, BODY_Y + 32);
                d.print(line.substring(3, min((int)line.length(), 38)));
            }
        }

        /* 802.15.4 is silent until a device transmits — animate an active
         * indicator each iteration so a quiet channel never reads as frozen. */
        ui_scanning_indicator("802.15.4", (int)pkt_count);

        uint16_t k = input_poll();
        if (k == PK_ESC) break;
        if (k == '+' && channel < 26) {
            channel++;
            NRF52Hardware::send_command("STOP", resp, 200);
            snprintf(cmd, sizeof(cmd), "ZB_SNIFF %d", channel);
            NRF52Hardware::send_command(cmd, resp, 500);
            d.fillRect(0, BODY_Y + 16, SCR_W, 10, T_BG);
            d.setTextColor(T_ACCENT, T_BG);
            d.setCursor(4, BODY_Y + 17);
            d.printf("Channel: %d", channel);
        }
        if (k == '-' && channel > 11) {
            channel--;
            NRF52Hardware::send_command("STOP", resp, 200);
            snprintf(cmd, sizeof(cmd), "ZB_SNIFF %d", channel);
            NRF52Hardware::send_command(cmd, resp, 500);
            d.fillRect(0, BODY_Y + 16, SCR_W, 10, T_BG);
            d.setTextColor(T_ACCENT, T_BG);
            d.setCursor(4, BODY_Y + 17);
            d.printf("Channel: %d", channel);
        }
        delay(20);
    }

    NRF52Hardware::send_command("STOP", resp, 500);
    nrf52_led_set(NRF52_LED_IDLE);
}

/* ------------------------------------------------------------------ */
/* BLE Connection MITM (placeholder — requires Feather FW v2)          */
/* ------------------------------------------------------------------ */
void feat_nrf52_mitm(void)
{
    if (!ensure_dongle()) return;

    /* NeoPixel: red↔magenta aggressive alternation */
    nrf52_led_set(NRF52_LED_MITM);

    ui_clear_body();
    ui_draw_status("nRF52", "MITM");
    ui_draw_footer("ESC=back");

    auto &d = M5Cardputer.Display;
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2);
    d.print("BLE Connection MITM");
    d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT2);

    d.setTextColor(T_WARN, T_BG);
    d.setCursor(4, BODY_Y + 24);
    d.print("Requires nRF52 MITM firmware.");
    d.setCursor(4, BODY_Y + 38);
    d.setTextColor(T_DIM, T_BG);
    d.print("1. Scan for target device");
    d.setCursor(4, BODY_Y + 50);
    d.print("2. nRF52 clones target ADV");
    d.setCursor(4, BODY_Y + 62);
    d.print("3. Relay packets both ways");
    d.setCursor(4, BODY_Y + 76);
    d.print("4. Inspect/modify L2CAP data");

    d.setTextColor(T_ACCENT2, T_BG);
    d.setCursor(4, BODY_Y + 92);
    d.print("Coming in Feather FW v2...");

    while (true) {
        uint16_t k = input_poll();
        if (k == PK_ESC) break;
        delay(40);
    }

    nrf52_led_set(NRF52_LED_IDLE);
}
