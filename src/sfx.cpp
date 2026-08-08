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
#if defined(POSEIDON_BOARD_TEMBED)
#include "board/audio_tembed.h"   /* TEMBED_SINE32 waveform */
#endif
#include <M5Cardputer.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

/* POS-AUDIT-017 part 2: SFX runs on a dedicated player task fed by a
 * FreeRTOS queue. Public sfx_X functions enqueue a 1-byte event and
 * return immediately — no audio path blocks the caller. Previously
 * sfx_select (~45 ms), sfx_back (~40 ms), sfx_click (~8 ms) ran inline
 * on the loopTask from input_poll_raw on every keypress; ENTER → feature
 * had 45 ms of perceptible latency, rapid letter-mnemonic nav was
 * throttled to ~70 keys/sec. Notification SFX from WiFi RX paths
 * (POS-AUDIT-019 wifi_pmkid) used to block the loop while the radio
 * task continued firing handshakes — drop events here are accepted; the
 * SFX is decoration, not data. */
enum sfx_evt_t : uint8_t {
    SFX_E_CLICK = 0,
    SFX_E_SELECT,
    SFX_E_BACK,
    SFX_E_ERROR,
    SFX_E_TOAST,
    SFX_E_SCAN_START,
    SFX_E_SCAN_HIT,
    SFX_E_DEAUTH_BURST,
    SFX_E_CAPTURE,
    SFX_E_HS_CAPTURE,
    SFX_E_PMKID_CAPTURE,
    SFX_E_CRACKED,
    SFX_E_BOOT,
    SFX_E_ALERT,
    SFX_E_GLITCH,
    SFX_E_NAV_UP,
    SFX_E_NAV_DOWN,
};
static QueueHandle_t s_sfx_q = nullptr;
/* Set by sfx_nav() just before enqueueing; read by the player task. A
 * dropped/raced value only changes a chirp's pitch, never correctness. */
static volatile uint8_t s_nav_index = 0;
static volatile uint8_t s_nav_total = 1;
static void sfx_player_task(void *);  /* fwd: defined after play_X */

/* POS-AUDIT-017 part 1: NVS handle scoped per call rather than held
 * open from sfx_init for the lifetime of the firmware. The previous
 * static Preferences s_prefs held a read-write handle in NVS until
 * power-loss — writes via putUChar / putBool sat in the in-RAM cache
 * because we never called .end() to flush. Now every read/write opens
 * → operates → closes, matching theme.cpp's pattern. */
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
    if (s_inited) return;             /* idempotent — header says safe to call multiple times */
    Preferences p;
    if (p.begin("sfx", true)) {        /* read-only */
        s_volume = p.getUChar("vol", 5);
        s_mute   = p.getBool("mute", false);
        p.end();
    }
    if (s_volume > 10) s_volume = 10;
    apply_volume();

    /* Player task + queue. 8 events is enough — sfx are short and the
     * player drains fast. Stack 3 KB covers worst-case sfx_boot (deepest
     * call chain through sweep + chord, both stack-frame-only). Priority 1
     * so even idle UI tasks preempt it; we don't want SFX timing to
     * starve real work. */
    if (!s_sfx_q) {
        s_sfx_q = xQueueCreate(8, sizeof(uint8_t));
        if (s_sfx_q) {
            xTaskCreate(sfx_player_task, "sfx", 3072, nullptr, 1, nullptr);
        }
    }
    s_inited = true;
}

void sfx_set_volume(uint8_t vol)
{
    if (vol > 10) vol = 10;
    s_volume = vol;
    if (s_inited) {
        Preferences p;
        if (p.begin("sfx", false)) {
            p.putUChar("vol", vol);
            p.end();
        }
    }
    apply_volume();
}

uint8_t sfx_get_volume(void) { return s_volume; }

void sfx_set_mute(bool on)
{
    s_mute = on;
    if (s_inited) {
        Preferences p;
        if (p.begin("sfx", false)) {
            p.putBool("mute", on);
            p.end();
        }
    }
    apply_volume();
}

bool sfx_is_muted(void) { return s_mute; }

/* ========== internal helper ========== */

static inline bool audio_on(void)
{
    return !s_mute && s_volume > 0;
}

/* Every tone in this file routes through here so the waveform is chosen in
 * exactly one place. tone()'s default is a square wave, which is all odd
 * harmonics and reads as harsh and gritty through a small class-D amp. On
 * boards where we supply a sine wavetable, notes come out clean instead. */
static inline void speak(float freq, uint32_t dur_ms, bool restart = true)
{
#if defined(POSEIDON_BOARD_TEMBED)
    M5Cardputer.Speaker.tone(freq, dur_ms, 0, restart,
                             TEMBED_SINE32, sizeof(TEMBED_SINE32));
#else
    (void)restart;
    M5Cardputer.Speaker.tone(freq, dur_ms);
#endif
}

static void note(int freq, int dur_ms)
{
    if (!audio_on()) return;
    speak((float)freq, (uint32_t)dur_ms);
}

static void chord(const int *freqs, int n, int dur_ms)
{
    /* M5 Speaker doesn't mix multiple tones cleanly — simulate a chord
     * by rapidly cycling pitches. Cheap arpeggio. */
    if (!audio_on()) return;
    int each = dur_ms / n;
    if (each < 8) each = 8;
    for (int i = 0; i < n; i++) {
        speak((float)freqs[i], (uint32_t)each);
        delay(each);
    }
}

/* Sweep helper — rapid frequency glide from f0 to f1 over dur_ms.
 * This is the core Tron/cyberpunk sound texture. */
static void sweep(int f0, int f1, int dur_ms)
{
    if (!audio_on()) return;
    int steps = dur_ms / 2;   /* finer steps: glide, not staircase */
    if (steps < 6) steps = 6;
    int step_ms = dur_ms / steps;
    for (int i = 0; i <= steps; i++) {
        int f = f0 + ((f1 - f0) * i) / steps;
        speak((float)f, (uint32_t)(step_ms + 2), (i == 0));
        delay(step_ms);
    }
}

/* ========== UI cues — digital / Tron (player-task impls) ========== */

/* Two-tone chirp whose centre pitch rises with position in the list.
 * Short (~26ms) so fast scrolling stays crisp instead of turning into a
 * drone, and bent up or down so direction is audible without looking. */
static void play_nav(bool up)
{
    if (!audio_on()) return;
    uint8_t idx = s_nav_index, tot = s_nav_total ? s_nav_total : 1;
    if (idx >= tot) idx = tot - 1;
    /* Map position onto a bit over an octave, centred around 1.6kHz. */
    float t    = (tot > 1) ? ((float)idx / (float)(tot - 1)) : 0.5f;
    float base = 1200.0f + t * 1500.0f;
    float bend = up ? 1.28f : 0.78f;
    speak(base, 9);
    delay(7);
    speak(base * bend, 12);
}

static void play_click(void)
{
    /* Short digital tick — reliably audible. 8ms with a descending second
     * tone. Previous 2ms × 2-stacked was below the M5 speaker's floor and
     * got clipped. */
    note(2800, 6);
    delay(3);
    note(2200, 5);
}

static void play_select(void)
{
    /* Short descending activation glide — the Tron "acknowledge". */
    sweep(3800, 2400, 45);
}

static void play_back(void)
{
    /* Quick sweep-down-then-tail — decisive disengagement. */
    sweep(2800, 1400, 35);
    delay(4);
    note(900, 15);
}

static void play_error(void)
{
    /* Broken-modem: low buzz + harsh noise burst. */
    note(220, 35); delay(5);
    note(180, 45); delay(5);
    sweep(400, 140, 60);
}

static void play_toast(void)
{
    /* Soft digital chirp — subtle info cue. */
    sweep(2400, 3200, 22);
}

/* ========== attack cues — cyberpunk ========== */

static void play_scan_start(void)
{
    /* Data-link initializing: dual-sweep up then lock. */
    sweep(400, 2800, 90);
    delay(10);
    note(3600, 20);
    note(2800, 20);
}

static void play_scan_hit(void)
{
    /* Target acquired — bright ping with sweep-up tail. */
    note(3800, 10);
    sweep(3800, 5200, 35);
}

static void play_deauth_burst(void)
{
    /* Aggressive rising zap + industrial hit. Hard, mean. */
    sweep(200, 2400, 60);
    delay(3);
    note(1200, 40);
    delay(3);
    note(600, 30);
}

static void play_capture(void)
{
    /* Bright glitch-into-chord — data acquired. */
    const int freqs[4] = { 3200, 1800, 4200, 2600 };
    for (int i = 0; i < 4; i++) { note(freqs[i], 18); delay(10); }
    delay(8);
    const int chord_notes[3] = { 2400, 3200, 4000 };
    chord(chord_notes, 3, 120);
}

static void play_hs_capture(void)
{
    /* POS-AUDIT-019: full WPA2 4-way handshake captured — three-note
     * rising fanfare. Mirrors wifi_pmkid.cpp's previous inline tones. */
    note(1200, 120); delay(130);
    note(1800, 120); delay(130);
    note(2400, 240);
}

static void play_pmkid_capture(void)
{
    /* POS-AUDIT-019: PMKID grabbed — shorter double-beep. */
    note(1500, 80);  delay(90);
    note(2000, 120);
}

static void play_cracked(void)
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

static void play_boot(void)
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

static void play_alert(void)
{
    /* Siren-style alternating high/low — unmistakable warning. */
    for (int i = 0; i < 3; i++) {
        note(2800, 50); delay(20);
        note(1400, 50); delay(20);
    }
}

static void play_glitch(void)
{
    /* Full broken-signal texture — frequency noise. */
    const int freqs[8] = { 3800, 600, 2200, 4400, 900, 3200, 500, 2800 };
    for (int i = 0; i < 8; i++) {
        note(freqs[i], 14);
        delay(8);
    }
}

/* ========== player task + enqueue wrappers ========== */

static void sfx_player_task(void *_)
{
    uint8_t evt;
    while (1) {
        if (xQueueReceive(s_sfx_q, &evt, portMAX_DELAY) != pdTRUE) continue;
        /* Re-check audio_on() at play time — user may have muted between
         * enqueue and dequeue. play_X internals also check via note(). */
        if (!audio_on()) continue;
        switch (evt) {
        case SFX_E_CLICK:        play_click();        break;
        case SFX_E_SELECT:       play_select();       break;
        case SFX_E_BACK:         play_back();         break;
        case SFX_E_ERROR:        play_error();        break;
        case SFX_E_TOAST:        play_toast();        break;
        case SFX_E_SCAN_START:   play_scan_start();   break;
        case SFX_E_SCAN_HIT:     play_scan_hit();     break;
        case SFX_E_DEAUTH_BURST: play_deauth_burst(); break;
        case SFX_E_CAPTURE:      play_capture();      break;
        case SFX_E_HS_CAPTURE:    play_hs_capture();    break;
        case SFX_E_PMKID_CAPTURE: play_pmkid_capture(); break;
        case SFX_E_CRACKED:      play_cracked();      break;
        case SFX_E_BOOT:         play_boot();         break;
        case SFX_E_ALERT:        play_alert();        break;
        case SFX_E_NAV_UP:   play_nav(true);  break;
        case SFX_E_NAV_DOWN: play_nav(false); break;
        case SFX_E_GLITCH:       play_glitch();       break;
        default: break;
        }
    }
}

static inline void enqueue(uint8_t evt)
{
    /* Drop if queue full or not yet initialised — SFX is decoration. */
    if (!s_sfx_q) return;
    (void)xQueueSend(s_sfx_q, &evt, 0);
}

void sfx_nav(bool up, uint8_t index, uint8_t total)
{
    s_nav_index = index;
    s_nav_total = total ? total : 1;
    enqueue(up ? SFX_E_NAV_UP : SFX_E_NAV_DOWN);
}

void sfx_click(void)        { enqueue(SFX_E_CLICK); }
void sfx_select(void)       { enqueue(SFX_E_SELECT); }
void sfx_back(void)         { enqueue(SFX_E_BACK); }
void sfx_error(void)        { enqueue(SFX_E_ERROR); }
void sfx_toast(void)        { enqueue(SFX_E_TOAST); }
void sfx_scan_start(void)   { enqueue(SFX_E_SCAN_START); }
void sfx_scan_hit(void)     { enqueue(SFX_E_SCAN_HIT); }
void sfx_deauth_burst(void) { enqueue(SFX_E_DEAUTH_BURST); }
void sfx_capture(void)      { enqueue(SFX_E_CAPTURE); }
void sfx_hs_capture(void)    { enqueue(SFX_E_HS_CAPTURE); }
void sfx_pmkid_capture(void) { enqueue(SFX_E_PMKID_CAPTURE); }
void sfx_cracked(void)      { enqueue(SFX_E_CRACKED); }
void sfx_boot(void)         { enqueue(SFX_E_BOOT); }
void sfx_alert(void)        { enqueue(SFX_E_ALERT); }
void sfx_glitch(void)       { enqueue(SFX_E_GLITCH); }
