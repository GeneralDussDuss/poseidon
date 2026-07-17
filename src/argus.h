/*
 * argus — animated mood-portrait renderer for the Triton gotchi.
 *
 * 48x48 RGB565 sprite per mood (10 moods, ~45 KB in flash) +
 * lightweight procedural overlays. Always push on every draw call —
 * caller's ui_clear_body() wipes the region, so cache + skip would
 * leave a black gap and look like flicker. pushImage of 4.6 KB at
 * the redraw cadence is light enough to not fragment WiFi DMA heap.
 */
#pragma once

#include <Arduino.h>
#include "argus_data.h"

void argus_draw(argus_mood_t mood, int x, int y);
void argus_flash(argus_mood_t mood, uint32_t ms);

/* Reset the internal draw cache so the NEXT argus_draw re-pushes the
 * sprite unconditionally. Call this right after a body clear (feature
 * entry, view switch) — otherwise the cache sees an unchanged mood/x/y
 * and skips the push, leaving a black gap where the face should be. */
void argus_invalidate(void);
