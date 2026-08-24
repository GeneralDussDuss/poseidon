/*
 * leds_tembed - theme-driven WS2812B ring around the T-Embed's rotary
 * encoder (8 LEDs, GPIO 14, GRB order). Every colour is pulled from the
 * active theme (src/theme.h) at render time — never hardcoded — so all
 * six palettes look correct without touching this file.
 *
 * leds_tick() is non-blocking and cheap: it reads millis(), skips work
 * if less than ~16ms has elapsed, and does a handful of float ops over
 * 8 LEDs. Call it from the UI loop once it's wired in (see report).
 */
#pragma once

#include <stdint.h>

#if defined(POSEIDON_BOARD_TEMBED)

typedef enum {
    LED_MODE_IDLE = 0,   /* dual-tone wave: T_ACCENT + T_ACCENT2 drift at different
                          * rates, low peak brightness, never syncs/repeats visibly */
    LED_MODE_ACTIVE,     /* comet: bright head + fading tail, ~2s/rev, T_ACCENT */
    LED_MODE_SCAN,        /* radar sweep: fast orbit + dim persistence trail, T_ACCENT2 */
    LED_MODE_ALERT,       /* strobe T_BAD / off @ ~4Hz */
    LED_MODE_RSSI,         /* signal-strength meter: leds_set_rssi() drives fill + heat + pulse */
    LED_MODE_CHANNEL,      /* position around ring encodes WiFi channel, leds_set_channel() */
    LED_MODE_TRAFFIC,      /* live packet monitor: leds_packet_hit() injects decaying energy */
    LED_MODE_ATTACK,       /* two counter-rotating comets, T_WARN/T_BAD, aggressive */
} led_mode_t;

typedef enum {
    LED_EVENT_NAV_CW = 0,  /* brief chase pulse, clockwise travel direction */
    LED_EVENT_NAV_CCW,     /* brief chase pulse, counter-clockwise travel direction */
    LED_EVENT_SELECT,      /* all 8 flash T_GOOD, decay over ~300ms */
    LED_EVENT_BACK,        /* dim reverse sweep, T_DIM */
    LED_EVENT_BOOT,        /* rainbow spin-up, one full revolution, for splash */
    LED_EVENT_HIT,          /* capture/handshake landed: flash expands from last traffic
                             * hit outward both directions, resolves to a full-ring T_GOOD pulse */
    LED_EVENT_ALERT_SPIN,   /* fast full-ring spin in T_BAD, for detection/alert screens */
} led_event_t;

/* Inits the RMT peripheral on TE_LED_PIN and clears the ring. Safe to
 * call once at boot. */
void leds_begin(void);

/* Drives whatever animation/event is currently active. Call every UI
 * loop iteration — internally rate-limited to ~60fps, no delays. */
void leds_tick(void);

/* Switches the persistent background animation. Takes effect on the
 * next leds_tick() once any in-flight one-shot event finishes. */
void leds_set_mode(led_mode_t m);

/* Fires a one-shot reaction that overlays the current mode until it
 * finishes, then the mode resumes. */
void leds_event(led_event_t e);

/* Global brightness cap, 0..255. Starts at 40 so the ring is present
 * but never blinding. All animations are scaled through this. */
void leds_set_brightness(uint8_t cap);

/* -------- radio-driven inputs (safe no-op defaults, call from anywhere) -------- */

/* Drives LED_MODE_RSSI. dbm is expected in roughly -90 (weak) .. -30
 * (strong); values outside that range are clamped internally. Defaults
 * to the weak end so an un-driven ring never looks falsely "hot". */
void leds_set_rssi(int8_t dbm);

/* Drives LED_MODE_CHANNEL. ch is a WiFi channel 1..14; out-of-range
 * values are clamped internally. Defaults to channel 1. */
void leds_set_channel(uint8_t ch);

/* Drives LED_MODE_TRAFFIC and seeds LED_EVENT_HIT's flash origin. Call
 * once per received frame (promiscuous RX, etc.) -- cheap (a handful of
 * byte stores), fine to call often from a WiFi/BLE task. */
void leds_packet_hit(void);

#else /* ---- non-T-Embed: inert inline no-ops ---- */

/*
 * The Cardputer has no LED ring. Rather than force every shared feature to
 * wrap its ring calls in #if POSEIDON_BOARD_TEMBED, provide no-op inlines and
 * matching enums so feature code can call leds_set_mode()/leds_event() freely
 * and compile to nothing on this board. Keeps the ~90 feature files free of
 * board conditionals.
 */
typedef enum {
    LED_MODE_IDLE = 0, LED_MODE_ACTIVE, LED_MODE_SCAN, LED_MODE_ALERT,
    LED_MODE_RSSI, LED_MODE_CHANNEL, LED_MODE_TRAFFIC, LED_MODE_ATTACK,
} led_mode_t;

typedef enum {
    LED_EVENT_NAV_CW = 0, LED_EVENT_NAV_CCW, LED_EVENT_SELECT, LED_EVENT_BACK,
    LED_EVENT_BOOT, LED_EVENT_HIT, LED_EVENT_ALERT_SPIN,
} led_event_t;

static inline void leds_begin(void) {}
static inline void leds_tick(void) {}
static inline void leds_set_mode(led_mode_t) {}
static inline void leds_event(led_event_t) {}
static inline void leds_set_brightness(uint8_t) {}
static inline void leds_set_rssi(int8_t) {}
static inline void leds_set_channel(uint8_t) {}
static inline void leds_packet_hit(void) {}

#endif /* POSEIDON_BOARD_TEMBED */
