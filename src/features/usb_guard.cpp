/*
 * usb_guard — "Cable Guard": detect dangerous USB cables / chargers by
 * their RF implant.
 *
 * Threat model: malicious cables (O.MG, Hak5, DIY ESP-based keystroke
 * injectors) and trojaned chargers hide a tiny radio in the connector.
 * When powered they beacon a WiFi AP (or BLE) to phone home / accept
 * C2. This feature catches that radio.
 *
 * Method — delta detection (the strongest general signal):
 *   1. BASELINE: scan all 2.4 GHz APs in range, record their BSSIDs.
 *   2. Operator plugs the suspect cable/charger (it powers the implant).
 *   3. DETECT: re-scan. Any AP that appeared in the gap is a candidate
 *      — a radio that switched on the instant the cable got power.
 *   4. SCORE each new AP against implant signatures + anomaly heuristics
 *      and render a CLEAN / SUSPICIOUS / DANGEROUS verdict.
 *
 * HONEST LIMITS: modern O.MG cables are deliberately stealthy — many
 * boot dormant (client mode, no beacon) and only light their AP when
 * remotely triggered, so a quiet cable is NOT proven safe. This tool
 * catches active/triggered implants and DIY ESP injectors, which is
 * most of what's in the wild, but "no detection" != "guaranteed safe".
 */
#include "app.h"
#include "../theme.h"
#include "ui.h"
#include "input.h"
#include "radio.h"
#include "ble_db.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_heap_caps.h>

#define UG_MAX 64

struct ug_ap_t {
    uint8_t bssid[6];
    char    ssid[33];
    int8_t  rssi;
    uint8_t channel;
    uint8_t auth;
    bool    hidden;
};

static ug_ap_t s_before[UG_MAX];
static int     s_before_n = 0;
static ug_ap_t s_after[UG_MAX];
static int     s_after_n = 0;

/* Raw-IDF scan into the given table. NEVER use Arduino WiFi.scanNetworks
 * after a raw-IDF init — it dup-creates the STA netif and panics (see
 * wifi_deauth_extras.cpp root-cause fix). */
static int ug_scan(ug_ap_t *out, int cap)
{
    wifi_scan_config_t scfg = {};
    scfg.show_hidden          = true;
    scfg.scan_type            = WIFI_SCAN_TYPE_ACTIVE;
    scfg.scan_time.active.min = 100;
    scfg.scan_time.active.max = 200;
    if (esp_wifi_scan_start(&scfg, true) != ESP_OK) return 0;
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n == 0) return 0;
    uint16_t want = n < cap ? n : cap;
    wifi_ap_record_t *recs = (wifi_ap_record_t *)heap_caps_malloc(
        want * sizeof(wifi_ap_record_t), MALLOC_CAP_8BIT);
    if (!recs) return 0;
    int got_n = 0;
    uint16_t got = want;
    if (esp_wifi_scan_get_ap_records(&got, recs) == ESP_OK) {
        for (int i = 0; i < (int)got && got_n < cap; ++i) {
            ug_ap_t &a = out[got_n++];
            memcpy(a.bssid, recs[i].bssid, 6);
            strncpy(a.ssid, (const char *)recs[i].ssid, sizeof(a.ssid) - 1);
            a.ssid[sizeof(a.ssid) - 1] = '\0';
            a.rssi    = recs[i].rssi;
            a.channel = recs[i].primary;
            a.auth    = (uint8_t)recs[i].authmode;
            a.hidden  = (a.ssid[0] == '\0');
        }
    }
    free(recs);
    return got_n;
}

static bool ug_seen_before(const uint8_t *bssid)
{
    for (int i = 0; i < s_before_n; ++i)
        if (memcmp(s_before[i].bssid, bssid, 6) == 0) return true;
    return false;
}

/* Implant signature / anomaly scoring. Returns 0-100 threat score and
 * fills `why` with the dominant reason. */
static int ug_score(const ug_ap_t &a, char *why, size_t why_sz)
{
    int score = 0;
    const char *reason = "new radio on plug-in";

    /* Any AP that appeared the instant the cable got power is inherently
     * suspicious — a normal cable has no radio. Baseline anomaly. */
    score += 35;

    uint32_t oui = ((uint32_t)a.bssid[0] << 16) |
                   ((uint32_t)a.bssid[1] << 8) | a.bssid[2];

    /* Espressif OUIs — ESP8266/ESP32 power most DIY keystroke injectors
     * and older O.MG / WiFi-Ducky builds. Strong implant indicator on a
     * device that should be a dumb cable. */
    static const uint32_t ESP_OUI[] = {
        0x18FE34, 0x240AC4, 0x3C7160, 0x7CDFA1, 0x8CAAB5,
        0xA4CF12, 0xB4E62D, 0xDC4F22, 0xE868E7, 0x84F3EB,
        0x2462AB, 0x10520C, 0x9C9C1F,
    };
    for (size_t i = 0; i < sizeof(ESP_OUI)/sizeof(ESP_OUI[0]); ++i) {
        if (oui == ESP_OUI[i]) {
            score += 45;
            reason = "Espressif radio (ESP implant)";
            break;
        }
    }

    /* Locally-administered BSSID — spoofed/random MAC, common on
     * implants hiding their real identity. */
    if (a.bssid[0] & 0x02) { score += 10; }

    /* Known malicious-cable SSID tells. */
    if (strcasestr(a.ssid, "O.MG") || strcasestr(a.ssid, "OMG_") ||
        strcasestr(a.ssid, "DuckyPad") || strcasestr(a.ssid, "WiFiDuck") ||
        strcasestr(a.ssid, "EvilCrow") || strcasestr(a.ssid, "Cactus") /* WiFi Cactus */) {
        score += 55;
        reason = "known implant SSID";
    }

    /* Hidden AP appearing exactly on plug-in — implants often run hidden
     * so a casual scan misses them. */
    if (a.hidden) { score += 15; if (score < 50) reason = "hidden AP on plug-in"; }

    /* Open auth on a freshly-appeared AP — implants default open for
     * easy C2 join. */
    if (a.auth == WIFI_AUTH_OPEN) { score += 5; }

    if (score > 100) score = 100;
    strncpy(why, reason, why_sz - 1);
    why[why_sz - 1] = '\0';
    return score;
}

void feat_usb_guard(void)
{
    radio_switch(RADIO_WIFI);
    if (!wifi_lean_sta_init()) { ui_toast("WiFi init failed", T_BAD, 1500); return; }

    auto &d = M5Cardputer.Display;

    /* ---- Phase 1: baseline ---- */
    ui_clear_body();
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2); d.print("CABLE GUARD");
    d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT);
    d.setTextColor(T_DIM, T_BG);
    d.setCursor(4, BODY_Y + 20); d.print("UNPLUG the suspect cable.");
    d.setCursor(4, BODY_Y + 32); d.print("baselining RF...");
    d.setCursor(4, BODY_Y + 44); d.print("scanning 2.4 GHz (1/3)");
    ui_draw_footer("scanning baseline");

    /* The IDF scan blocks, so it cannot animate mid call — draw one
     * radar frame as a persistent "scan in progress" marker. */
    ui_radar(SCR_W / 2, BODY_Y + 72, 18, T_ACCENT);
    s_before_n = ug_scan(s_before, UG_MAX);

    /* ---- Phase 2: prompt to plug ---- */
    ui_clear_body();
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2); d.print("CABLE GUARD");
    d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT);
    d.setTextColor(T_GOOD, T_BG);
    d.setCursor(4, BODY_Y + 22); d.printf("baseline: %d APs", s_before_n);
    d.setTextColor(T_WARN, T_BG);
    d.setCursor(4, BODY_Y + 40); d.print("NOW plug the suspect");
    d.setCursor(4, BODY_Y + 50); d.print("cable / charger in.");
    d.setTextColor(T_FG, T_BG);
    d.setCursor(4, BODY_Y + 66); d.print("ENTER = scan for implant");
    ui_draw_footer("ENTER=scan  `=cancel");
    while (true) {
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) { radio_switch(RADIO_NONE); return; }
        if (k == PK_ENTER) break;
    }

    /* Give the implant a moment to boot + start beaconing. */
    ui_clear_body();
    d.setTextColor(T_BAD, T_BG);
    d.setCursor(4, BODY_Y + 2); d.print("ANALYZING...");
    d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_BAD);
    ui_draw_footer("scanning");
    /* Animate the radar across the wait instead of freezing on one frame. */
    {
        uint32_t end = millis() + 1500;
        while ((int32_t)(end - millis()) > 0) {
            ui_radar(SCR_W / 2, BODY_Y + 50, 22, T_BAD);
            delay(50);
        }
    }

    /* ---- Phase 3: detect deltas, run twice to catch slow beacons ---- */
    d.setTextColor(T_DIM, T_BG);
    d.setCursor(4, BODY_Y + 78); d.print("scanning 2.4 GHz (2/3)");
    ui_radar(SCR_W / 2, BODY_Y + 50, 22, T_BAD);
    s_after_n = ug_scan(s_after, UG_MAX);
    delay(800);
    /* merge a second pass so a once-per-second beacon isn't missed */
    {
        ug_ap_t pass2[UG_MAX];
        d.setTextColor(T_DIM, T_BG);
        d.setCursor(4, BODY_Y + 78); d.print("scanning 2.4 GHz (3/3)");
        ui_radar(SCR_W / 2, BODY_Y + 50, 22, T_BAD);
        int p2 = ug_scan(pass2, UG_MAX);
        for (int i = 0; i < p2 && s_after_n < UG_MAX; ++i) {
            bool dup = false;
            for (int j = 0; j < s_after_n; ++j)
                if (memcmp(s_after[j].bssid, pass2[i].bssid, 6) == 0) { dup = true; break; }
            if (!dup) s_after[s_after_n++] = pass2[i];
        }
    }

    /* Collect new APs + scores. */
    struct hit_t { ug_ap_t ap; int score; char why[32]; };
    static hit_t hits[UG_MAX];
    int hit_n = 0;
    int worst = 0;
    for (int i = 0; i < s_after_n; ++i) {
        if (ug_seen_before(s_after[i].bssid)) continue;
        hit_t &h = hits[hit_n];
        h.ap = s_after[i];
        h.score = ug_score(s_after[i], h.why, sizeof(h.why));
        if (h.score > worst) worst = h.score;
        hit_n++;
        if (hit_n >= UG_MAX) break;
    }

    /* ---- Phase 4: verdict + list ---- */
    int cursor = 0;
    bool redraw = true;
    while (true) {
        if (redraw) {
            redraw = false;
            ui_clear_body();
            /* Verdict banner. */
            const char *verdict; uint16_t vc;
            if (hit_n == 0)        { verdict = "NO IMPLANT SEEN";  vc = T_GOOD; }
            else if (worst >= 70)  { verdict = "DANGEROUS";        vc = T_BAD;  }
            else if (worst >= 45)  { verdict = "SUSPICIOUS";       vc = T_WARN; }
            else                   { verdict = "LIKELY CLEAN";     vc = T_GOOD; }
            d.setTextColor(vc, T_BG);
            d.setCursor(4, BODY_Y + 2);
            d.printf("%s  (%d new)", verdict, hit_n);
            d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, vc);

            if (hit_n == 0) {
                d.setTextColor(T_DIM, T_BG);
                d.setCursor(4, BODY_Y + 22); d.print("no new radio appeared.");
                d.setCursor(4, BODY_Y + 34); d.print("active implants caught;");
                d.setCursor(4, BODY_Y + 44); d.print("stealth/dormant ones");
                d.setCursor(4, BODY_Y + 54); d.print("can still hide. not a");
                d.setCursor(4, BODY_Y + 64); d.print("guarantee of safety.");
            } else {
                int rows = 6;
                int first = cursor - rows / 2;
                if (first < 0) first = 0;
                if (first + rows > hit_n) first = (hit_n > rows) ? hit_n - rows : 0;
                for (int r = 0; r < rows && first + r < hit_n; ++r) {
                    const hit_t &h = hits[first + r];
                    int y = BODY_Y + 16 + r * 12;
                    bool sel = (first + r == cursor);
                    if (sel) d.fillRect(0, y - 1, SCR_W, 12, 0x18C7);
                    uint16_t sc = h.score >= 70 ? T_BAD : h.score >= 45 ? T_WARN : T_DIM;
                    d.setTextColor(sc, sel ? 0x18C7 : T_BG);
                    d.setCursor(2, y);  d.printf("%3d", h.score);
                    d.setTextColor(sel ? T_ACCENT : T_FG, sel ? 0x18C7 : T_BG);
                    d.setCursor(24, y);
                    d.printf("%.16s", h.ap.ssid[0] ? h.ap.ssid : "<hidden>");
                    d.setTextColor(T_DIM, sel ? 0x18C7 : T_BG);
                    d.setCursor(132, y); d.printf("c%u", h.ap.channel);
                }
            }
            ui_draw_footer(hit_n ? ";/.=move ENTER=info `=back" : "`=back");
        }
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) break;
        if (hit_n > 0) {
            if ((k == ';' || k == PK_UP)   && cursor > 0)        { cursor--; redraw = true; }
            if ((k == '.' || k == PK_DOWN) && cursor + 1 < hit_n){ cursor++; redraw = true; }
            if (k == PK_ENTER) {
                /* Per-hit detail. */
                const hit_t &h = hits[cursor];
                ui_force_clear_body();
                d.setTextColor(T_ACCENT, T_BG);
                d.setCursor(4, BODY_Y + 2); d.print("IMPLANT DETAIL");
                d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT);
                uint16_t sc = h.score >= 70 ? T_BAD : h.score >= 45 ? T_WARN : T_GOOD;
                d.setTextColor(sc, T_BG);
                d.setCursor(4, BODY_Y + 18); d.printf("THREAT %d/100", h.score);
                d.setTextColor(T_FG, T_BG);
                d.setCursor(4, BODY_Y + 30); d.printf("WHY  %.34s", h.why);
                d.setCursor(4, BODY_Y + 42);
                d.printf("SSID %.30s", h.ap.ssid[0] ? h.ap.ssid : "<hidden>");
                d.setCursor(4, BODY_Y + 54);
                d.printf("BSSID %02X:%02X:%02X:%02X:%02X:%02X",
                         h.ap.bssid[0], h.ap.bssid[1], h.ap.bssid[2],
                         h.ap.bssid[3], h.ap.bssid[4], h.ap.bssid[5]);
                uint32_t oui = ((uint32_t)h.ap.bssid[0] << 16) |
                               ((uint32_t)h.ap.bssid[1] << 8) | h.ap.bssid[2];
                const char *ven = (h.ap.bssid[0] & 0x02) ? "(random/spoofed)" : ble_db_oui(oui);
                d.setTextColor(T_ACCENT2, T_BG);
                d.setCursor(4, BODY_Y + 66); d.printf("VEN  %.32s", ven ? ven : "unknown");
                d.setTextColor(T_DIM, T_BG);
                d.setCursor(4, BODY_Y + 78); d.printf("ch%u  rssi %d  %s",
                         h.ap.channel, h.ap.rssi,
                         h.ap.auth == WIFI_AUTH_OPEN ? "OPEN" : "enc");
                ui_draw_footer("`=back");
                while (true) {
                    uint16_t k2 = input_poll();
                    if (k2 == PK_NONE) { delay(20); continue; }
                    if (k2 == PK_ESC) break;
                }
                redraw = true;
            }
        }
    }
    radio_switch(RADIO_NONE);
}
