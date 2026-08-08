/*
 * sfx — sound effect library + volume/mute settings.
 *
 * Tone-based (cheap, no samples on SD, fits the hacky aesthetic). All SFX
 * are short, non-blocking where possible, and respect the global mute /
 * volume settings persisted in NVS.
 *
 * Use sfx_click() as the subtle "clack" on keypresses — global hook lives
 * in input.cpp. Higher-level feature-specific SFX are called from the
 * feature code at the right moment (handshake captured, deauth burst,
 * etc.)
 */
#pragma once

#include <Arduino.h>
#include <stdint.h>

/* Load persisted volume + mute from NVS, init the speaker. Safe to call
 * multiple times. */
void sfx_init(void);

/* Volume 0..10. sfx_set_volume(0) is equivalent to mute. Persists to NVS. */
void sfx_set_volume(uint8_t vol);
uint8_t sfx_get_volume(void);

/* Global mute toggle. Persisted to NVS. When muted, all sfx_* calls are
 * no-ops. Volume is preserved separately so unmute restores prior level. */
void sfx_set_mute(bool on);
bool sfx_is_muted(void);

/* UI / navigation cues — very short, low pitch, unobtrusive */
void sfx_click(void);    /* any key press — global hook */

/* Encoder detent. Pitch tracks position in the list, so scrolling reads as
 * a melodic run rather than the same blip repeated — the R2D2 trick. `up`
 * bends the chirp upward, down bends it down. Non-blocking: enqueued to the
 * sfx player task like every other cue. */
void sfx_nav(bool up, uint8_t index, uint8_t total);
void sfx_select(void);   /* ENTER on a menu item */
void sfx_back(void);     /* ESC / back out */
void sfx_error(void);    /* bad input, action failed */
void sfx_toast(void);    /* info pop-up */

/* Attack + capture cues — distinctive, short */
void sfx_scan_start(void);
void sfx_scan_hit(void);      /* new network / signal found */
void sfx_deauth_burst(void);  /* firing frames */
void sfx_capture(void);       /* generic capture grabbed */
void sfx_hs_capture(void);    /* full 4-way handshake captured (longer fanfare) */
void sfx_pmkid_capture(void); /* PMKID grabbed (shorter double-beep) */
void sfx_cracked(void);       /* wifi pwned */

/* System cues */
void sfx_boot(void);      /* splash — distinctive POSEIDON boot jingle */
void sfx_alert(void);     /* PMF warning, oh-shit moment */
void sfx_glitch(void);    /* for anything cyberpunk / hack moment */
