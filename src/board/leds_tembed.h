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
    LED_MODE_IDLE = 0,   /* slow breathing in T_ACCENT, ~3s cycle */
    LED_MODE_ACTIVE,     /* comet: bright head + fading tail, ~2s/rev, T_ACCENT */
    LED_MODE_SCAN,        /* radar sweep: fast orbit + dim persistence trail, T_ACCENT2 */
    LED_MODE_ALERT,       /* strobe T_BAD / off @ ~4Hz */
} led_mode_t;

typedef enum {
    LED_EVENT_NAV_CW = 0,  /* brief chase pulse, clockwise travel direction */
    LED_EVENT_NAV_CCW,     /* brief chase pulse, counter-clockwise travel direction */
    LED_EVENT_SELECT,      /* all 8 flash T_GOOD, decay over ~300ms */
    LED_EVENT_BACK,        /* dim reverse sweep, T_DIM */
    LED_EVENT_BOOT,        /* rainbow spin-up, one full revolution, for splash */
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

#endif /* POSEIDON_BOARD_TEMBED */
