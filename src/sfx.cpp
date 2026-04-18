/*
 * sfx — speaker SFX engine, driven off M5Cardputer.Speaker.tone().
 *
 * Design principles:
 *   - Non-overbearing: every SFX clamps to ≤120ms total airtime
 *   - Non-blocking where plausible: `tone()` schedules internally; we
 *     chain notes with tiny delays only when sequencing needs it
 *   - Fails silent: if speaker init failed or mute is on, every call is
 *     a no-op, never panics
 */
#include "sfx.h"
#include <M5Cardputer.h>
#include <Preferences.h>

static Preferences s_prefs;
static uint8_t s_volume = 5;     /* 0..10 user-facing */
static bool    s_mute = false;
static bool    s_inited = false;

static inline uint8_t user_to_m5(uint8_t u)
{
    /* M5 speaker volume is 0..255. Map 0..10 exponentially so the low
     * end doesn't just jump from silent to loud. */
    if (u == 0) return 0;
    static const uint8_t curve[11] = { 0, 8, 18, 32, 48, 72, 100, 140, 180, 220, 255 };
    if (u > 10) u = 10;
    return curve[u];
}

static void apply_volume(void)
{
    uint8_t v = s_mute ? 0 : user_to_m5(s_volume);
    M5Cardputer.Speaker.setVolume(v);
}

void sfx_init(void)
{
    s_prefs.begin("sfx", false);
    s_volume = s_prefs.getUChar("vol", 5);
    s_mute   = s_prefs.getBool("mute", false);
    if (s_volume > 10) s_volume = 10;
    apply_volume();
    s_inited = true;
}

void sfx_set_volume(uint8_t vol)
{
    if (vol > 10) vol = 10;
    s_volume = vol;
    if (s_inited) s_prefs.putUChar("vol", vol);
    apply_volume();
}

uint8_t sfx_get_volume(void) { return s_volume; }

void sfx_set_mute(bool on)
{
    s_mute = on;
    if (s_inited) s_prefs.putBool("mute", on);
    apply_volume();
}

bool sfx_is_muted(void) { return s_mute; }

/* ========== internal helper ========== */

static inline bool audio_on(void)
{
    return !s_mute && s_volume > 0;
}

static void note(int freq, int dur_ms)
{
    if (!audio_on()) return;
    M5Cardputer.Speaker.tone(freq, dur_ms);
}

static void chord(const int *freqs, int n, int dur_ms)
{
    /* M5 Speaker doesn't mix multiple tones cleanly — simulate a chord
     * by rapidly cycling pitches. Cheap arpeggio. */
    if (!audio_on()) return;
    int each = dur_ms / n;
    if (each < 8) each = 8;
    for (int i = 0; i < n; i++) {
        M5Cardputer.Speaker.tone(freqs[i], each);
        delay(each);
    }
}

/* ========== UI cues — subtle ========== */

void sfx_click(void)
{
    note(1800, 4);  /* crisp 4ms tick */
}

void sfx_select(void)
{
    note(2400, 12);
    delay(8);
    note(3200, 12);
}

void sfx_back(void)
{
    note(1800, 12);
    delay(6);
    note(1200, 16);
}

void sfx_error(void)
{
    note(500, 40);
    delay(10);
    note(300, 60);
}

void sfx_toast(void)
{
    note(2600, 18);
}

/* ========== attack cues — distinctive ========== */

void sfx_scan_start(void)
{
    /* Ascending sweep — "stalking" feel */
    note(800, 20); delay(15);
    note(1200, 20); delay(15);
    note(1800, 25);
}

void sfx_scan_hit(void)
{
    /* Bright little ping */
    note(3000, 14);
    delay(8);
    note(3800, 18);
}

void sfx_deauth_burst(void)
{
    /* Aggressive low-mid pulse — quick, hits hard */
    note(2200, 20);
    delay(4);
    note(1600, 30);
}

void sfx_capture(void)
{
    /* Triumphant 3-note ascending — handshake / PMKID grabbed */
    const int notes[3] = { 1600, 2400, 3600 };
    for (int i = 0; i < 3; i++) {
        note(notes[i], 70);
        delay(55);
    }
}

void sfx_cracked(void)
{
    /* Bigger capture — 4-note major arpeggio */
    const int notes[4] = { 1500, 1900, 2250, 3000 };
    for (int i = 0; i < 4; i++) {
        note(notes[i], 60);
        delay(45);
    }
    delay(30);
    note(3600, 110);
}

/* ========== system cues ========== */

void sfx_boot(void)
{
    /* Deep wave + glitch shimmer + title bloom.
     *   1. Two low rumbles (the deep)
     *   2. Ascending glitch sweep
     *   3. Bright chord (POSEIDON rising) */
    note(180, 90);  delay(60);
    note(240, 110); delay(80);
    /* glitch sweep */
    for (int f = 600; f <= 2400; f += 240) {
        note(f, 12);
        delay(10);
    }
    delay(40);
    const int chord_notes[3] = { 2400, 3000, 3600 };
    chord(chord_notes, 3, 180);
}

void sfx_alert(void)
{
    /* Two fast descending pulses — "something's wrong" */
    note(2800, 40); delay(40);
    note(2400, 40); delay(40);
    note(2800, 40); delay(40);
    note(2400, 60);
}

void sfx_glitch(void)
{
    /* Rapid frequency walk — hacker-movie vibe */
    const int freqs[6] = { 1800, 700, 2600, 900, 3400, 1400 };
    for (int i = 0; i < 6; i++) {
        note(freqs[i], 18);
        delay(10);
    }
}
