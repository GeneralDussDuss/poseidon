/*
 * feat_satcom — satellite tracker UI.
 *
 * Flow:
 *   1. Pick from SATCOM_FAVORITES (or paste a NORAD ID — later)
 *   2. Fetch TLE (HTTPS to Celestrak, fall back to SD cache)
 *   3. Live tracking screen: polar skyplot, az/el, lat/lon, alt
 *   4. ENTER → pass-predict screen: next 24h AOS/LOS list
 *
 * Observer location: GPS fix (background gps_poll task), or zero if
 * no fix yet (sat still computed using its own ephemeris, but az/el
 * relative to (0,0) — UI badges this).
 *
 * Time source: system epoch (set by NTP after WiFi connect, or GPS).
 */
#include "app.h"
#include "../theme.h"
#include "ui.h"
#include "input.h"
#include "radio.h"
#include "gps.h"
#include "sfx.h"
#include "../satcom.h"
#include <math.h>
#include <time.h>

static int s_sel_idx = 0;
static satcom_tle_t s_tle = {};

static void draw_skyplot(int cx, int cy, int radius, const satcom_pos_t &p)
{
    auto &d = M5Cardputer.Display;
    /* Concentric rings: outer = horizon (el=0), inner = el=45, dot = zenith (el=90). */
    d.drawCircle(cx, cy, radius,      T_DIM);
    d.drawCircle(cx, cy, radius / 2,  0x2104);
    d.drawPixel(cx, cy, T_DIM);
    /* Cardinal markers. */
    d.setTextColor(T_DIM, T_BG);
    d.setCursor(cx - 2, cy - radius - 8); d.print("N");
    d.setCursor(cx + radius + 2, cy - 3); d.print("E");
    d.setCursor(cx - 2, cy + radius + 2); d.print("S");
    d.setCursor(cx - radius - 8, cy - 3); d.print("W");
    /* Sat dot — only if above horizon. Az: 0=N=top, 90=E=right, etc.
     * r = (90 - el) / 90 * radius  → zenith at center, horizon at edge. */
    if (p.valid && p.el_deg > 0) {
        double az_rad = p.az_deg * (M_PI / 180.0) - M_PI / 2.0;  /* rotate so 0=top */
        double r_pct  = (90.0 - p.el_deg) / 90.0;
        int sx = cx + (int)(cos(az_rad) * radius * r_pct);
        int sy = cy + (int)(sin(az_rad) * radius * r_pct);
        d.fillCircle(sx, sy, 3, T_ACCENT);
        d.drawCircle(sx, sy, 5, T_ACCENT2);
    }
}

static void track_screen(void)
{
    auto &d = M5Cardputer.Display;
    ui_clear_body();
    ui_draw_footer("ENTER=passes  R=refresh TLE  `=back");

    uint32_t last = 0;
    bool dirty = true;
    while (true) {
        gps_poll();
        uint32_t now = millis();
        if (now - last > 500) {
            last = now;
            if (dirty) {
                ui_clear_body();
                d.setTextColor(T_ACCENT, T_BG);
                d.setCursor(4, BODY_Y + 2); d.printf("%-20s", s_tle.name);
                d.setTextColor(T_DIM, T_BG);
                d.setCursor(SCR_W - 60, BODY_Y + 2); d.printf("#%lu", (unsigned long)s_tle.norad);
                d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT);
                dirty = false;
            }
            const gps_fix_t &g = gps_get();
            double obs_lat = g.valid ? g.lat_deg : 0.0;
            double obs_lon = g.valid ? g.lon_deg : 0.0;
            double obs_alt = g.valid ? g.alt_m   : 0.0;
            uint32_t utc = (uint32_t)time(nullptr);

            /* M1: SGP4 with utc=0 (1970 epoch) propagates against a
             * massive negative tsince and produces garbage. Guard
             * against showing "live" data until we have a real clock. */
            bool time_valid = utc > 1000000000UL;
            satcom_pos_t p = {};
            if (time_valid) {
                satcom_compute(&s_tle, obs_lat, obs_lon, obs_alt, utc, &p);
            }

            /* Skyplot — left half. */
            int cx = 38, cy = BODY_Y + 60, R = 32;
            d.fillRect(0, BODY_Y + 18, 80, 80, T_BG);
            if (time_valid) {
                draw_skyplot(cx, cy, R, p);
            } else {
                d.setTextColor(T_BAD, T_BG);
                d.setCursor(4, cy - 4);
                d.print("NO TIME LOCK");
                d.setTextColor(T_DIM, T_BG);
                d.setCursor(4, cy + 6);
                d.print("connect WiFi");
                d.setCursor(4, cy + 16);
                d.print("or GPS fix");
            }

            /* Stats — right half. */
            d.fillRect(82, BODY_Y + 18, SCR_W - 82, BODY_H - 30, T_BG);
            d.setTextColor(p.el_deg > 0 ? T_GOOD : T_DIM, T_BG);
            d.setCursor(86, BODY_Y + 20);
            d.printf("EL %+6.2f", p.el_deg);
            d.setTextColor(T_FG, T_BG);
            d.setCursor(86, BODY_Y + 32); d.printf("AZ %6.2f",  p.az_deg);
            d.setCursor(86, BODY_Y + 44); d.printf("LAT %6.2f", p.lat_deg);
            d.setCursor(86, BODY_Y + 56); d.printf("LON %6.2f", p.lon_deg);
            d.setCursor(86, BODY_Y + 68); d.printf("ALT %4.0fk",p.alt_km);
            d.setCursor(86, BODY_Y + 80); d.printf("R   %4.0fk",p.range_km);

            /* Observer state badge. */
            d.setTextColor(g.valid ? T_GOOD : T_WARN, T_BG);
            d.setCursor(4, BODY_Y + BODY_H - 8);
            d.print(g.valid ? "OBS: GPS LOCK" : "OBS: NO FIX (using 0,0)");
        }

        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) return;
        if (k == 'r' || k == 'R') {
            ui_toast("refreshing TLE...", T_ACCENT, 800);
            /* Blocking HTTPS GET (up to ~8s). Synchronous, so it cannot
             * animate — draw one labeled spinner frame before it. */
            ui_clear_body();
            d.setTextColor(T_ACCENT, T_BG);
            d.setCursor(4, BODY_Y + 2); d.print("SATCOM");
            d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT);
            d.setTextColor(T_DIM, T_BG);
            d.setCursor(4, BODY_Y + 30); d.print("fetching TLE...");
            ui_spinner(SCR_W / 2, BODY_Y + 56, T_ACCENT);
            satcom_fetch_tle(s_tle.norad, &s_tle);
            dirty = true;
        }
        if (k == PK_ENTER) {
            /* Pass-predict screen */
            ui_clear_body();
            d.setTextColor(T_ACCENT, T_BG);
            d.setCursor(4, BODY_Y + 2); d.print("NEXT 24H PASSES");
            d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT);

            const gps_fix_t &g = gps_get();
            double obs_lat = g.valid ? g.lat_deg : 0.0;
            double obs_lon = g.valid ? g.lon_deg : 0.0;
            double obs_alt = g.valid ? g.alt_m   : 0.0;
            uint32_t utc = (uint32_t)time(nullptr);

            /* The SGP4 sweep is a synchronous ~2880 step loop; it cannot
             * animate, so draw one labeled spinner frame so the screen
             * does not read as frozen. (Low risk option: keeps satcom.cpp
             * UI free rather than wiring a repaint into the compute loop.) */
            d.setTextColor(T_DIM, T_BG);
            d.setCursor(4, BODY_Y + 20); d.print("computing passes...");
            ui_spinner(SCR_W / 2, BODY_Y + 54, T_ACCENT);

            satcom_pass_t passes[6];
            int np = satcom_predict_passes(&s_tle, obs_lat, obs_lon, obs_alt,
                                           utc, 86400, 10.0, passes, 6);

            /* Wipe the spinner frame before rendering the pass list. */
            d.fillRect(0, BODY_Y + 16, SCR_W, BODY_H - 24, T_BG);
            if (np == 0) {
                d.setTextColor(T_DIM, T_BG);
                d.setCursor(4, BODY_Y + 20);
                d.print("no passes >10deg in next 24h");
            } else {
                for (int i = 0; i < np; i++) {
                    int y = BODY_Y + 20 + i * 14;
                    /* H5 fix: guard against aos_ts < utc underflow. Can
                     * happen if the predict ran a few seconds ago and
                     * the first pass is happening basically now. */
                    uint32_t aos_in_sec = (passes[i].aos_ts > utc)
                                          ? (passes[i].aos_ts - utc) : 0;
                    int h = aos_in_sec / 3600;
                    int m = (aos_in_sec / 60) % 60;
                    uint32_t dur = (passes[i].los_ts > passes[i].aos_ts)
                                   ? (passes[i].los_ts - passes[i].aos_ts) : 0;
                    d.setTextColor(T_FG, T_BG);
                    d.setCursor(4, y);
                    d.printf("+%02dh%02dm  %3.0f° max  %2lus dur",
                             h, m, passes[i].max_el_deg, (unsigned long)dur);
                }
            }
            ui_draw_footer("`=back");
            while (true) {
                uint16_t k2 = input_poll();
                if (k2 == PK_ESC) break;
                delay(30);
            }
            dirty = true;
            ui_draw_footer("ENTER=passes  R=refresh TLE  `=back");
        }
    }
}

static bool pick_favorite(void)
{
    auto &d = M5Cardputer.Display;
    int cursor = s_sel_idx;
    int prev = -1;
    ui_draw_footer(";/. pick  ENTER=lock  `=back");
    while (true) {
        if (cursor != prev) {
            prev = cursor;
            ui_clear_body();
            d.setTextColor(T_ACCENT2, T_BG);
            d.setCursor(4, BODY_Y + 2); d.print("PICK SATELLITE");
            d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT2);
            const int rows = 8;
            int first = cursor - rows / 2;
            if (first < 0) first = 0;
            if (first + rows > SATCOM_FAVORITES_N)
                first = max(0, SATCOM_FAVORITES_N - rows);
            for (int r = 0; r < rows && first + r < SATCOM_FAVORITES_N; r++) {
                int i = first + r;
                int y = BODY_Y + 18 + r * 12;
                bool sel = (i == cursor);
                if (sel) d.fillRect(0, y - 1, SCR_W, 12, 0x3007);
                d.setTextColor(sel ? T_ACCENT : T_FG, sel ? 0x3007 : T_BG);
                d.setCursor(4, y);
                d.printf("#%-6lu %.24s",
                         (unsigned long)SATCOM_FAVORITES[i].norad,
                         SATCOM_FAVORITES[i].name);
            }
        }
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(30); continue; }
        if (k == PK_ESC) return false;
        if (k == ';' || k == PK_UP)   { if (cursor > 0) cursor--; }
        if (k == '.' || k == PK_DOWN) { if (cursor < SATCOM_FAVORITES_N - 1) cursor++; }
        if (k == PK_ENTER) { s_sel_idx = cursor; return true; }
    }
}

void feat_satcom(void)
{
    /* Stay on RADIO_WIFI so HTTPClient can hit Celestrak. The Cardputer
     * may not be connected; that's fine — fetch_tle falls back to SD
     * cache if WiFi is down. */
    radio_switch(RADIO_WIFI);

    if (!pick_favorite()) return;

    uint32_t norad = SATCOM_FAVORITES[s_sel_idx].norad;
    ui_clear_body();
    auto &d = M5Cardputer.Display;
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2); d.print("SATCOM");
    d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT);
    d.setTextColor(T_FG, T_BG);
    d.setCursor(4, BODY_Y + 22);
    d.printf("Loading %.20s...", SATCOM_FAVORITES[s_sel_idx].name);
    /* fetch may block (HTTPS GET up to ~8s if it falls through to live);
     * it is synchronous so it cannot animate — draw one spinner frame. */
    d.setTextColor(T_DIM, T_BG);
    d.setCursor(4, BODY_Y + 34); d.print("fetching TLE...");
    ui_spinner(SCR_W / 2, BODY_Y + 62, T_ACCENT);

    /* Offline-first: TLEs are baked into firmware via
     * scripts/fetch_satcom_tles.py. No WiFi connection required at
     * runtime. SD cache (if any) wins over baked; baked wins over
     * live (which only fires if WiFi is already up — never auto). */
    if (!satcom_fetch_tle(norad, &s_tle)) {
        /* Clear the "fetching" spinner frame before the error text. */
        d.fillRect(0, BODY_Y + 30, SCR_W, BODY_H - 38, T_BG);
        d.setTextColor(T_BAD, T_BG);
        d.setCursor(4, BODY_Y + 40);
        d.print("No baked TLE for this NORAD.");
        d.setTextColor(T_DIM, T_BG);
        d.setCursor(4, BODY_Y + 52);
        d.print("Add it to scripts/fetch_satcom_tles.py");
        d.setCursor(4, BODY_Y + 62);
        d.print("FAVORITES list, re-run, reflash.");
        ui_draw_footer("`=back");
        while (true) {
            uint16_t k = input_poll();
            if (k == PK_ESC) return;
            delay(30);
        }
    }

    track_screen();
}
