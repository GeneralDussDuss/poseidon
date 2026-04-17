/*
 * gps.cpp — minimal NMEA parser (GGA + RMC).
 */
#include "gps.h"
#include <HardwareSerial.h>

static HardwareSerial s_uart(1);
static gps_fix_t s_fix = {};
static char s_line[128];
static int  s_line_len = 0;
static gps_diag_t s_diag = {};

const gps_diag_t &gps_diag(void) { return s_diag; }

static const uint32_t BAUD_CYCLE[] = { 9600, 115200, 38400, 4800, 19200, 57600 };
static size_t  s_baud_idx = 0;
static uint32_t s_baud    = GPS_BAUD;

static bool s_started = false;

bool gps_begin(void)
{
    if (s_started) return true;
    s_uart.begin(s_baud, SERIAL_8N1, GPS_UART_RX_PIN, GPS_UART_TX_PIN);
    s_started = true;
    return true;
}

void gps_end(void)
{
    if (!s_started) return;
    s_uart.end();
    s_started = false;
}

uint32_t gps_current_baud(void) { return s_baud; }

static volatile bool s_pause_poll = false;

uint32_t gps_cycle_baud(void)
{
    s_pause_poll = true;  /* pause gps_task to avoid UART read during teardown */
    delay(20);
    s_baud_idx = (s_baud_idx + 1) % (sizeof(BAUD_CYCLE) / sizeof(BAUD_CYCLE[0]));
    s_baud = BAUD_CYCLE[s_baud_idx];
    s_uart.end();
    s_started = false;
    s_uart.begin(s_baud, SERIAL_8N1, GPS_UART_RX_PIN, GPS_UART_TX_PIN);
    s_started = true;
    s_diag = {};
    s_line_len = 0;
    s_pause_poll = false;
    Serial.printf("[gps] baud -> %lu\n", (unsigned long)s_baud);
    return s_baud;
}

const gps_fix_t &gps_get(void) { return s_fix; }

bool gps_snapshot(gps_fix_t *out)
{
    if (!out || !s_fix.valid) return false;
    *out = s_fix;
    return true;
}

/* ---- helpers ---- */

static double nmea_to_degrees(const char *s, char hemi)
{
    /* NMEA format: DDMM.mmmm or DDDMM.mmmm */
    if (!s || !*s) return 0.0;
    const char *dot = strchr(s, '.');
    int pre = dot ? (int)(dot - s) : (int)strlen(s);
    if (pre < 4) return 0.0;
    int deg_digits = pre - 2;
    char deg_buf[4] = {0};
    memcpy(deg_buf, s, deg_digits);
    double deg = atof(deg_buf);
    double min = atof(s + deg_digits);
    double out = deg + min / 60.0;
    if (hemi == 'S' || hemi == 'W') out = -out;
    return out;
}

static int split_csv(char *line, char **fields, int max)
{
    int n = 0;
    fields[n++] = line;
    for (char *p = line; *p && n < max; ++p) {
        if (*p == ',') { *p = '\0'; fields[n++] = p + 1; }
        if (*p == '*') { *p = '\0'; break; }
    }
    return n;
}

static void parse_gga(char *line)
{
    char *f[16] = {0};
    int n = split_csv(line, f, 16);
    if (n < 10) return;
    int fix_quality = atoi(f[6]);
    if (fix_quality < 1) return;
    s_fix.lat_deg = nmea_to_degrees(f[2], f[3][0]);
    s_fix.lon_deg = nmea_to_degrees(f[4], f[5][0]);
    s_fix.sats    = (uint8_t)atoi(f[7]);
    s_fix.hdop    = (float)atof(f[8]);
    s_fix.alt_m   = (float)atof(f[9]);
    strncpy(s_fix.utc, f[1], sizeof(s_fix.utc) - 1);
    s_fix.valid   = true;
    s_fix.time_ms = millis();
}

static void parse_rmc(char *line)
{
    char *f[16] = {0};
    int n = split_csv(line, f, 16);
    if (n < 10) return;
    if (f[2][0] != 'A') return;  /* A = active, V = void */
    s_fix.lat_deg    = nmea_to_degrees(f[3], f[4][0]);
    s_fix.lon_deg    = nmea_to_degrees(f[5], f[6][0]);
    s_fix.speed_kts  = (float)atof(f[7]);
    s_fix.course_deg = (float)atof(f[8]);
    strncpy(s_fix.date, f[9], sizeof(s_fix.date) - 1);
    s_fix.valid   = true;
    s_fix.time_ms = millis();
}

static void process_line(char *line)
{
    s_diag.lines++;
    strncpy(s_diag.last, line, sizeof(s_diag.last) - 1);
    s_diag.last[sizeof(s_diag.last) - 1] = '\0';
    if (strncmp(line, "$GPGGA", 6) == 0 || strncmp(line, "$GNGGA", 6) == 0) {
        s_diag.gga++;
        parse_gga(line);
    } else if (strncmp(line, "$GPRMC", 6) == 0 || strncmp(line, "$GNRMC", 6) == 0) {
        s_diag.rmc++;
        parse_rmc(line);
    }
}

void gps_poll(void)
{
    if (s_pause_poll || !s_started) return;
    while (s_uart.available()) {
        int c = s_uart.read();
        if (c < 0) break;
        s_diag.bytes++;
        if (c == '\r') continue;
        if (c == '\n') {
            s_line[s_line_len] = '\0';
            if (s_line_len > 6) process_line(s_line);
            s_line_len = 0;
            continue;
        }
        if (s_line_len + 1 < (int)sizeof(s_line)) {
            s_line[s_line_len++] = (char)c;
        } else {
            s_line_len = 0;  /* overflow, discard */
        }
    }

    /* Age out stale fixes after 10 seconds without an update. */
    if (s_fix.valid && millis() - s_fix.time_ms > 10000) {
        s_fix.valid = false;
    }
}
