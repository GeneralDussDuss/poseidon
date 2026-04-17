/*
 * gps — NMEA parser for the M5Stack LoRa-GNSS HAT.
 *
 * Hardware: HAT sits on the Cardputer's expansion header and exposes
 * the u-blox GPS module on UART1. Cardputer ADV pinout:
 *   GPIO 15 = GPS TX  (module → MCU)
 *   GPIO 13 = GPS RX  (MCU → module)
 *   9600 baud, 8N1
 *
 * We poll the UART every 100ms, parse GGA + RMC sentences, keep the
 * last valid fix in a global struct. Non-blocking — safe to call from
 * any task.
 */
#pragma once

#include <Arduino.h>

#define GPS_UART_RX_PIN 15
#define GPS_UART_TX_PIN 13
#define GPS_BAUD        9600

struct gps_fix_t {
    bool     valid;          /* true when we have a 3D fix */
    double   lat_deg;        /* signed degrees */
    double   lon_deg;
    float    alt_m;
    float    speed_kts;
    float    course_deg;
    uint8_t  sats;
    float    hdop;
    uint32_t time_ms;        /* millis() when last updated */
    char     utc[12];        /* HHMMSS.ss */
    char     date[8];        /* DDMMYY */
};

bool gps_begin(void);
void gps_end(void);
void gps_poll(void);
/* Restart the UART at a different baud. Returns the new baud. The
 * cycle covers every rate the ATGM336H/AT6668 datasheets list. */
uint32_t gps_cycle_baud(void);
uint32_t gps_current_baud(void);
const gps_fix_t &gps_get(void);

/* Convenience: snapshot for CSV writers. Returns false if no fix yet. */
bool gps_snapshot(gps_fix_t *out);

/* Diagnostics: bytes / lines / sentences seen since boot, plus the most
 * recent NMEA line. Used by the GPS fix page so the user can tell
 * whether the UART is wired right vs no sky view. */
struct gps_diag_t {
    uint32_t bytes;
    uint32_t lines;
    uint32_t gga;
    uint32_t rmc;
    char     last[96];
};
const gps_diag_t &gps_diag(void);
