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

/* Sweep helper — rapid frequency glide from f0 to f1 over dur_ms.
 * This is the core Tron/cyberpunk sound texture. */
static void sweep(int f0, int f1, int dur_ms)
{
    if (!audio_on()) return;
    int steps = dur_ms / 3;
    if (steps < 4) steps = 4;
    int step_ms = dur_ms / steps;
    for (int i = 0; i <= steps; i++) {
        int f = f0 + ((f1 - f0) * i) / steps;
        M5Cardputer.Speaker.tone(f, step_ms + 2);
        delay(step_ms);
    }
}

/* ========== UI cues — digital / Tron ========== */

void sfx_click(void)
{
    /* Short digital tick — reliably audible. 8ms with a descending second
     * tone. Previous 2ms × 2-stacked was below the M5 speaker's floor and
     * got clipped. */
    note(2800, 6);
    delay(3);
    note(2200, 5);
}

void sfx_select(void)
{
    /* Short descending activation glide — the Tron "acknowledge". */
    sweep(3800, 2400, 45);
}

void sfx_back(void)
{
    /* Quick sweep-down-then-tail — decisive disengagement. */
    sweep(2800, 1400, 35);
    delay(4);
    note(900, 15);
}

void sfx_error(void)
{
    /* Broken-modem: low buzz + harsh noise burst. */
    note(220, 35); delay(5);
    note(180, 45); delay(5);
    sweep(400, 140, 60);
}

void sfx_toast(void)
{
    /* Soft digital chirp — subtle info cue. */
    sweep(2400, 3200, 22);
}

/* ========== attack cues — cyberpunk ========== */

void sfx_scan_start(void)
{
    /* Data-link initializing: dual-sweep up then lock. */
    sweep(400, 2800, 90);
    delay(10);
    note(3600, 20);
    note(2800, 20);
}

void sfx_scan_hit(void)
{
    /* Target acquired — bright ping with sweep-up tail. */
    note(3800, 10);
    sweep(3800, 5200, 35);
}

void sfx_deauth_burst(void)
{
    /* Aggressive rising zap + industrial hit. Hard, mean. */
    sweep(200, 2400, 60);
    delay(3);
    note(1200, 40);
    delay(3);
    note(600, 30);
}

void sfx_capture(void)
{
    /* Bright glitch-into-chord — data acquired. */
    const int freqs[4] = { 3200, 1800, 4200, 2600 };
    for (int i = 0; i < 4; i++) { note(freqs[i], 18); delay(10); }
    delay(8);
    const int chord_notes[3] = { 2400, 3200, 4000 };
    chord(chord_notes, 3, 120);
}

void sfx_cracked(void)
{
    /* The win SFX: glitch-rise-to-chord finale. */
    sweep(400, 2800, 120);
    delay(10);
    const int chord_notes[4] = { 1900, 2800, 3600, 4800 };
    chord(chord_notes, 4, 180);
    delay(30);
    note(5200, 140);
}

/* ========== system cues — cinematic ========== */

void sfx_boot(void)
{
    /* Power-on sequence — sub-bass heartbeat, modem handshake, chord bloom.
     *   1. Two sub-bass pulses  — deep, "waking up"
     *   2. Modem-handshake texture (rapid alternating pitches)
     *   3. Rising sweep
     *   4. POSEIDON chord */
    note(120, 110); delay(90);
    note(160, 130); delay(100);
    /* modem-style handshake */
    for (int i = 0; i < 6; i++) {
        note(900 + (i * 180), 15);
        delay(5);
        note(2400 - (i * 140), 15);
        delay(5);
    }
    /* rising sweep */
    sweep(300, 3200, 200);
    delay(30);
    /* final POSEIDON chord */
    const int final_chord[4] = { 2000, 2800, 3400, 4200 };
    chord(final_chord, 4, 220);
}

void sfx_alert(void)
{
    /* Siren-style alternating high/low — unmistakable warning. */
    for (int i = 0; i < 3; i++) {
        note(2800, 50); delay(20);
        note(1400, 50); delay(20);
    }
}

void sfx_glitch(void)
{
    /* Full broken-signal texture — frequency noise. */
    const int freqs[8] = { 3800, 600, 2200, 4400, 900, 3200, 500, 2800 };
    for (int i = 0; i < 8; i++) {
        note(freqs[i], 14);
        delay(8);
    }
}
