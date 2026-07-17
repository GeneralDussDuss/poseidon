/*
 * wdr_mood — pure mood-selection logic for the Argus wardrive view.
 *
 * No Arduino / esp headers so it compiles and unit-tests on the host.
 * Reads a snapshot of wardrive state and returns which Argus mood to
 * draw. All thresholds live here; wifi_wardrive.cpp only fills the ctx.
 */
#pragma once
#include <stdint.h>
#include "../argus_data.h"   /* argus_mood_t (pure enum + sprite data) */

struct wdr_mood_ctx {
    bool     gps_valid;        /* current fix valid */
    bool     gps_ever_locked;  /* a fix has existed this run */
    float    gps_speed_kts;    /* current ground speed */
    uint32_t now_ms;           /* millis() snapshot */
    uint32_t entry_ms;         /* millis() at feature start */
    uint32_t last_new_ms;      /* millis() of most recent new AP (0 if none yet) */
    int      new_in_5s;        /* new APs added in the last ~5 s */
    int      ap_count;         /* distinct APs so far */
    int      ap_cap;           /* WARDRIVE_MAX_APS */
};

/* True when growing the new-AP count from prev to cur crosses a
 * multiple of step (step > 0). */
static inline bool wdr_milestone_crossed(int prev, int cur, int step) {
    return step > 0 && (cur / step) > (prev / step);
}

static inline argus_mood_t wdr_pick_mood(const wdr_mood_ctx& c) {
    if (c.ap_count >= c.ap_cap) return ARGUS_STERN;

    if (!c.gps_valid) {
        if (!c.gps_ever_locked && (c.now_ms - c.entry_ms) > 20000)
            return ARGUS_REFLECTIVE;
        return ARGUS_CURIOUS;
    }

    /* Locked: hunt personas. idle = time since the last new AP (or since
     * entry if none found yet). */
    uint32_t idle = (c.last_new_ms == 0) ? (c.now_ms - c.entry_ms)
                                         : (c.now_ms - c.last_new_ms);
    if (c.new_in_5s >= 3) return ARGUS_CALCULATING;
    if (idle < 8000)      return ARGUS_INTERESTED;
    if (idle < 60000)     return (c.gps_speed_kts > 1.0f) ? ARGUS_WATCHING
                                                          : ARGUS_RESIGNED;
    if (idle < 180000)    return ARGUS_RESIGNED;
    return ARGUS_SLEEPING;
}
