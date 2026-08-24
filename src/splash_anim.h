#pragma once

#include <stdint.h>

/*
 * splash_anim - animated POSEIDON boot splash (T-Embed only).
 *
 * Plays the type-motion wordmark reveal authored at 320x170. Frame data lives
 * in sprites/splash_anim.h (palette + RLE, ~555 KB flash); the player streams
 * it one scanline at a time so it costs ~640 bytes of RAM and no allocation.
 *
 * On non-T-Embed builds this header still compiles but the symbol is absent -
 * callers must gate on POSEIDON_BOARD_TEMBED (the frame data is sized for that
 * panel, so there is nothing sensible to draw on a 240x135 screen).
 *
 * Any keypress / encoder input aborts playback immediately.
 */

#if defined(POSEIDON_BOARD_TEMBED)

/* Play the full splash once. frame_ms is the target time per frame (the
 * source animation was authored at 100 ms). Returns as soon as the animation
 * finishes or the user presses anything. */
void splash_anim_play(uint32_t frame_ms = 100);

#endif
