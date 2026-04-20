/*
 * subghz_types — shared across sub-GHz features so scan can hand off a
 * captured waveform to replay / broadcast without forcing an SD save.
 *
 * Producer: subghz_scan (fills on successful capture).
 * Consumers: subghz_replay (offers "last captured" as a play option),
 *            subghz_record (uses as a starting waveform if no fresh RX).
 *
 * No synchronisation — radio mutex keeps producer + consumer from
 * running concurrently.
 */
#pragma once

#include <Arduino.h>

#define SUBGHZ_MAX_PULSES 512

struct subghz_capture_t {
    int16_t  pulses[SUBGHZ_MAX_PULSES];
    int      pulse_count;
    float    freq_mhz;
    uint32_t ts;         /* millis() at capture */
};

extern subghz_capture_t g_subghz_last_cap;
extern bool             g_subghz_last_valid;
