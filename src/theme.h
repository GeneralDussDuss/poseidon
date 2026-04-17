/*
 * theme.h — swappable color palettes + UI chrome config.
 *
 * Every COL_* reference in features should use theme_xxx() instead
 * of hardcoded values. Theme persists via Preferences.
 */
#pragma once

#include <Arduino.h>

struct poseidon_theme_t {
    const char *name;
    uint16_t bg;          /* main background */
    uint16_t fg;          /* primary text */
    uint16_t accent;      /* titles, highlights */
    uint16_t accent2;     /* secondary accent (borders, rules) */
    uint16_t warn;        /* warnings */
    uint16_t bad;         /* errors, attacks */
    uint16_t good;        /* success, captures */
    uint16_t dim;         /* muted text, hints */
    uint16_t sel_bg;      /* selected row background */
    uint16_t sel_border;  /* selected row border */
    uint16_t status_bg;   /* status bar gradient top */
    uint16_t status_bg2;  /* status bar gradient bottom */
    uint16_t footer_bg;   /* footer gradient */
    uint16_t rule;        /* divider lines */
};

enum theme_id_t {
    THEME_POSEIDON = 0,   /* cyan/magenta on black — the OG */
    THEME_PHANTOM,        /* deep purple/violet */
    THEME_MATRIX,         /* green on black */
    THEME_AMBER,          /* amber/orange on black (retro) */
    THEME_EINK,           /* black on white (paper) */
    THEME_TRON,           /* neon blue + cyan glow, circuit aesthetic */
    THEME__COUNT
};

void theme_set(theme_id_t id);
theme_id_t theme_current_id(void);
const poseidon_theme_t &theme(void);

/* Convenience — replaces COL_* macros. */
#define T_BG       (theme().bg)
#define T_FG       (theme().fg)
#define T_ACCENT   (theme().accent)
#define T_ACCENT2  (theme().accent2)
#define T_WARN     (theme().warn)
#define T_BAD      (theme().bad)
#define T_GOOD     (theme().good)
#define T_DIM      (theme().dim)
#define T_SEL_BG   (theme().sel_bg)
#define T_SEL_BD   (theme().sel_border)
