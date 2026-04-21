/*
 * radio_lora.cpp — LoRa + GNSS features for the CAP-LoRa1262 hat.
 *
 *   R-s  Scan      passive RX, band picker, RSSI + payload hex -> SD
 *   R-b  Beacon    TX "POSEIDON:<uptime>:<lat>,<lon>" every 3s
 *   R-m  Mesh LF   Meshtastic LongFast US listener (header-only decode)
 *   R-g  GPS fix   live NMEA: lat/lon/alt/sats/hdop, UTC time
 */
#include "../app.h"
#include "../theme.h"
#include "../ui.h"
#include "../input.h"
#include "../radio.h"
#include "../gps.h"
#include "../lora_hw.h"

/* ---- shared band picker ---------------------------------------------- */

static lora_band_t pick_band(const char *title)
{
    auto &d = M5Cardputer.Display;
    int sel = LORA_BAND_915;
    while (true) {
        ui_clear_body();
        d.setTextColor(T_ACCENT, T_BG);
        d.setCursor(4, BODY_Y + 2); d.printf("%s - pick band", title);
        d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT);
        for (int i = 0; i < LORA_BAND__COUNT; ++i) {
            int y = BODY_Y + 20 + i * 14;
            bool s = (i == sel);
            if (s) d.fillRoundRect(4, y - 2, SCR_W - 8, 13, 2, 0x3007);
            d.setTextColor(s ? T_ACCENT2 : T_FG, s ? 0x3007 : T_BG);
            d.setCursor(10, y);
            d.printf("%s", lora_band_name((lora_band_t)i));
        }
        ui_draw_footer(";/.=move  ENTER=go  ESC=back");
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC)   return (lora_band_t)-1;
        if (k == PK_ENTER) return (lora_band_t)sel;
        if (k == ';' || k == PK_UP)   sel = (sel - 1 + LORA_BAND__COUNT) % LORA_BAND__COUNT;
        if (k == '.' || k == PK_DOWN) sel = (sel + 1) % LORA_BAND__COUNT;
    }
}

/* ---- R-s  Scan ------------------------------------------------------- */

/* DIO1 ISR sets this flag; main loop polls it. Avoids per-frame SPI
 * getPacketLength() calls that can wedge the SX1262 when the chip is
 * in a transition state (seen as a hard hang on 915 MHz scan). */
static volatile bool s_lora_rx_flag = false;
static void IRAM_ATTR lora_rx_isr(void) { s_lora_rx_flag = true; }

void feat_lora_scan(void)
{
    lora_band_t b = pick_band("LoRa SCAN");
    if ((int)b < 0) return;

    radio_switch(RADIO_LORA);
    lora_config_t cfg = lora_preset(b);
    int lora_st = lora_begin(cfg);
    if (lora_st != RADIOLIB_ERR_NONE) {
        char msg[32]; snprintf(msg, sizeof(msg), "LoRa err %d", lora_st);
        ui_toast(msg, T_BAD, 2000);
        radio_switch(RADIO_NONE);
        return;
    }

    /* No SD logging here: SD (HSPI) and LoRa share the HSPI bus on the
     * same pins 40/39/14 (CS=12 vs NSS=5). Opening a file between LoRa
     * transactions is safe in theory, but the raw SPI contention made
     * the scan unreliable. If logging is wanted later, gate writes
     * behind the lora_rx_flag so they only happen between frames. */

    auto &radio = lora_radio();
    s_lora_rx_flag = false;
    radio.setPacketReceivedAction(lora_rx_isr);
    int st = radio.startReceive();
    if (st != RADIOLIB_ERR_NONE) {
        char msg[32]; snprintf(msg, sizeof(msg), "RX start %d", st);
        ui_toast(msg, T_BAD, 1500);
        radio.clearPacketReceivedAction();
        lora_end();
        radio_switch(RADIO_NONE);
        return;
    }

    uint32_t pkts = 0;
    int last_rssi = 0;
    float last_snr = 0;
    char last_hex[97] = "(waiting)";

    auto &d = M5Cardputer.Display;
    uint32_t last_draw = 0;
    while (true) {
        if (s_lora_rx_flag) {
            s_lora_rx_flag = false;
            int len = radio.getPacketLength();
            if (len > 0 && len <= 256) {
                uint8_t buf[256];
                int n = radio.readData(buf, len);
                if (n == RADIOLIB_ERR_NONE) {
                    pkts++;
                    last_rssi = (int)radio.getRSSI();
                    last_snr  = radio.getSNR();
                    int hex_n = (len < 32) ? len : 32;
                    for (int i = 0; i < hex_n; ++i)
                        snprintf(last_hex + i * 3, 4, "%02X ", buf[i]);
                }
            }
            /* Re-arm RX for next packet. If startReceive returns non-OK,
             * log + keep going — one bad rearm shouldn't kill the scan. */
            int rx_err = radio.startReceive();
            if (rx_err != RADIOLIB_ERR_NONE)
                Serial.printf("[lora] rearm err %d\n", rx_err);
        }

        uint32_t now = millis();
        if (now - last_draw > 250) {
            last_draw = now;
            ui_clear_body();
            ui_draw_status(radio_name(), lora_band_name(b));
            d.setTextColor(T_ACCENT, T_BG);
            d.setCursor(4, BODY_Y + 2); d.printf("LoRa RX %.3f MHz", cfg.freq_mhz);
            d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT);
            d.setTextColor(T_FG, T_BG);
            d.setCursor(4, BODY_Y + 18); d.printf("pkts %lu", (unsigned long)pkts);
            d.setCursor(4, BODY_Y + 30); d.printf("rssi %d dBm", last_rssi);
            d.setCursor(4, BODY_Y + 42); d.printf("snr  %.1f dB", last_snr);
            d.setTextColor(T_DIM, T_BG);
            d.setCursor(4, BODY_Y + 58); d.printf("last:");
            d.setCursor(4, BODY_Y + 70); d.printf("%.*s", 40, last_hex);
            d.setCursor(4, BODY_Y + 82); d.printf("%.*s", 40, last_hex + 40);
            ui_draw_footer("ESC=stop");
        }

        uint16_t k = input_poll();
        if (k == PK_ESC) break;
        delay(10);
    }

    radio.clearPacketReceivedAction();
    lora_end();
    radio_switch(RADIO_NONE);
}

/* ---- R-b  Beacon ----------------------------------------------------- */

void feat_lora_beacon(void)
{
    lora_band_t b = pick_band("LoRa BEACON");
    if ((int)b < 0) return;

    radio_switch(RADIO_LORA);
    lora_config_t cfg = lora_preset(b);
    if (lora_begin(cfg) != RADIOLIB_ERR_NONE) {
        ui_toast("LoRa init fail", T_BAD, 1500);
        radio_switch(RADIO_NONE);
        return;
    }

    auto &radio = lora_radio();
    uint32_t beacons = 0;
    uint32_t last_tx = 0;
    int last_st = 0;

    auto &d = M5Cardputer.Display;
    uint32_t last_draw = 0;
    while (true) {
        uint32_t now = millis();
        if (now - last_tx > 3000) {
            last_tx = now;
            gps_fix_t g;
            bool gps = gps_snapshot(&g);
            char payload[96];
            int n = snprintf(payload, sizeof(payload),
                             "POSEIDON:%lu:%s%.5f,%.5f",
                             (unsigned long)(now / 1000),
                             gps ? "" : "nofix:",
                             gps ? g.lat_deg : 0.0,
                             gps ? g.lon_deg : 0.0);
            last_st = radio.transmit((uint8_t *)payload, n);
            if (last_st == RADIOLIB_ERR_NONE) beacons++;
        }

        if (now - last_draw > 200) {
            last_draw = now;
            ui_clear_body();
            ui_draw_status(radio_name(), lora_band_name(b));
            d.setTextColor(T_ACCENT, T_BG);
            d.setCursor(4, BODY_Y + 2); d.printf("LoRa TX %.3f MHz", cfg.freq_mhz);
            d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT);
            d.setTextColor(T_FG, T_BG);
            d.setCursor(4, BODY_Y + 18); d.printf("beacons: %lu", (unsigned long)beacons);
            d.setCursor(4, BODY_Y + 30); d.printf("power:   %d dBm", cfg.power);
            d.setTextColor(last_st == 0 ? T_GOOD : T_BAD, T_BG);
            d.setCursor(4, BODY_Y + 42); d.printf("last tx: %s",
                                                   last_st == 0 ? "OK" : "ERR");
            uint32_t until = (last_tx + 3000 > now) ? (last_tx + 3000 - now) : 0;
            d.setTextColor(T_DIM, T_BG);
            d.setCursor(4, BODY_Y + 58); d.printf("next in %lu ms", (unsigned long)until);
            ui_draw_footer("ESC=stop");
        }

        uint16_t k = input_poll();
        if (k == PK_ESC) break;
        delay(20);
    }

    lora_end();
    radio_switch(RADIO_NONE);
}

/* ---- R-m  Meshtastic listener --------------------------------------- */

void feat_lora_meshtastic(void)
{
    radio_switch(RADIO_LORA);
    lora_config_t cfg = lora_preset(LORA_BAND_MESHTASTIC_US);
    if (lora_begin(cfg) != RADIOLIB_ERR_NONE) {
        ui_toast("LoRa init fail", T_BAD, 1500);
        radio_switch(RADIO_NONE);
        return;
    }
    auto &radio = lora_radio();
    s_lora_rx_flag = false;
    radio.setPacketReceivedAction(lora_rx_isr);
    int rx_st = radio.startReceive();
    if (rx_st != RADIOLIB_ERR_NONE) {
        char msg[32]; snprintf(msg, sizeof(msg), "RX start %d", rx_st);
        ui_toast(msg, T_BAD, 1500);
        radio.clearPacketReceivedAction();
        lora_end();
        radio_switch(RADIO_NONE);
        return;
    }

    uint32_t pkts = 0;
    uint32_t last_from = 0, last_to = 0, last_id = 0;
    int last_rssi = 0;
    float last_snr = 0;

    auto &d = M5Cardputer.Display;
    uint32_t last_draw = 0;
    while (true) {
        if (s_lora_rx_flag) {
            s_lora_rx_flag = false;
            int len = radio.getPacketLength();
            uint8_t buf[256];
            int n = (len > 0 && len <= 256) ? radio.readData(buf, len) : RADIOLIB_ERR_RX_TIMEOUT;
            Serial.printf("[mesh] rx len=%d readData=%d rssi=%d snr=%.1f\n",
                          len, n, (int)radio.getRSSI(), radio.getSNR());
            if (n == RADIOLIB_ERR_NONE && len >= 16) {
                /* Meshtastic packet header (little-endian):
                 *   0..3  dest nodeId
                 *   4..7  from nodeId
                 *   8..11 packet id
                 *   12    flags
                 *   13    channel hash
                 *   14..15 reserved */
                last_to   = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8)
                          | ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
                last_from = (uint32_t)buf[4] | ((uint32_t)buf[5] << 8)
                          | ((uint32_t)buf[6] << 16) | ((uint32_t)buf[7] << 24);
                last_id   = (uint32_t)buf[8] | ((uint32_t)buf[9] << 8)
                          | ((uint32_t)buf[10] << 16) | ((uint32_t)buf[11] << 24);
                last_rssi = (int)radio.getRSSI();
                last_snr  = radio.getSNR();
                pkts++;
                Serial.printf("[mesh] pkt#%lu to=0x%08lx from=0x%08lx id=0x%08lx\n",
                              (unsigned long)pkts,
                              (unsigned long)last_to,
                              (unsigned long)last_from,
                              (unsigned long)last_id);
            }
            /* Re-arm. One bad rearm shouldn't stop the scan. */
            int rearm_err = radio.startReceive();
            if (rearm_err != RADIOLIB_ERR_NONE)
                Serial.printf("[mesh] rearm err %d\n", rearm_err);
        }

        uint32_t now = millis();
        if (now - last_draw > 250) {
            last_draw = now;
            ui_clear_body();
            ui_draw_status(radio_name(), "mesh LF");
            d.setTextColor(T_ACCENT2, T_BG);
            d.setCursor(4, BODY_Y + 2); d.printf("Meshtastic 906.875");
            d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT2);
            d.setTextColor(T_FG, T_BG);
            d.setCursor(4, BODY_Y + 18); d.printf("packets: %lu", (unsigned long)pkts);
            d.setCursor(4, BODY_Y + 30); d.printf("from: %08lX", (unsigned long)last_from);
            d.setCursor(4, BODY_Y + 42); d.printf("to:   %08lX", (unsigned long)last_to);
            d.setCursor(4, BODY_Y + 54); d.printf("id:   %08lX", (unsigned long)last_id);
            d.setTextColor(T_DIM, T_BG);
            d.setCursor(4, BODY_Y + 70); d.printf("rssi %d  snr %.1f", last_rssi, last_snr);
            ui_draw_footer("ESC=stop");
        }

        uint16_t k = input_poll();
        if (k == PK_ESC) break;
        delay(10);
    }

    radio.clearPacketReceivedAction();
    lora_end();
    radio_switch(RADIO_NONE);
}

/* ---- R-g  GPS fix ---------------------------------------------------- */

void feat_gps_fix(void)
{
    /* GPS task already running from boot, nothing to init. */
    auto &d = M5Cardputer.Display;
    uint32_t last_draw = 0;
    while (true) {
        uint32_t now = millis();
        if (now - last_draw > 250) {
            last_draw = now;
            ui_clear_body();
            ui_draw_status(radio_name(), "gps");
            const gps_fix_t &g = gps_get();
            const gps_diag_t &dg = gps_diag();
            d.setTextColor(g.valid ? T_GOOD : T_WARN, T_BG);
            d.setCursor(4, BODY_Y + 2);
            d.printf("GNSS: %s  baud %lu",
                     g.valid ? "FIX" : "searching",
                     (unsigned long)gps_current_baud());
            d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT);
            if (g.valid) {
                d.setTextColor(T_GOOD, T_BG);
                d.setCursor(4, BODY_Y + 18); d.printf("lat  %.6f", g.lat_deg);
                d.setCursor(4, BODY_Y + 30); d.printf("lon  %.6f", g.lon_deg);
                d.setCursor(4, BODY_Y + 42); d.printf("alt  %.1f m", g.alt_m);
                d.setCursor(4, BODY_Y + 54); d.printf("sats %u  hdop %.1f", g.sats, g.hdop);
                d.setCursor(4, BODY_Y + 66); d.printf("spd  %.1f kt", g.speed_kts);
                d.setCursor(4, BODY_Y + 78); d.printf("utc  %s", g.utc);
            } else {
                d.setTextColor(T_WARN, T_BG);
                d.setCursor(4, BODY_Y + 18); d.print("waiting for fix...");
                d.setTextColor(dg.bytes ? T_GOOD : T_BAD, T_BG);
                d.setCursor(4, BODY_Y + 32); d.printf("bytes %lu", (unsigned long)dg.bytes);
                d.setTextColor(dg.lines ? T_GOOD : T_BAD, T_BG);
                d.setCursor(4, BODY_Y + 44); d.printf("lines %lu  GGA %lu  RMC %lu",
                    (unsigned long)dg.lines, (unsigned long)dg.gga, (unsigned long)dg.rmc);
                d.setTextColor(T_DIM, T_BG);
                d.setCursor(4, BODY_Y + 58); d.printf("%.*s", 40, dg.last[0] ? dg.last : "(no NMEA yet)");
                d.setCursor(4, BODY_Y + 70); d.printf("%.*s", 40, dg.last[0] ? dg.last + 40 : "");
                d.setTextColor(T_DIM, T_BG);
                d.setCursor(4, BODY_Y + 88); d.print("bytes=0? try B to retry baud");
            }
            ui_draw_footer(g.valid ? "ESC=back" : "ESC=back  B=cycle baud");
        }
        uint16_t k = input_poll();
        if (k == PK_ESC) break;
        if (k == 'b' || k == 'B') {
            uint32_t nb = gps_cycle_baud();
            char msg[32];
            snprintf(msg, sizeof(msg), "baud=%lu", (unsigned long)nb);
            ui_toast(msg, T_ACCENT, 600);
        }
        delay(20);
    }
}
