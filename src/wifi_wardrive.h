/*
 * wifi_wardrive — public globals.
 *
 * Triton (and other WiFi-centric features) can seed their internal
 * BSSID→SSID resolver from wardrive's accumulated AP table instead of
 * waiting to catch beacons from scratch. Single-producer / many-reader;
 * safe because radio mutex prevents wardrive + consumers running
 * concurrently.
 */
#pragma once

#include <Arduino.h>

#define WARDRIVE_MAX_APS 256

struct wdr_ap_t {
    uint8_t  bssid[6];
    char     ssid[33];
    int8_t   rssi;
    uint8_t  channel;
    uint8_t  auth;
    double   lat;
    double   lon;
    float    alt;
    uint32_t first_seen;
    uint32_t last_seen;
    bool     dirty;
};

/* heap-001: was static BSS (20 KB). Now allocated on first wardrive
 * call — other features (Triton, wifi_scan) read it so it persists
 * for the session.  g_wdr_aps_init() allocates; never freed. */
extern wdr_ap_t *g_wdr_aps;
extern int       g_wdr_ap_count;
bool g_wdr_aps_init(void);  /* idempotent — returns false on OOM */
