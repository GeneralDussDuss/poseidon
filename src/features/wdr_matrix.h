/*
 * wdr_matrix — cinematic "matrix decode" render for the wardrive view.
 *
 * Full-screen matrix rain with real captured SSIDs decoding out of the
 * columns (glyph scramble resolving into the name) then popping into a
 * roster at the top. OPEN / WPA3 catches fire a "sick" full-width banner.
 *
 * Drawn every frame (no framebuffer, no heap alloc): fillScreen + rain +
 * decodes + roster + HUD, same profile as the WARDRIVE.cinema screensaver.
 * The wardrive loop owns capture + AP detection and feeds this module the
 * SSIDs; this module owns only the animation state.
 */
#pragma once
#include <stdint.h>

/* Reset animation + clear the roster. Call when switching INTO this view. */
void wdr_matrix_begin(void);

/* Preload the roster from already-seen APs (no decode, no banner). Call
 * once per AP oldest->newest right after begin() so the newest lands on
 * top. auth is the esp WIFI_AUTH_* value. */
void wdr_matrix_seed(const char *ssid, uint8_t auth, int8_t rssi);

/* A newly discovered AP arrived: queue it for a decode fly-in, and if it
 * is OPEN or WPA3 raise the catch banner. */
void wdr_matrix_feed(const char *ssid, uint8_t auth, int8_t rssi);

/* Render one frame. Call at ~40 ms cadence while this view is active. */
void wdr_matrix_render(uint8_t chan, int ap_count, bool gps_valid, uint8_t sats);
