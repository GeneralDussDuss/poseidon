/*
 * theme.cpp — built-in palettes.
 *
 * Active theme persists to NVS (namespace "pui", key "theme") so the
 * user's pick survives reboots. theme_init() loads it; theme_set()
 * writes on change. Keys kept ≤15 chars — Preferences NVS limit.
 */
#include "theme.h"
#include <M5Cardputer.h>
#include <Preferences.h>

static const poseidon_theme_t THEMES[] = {
    /* POSEIDON — cyan/magenta on black. The original. */
    {
        "POSEIDON",
        0x0000,             /* bg: pure black */
        0xFFFF,             /* fg: white */
        0x07FF,             /* accent: cyan */
        0xF81F,             /* accent2: magenta */
        0xFFE0,             /* warn: yellow */
        0xF800,             /* bad: red */
        0x07E0,             /* good: green */
        0x7BEF,             /* dim: gray */
        0x3007,             /* sel_bg: deep cyan-purple */
        0xF81F,             /* sel_border: magenta */
        0x20A5,             /* status_bg: teal */
        0x0841,             /* status_bg2: dark blue-green */
        0x1082,             /* footer_bg: deep slate */
        0x2124,             /* rule: muted cyan */
    },
    /* PHANTOM — deep purple/violet. */
    {
        "PHANTOM",
        0x0000,
        0xDEDB,             /* fg: soft white */
        0xC01F,             /* accent: bright violet */
        0x780F,             /* accent2: deep purple */
        0xFBE0,             /* warn: gold */
        0xF800,
        0x87F0,             /* good: mint */
        0x630C,             /* dim: purple-gray */
        0x2808,             /* sel_bg: dark purple */
        0xC01F,             /* sel_border: violet */
        0x3808,             /* status_bg: purple */
        0x1004,             /* status_bg2: near-black purple */
        0x1804,             /* footer_bg */
        0x4810,             /* rule: muted violet */
    },
    /* MATRIX — green phosphor on black. */
    {
        "MATRIX",
        0x0000,
        0x07E0,             /* fg: bright green */
        0x07E0,             /* accent: green */
        0x03E0,             /* accent2: darker green */
        0x07E0,             /* warn: green (mono) */
        0x04A0,             /* bad: dim green */
        0x07E0,             /* good: bright green */
        0x0320,             /* dim: dark green */
        0x0120,             /* sel_bg: very dark green */
        0x07E0,             /* sel_border: green */
        0x01A0,             /* status_bg: dark green */
        0x0060,             /* status_bg2: near-black green */
        0x00C0,             /* footer_bg */
        0x0260,             /* rule: muted green */
    },
    /* AMBER — warm retro terminal. */
    {
        "AMBER",
        0x0000,
        0xFCA0,             /* fg: amber */
        0xFCA0,             /* accent: amber */
        0xC460,             /* accent2: dark amber */
        0xFCA0,             /* warn: amber (mono) */
        0xA300,             /* bad: dark orange */
        0xFCA0,             /* good: amber */
        0x6180,             /* dim: dark amber */
        0x2080,             /* sel_bg: very dark amber */
        0xFCA0,             /* sel_border: amber */
        0x4100,             /* status_bg: brown-amber */
        0x1040,             /* status_bg2 */
        0x2080,             /* footer_bg */
        0x4100,             /* rule */
    },
    /* E-INK — paper white, minimal. */
    {
        "E-INK",
        0xFFFF,             /* bg: white */
        0x0000,             /* fg: black */
        0x0000,             /* accent: black */
        0x4208,             /* accent2: dark gray */
        0x4208,             /* warn: gray */
        0x0000,             /* bad: black */
        0x4208,             /* good: dark gray */
        0x8410,             /* dim: medium gray */
        0xC618,             /* sel_bg: light gray */
        0x0000,             /* sel_border: black */
        0xDEFB,             /* status_bg: off-white */
        0xC618,             /* status_bg2: light gray */
        0xDEFB,             /* footer_bg */
        0x8410,             /* rule: gray */
    },
    /* TRON — neon circuit glow. */
    {
        "TRON",
        0x0000,
        0xBFFF,             /* fg: ice blue */
        0x07FF,             /* accent: neon cyan */
        0x04DF,             /* accent2: electric blue */
        0xFFE0,             /* warn: yellow */
        0xF800,             /* bad: red */
        0x07FF,             /* good: cyan */
        0x2967,             /* dim: dark steel blue */
        0x0113,             /* sel_bg: deep dark blue */
        0x07FF,             /* sel_border: neon cyan */
        0x010B,             /* status_bg: deep navy */
        0x0005,             /* status_bg2: near-black blue */
        0x0109,             /* footer_bg: dark blue */
        0x04DF,             /* rule: electric blue */
    },
};

static theme_id_t s_current = THEME_POSEIDON;
static bool       s_inited  = false;

void theme_init(void)
{
    if (s_inited) return;
    s_inited = true;
    Preferences p;
    if (p.begin("pui", true)) {   /* read-only */
        uint8_t v = p.getUChar("theme", (uint8_t)THEME_POSEIDON);
        p.end();
        if (v >= THEME__COUNT) v = THEME_POSEIDON;
        s_current = (theme_id_t)v;
    }
}

void theme_set(theme_id_t id)
{
    if (id >= THEME__COUNT) id = THEME_POSEIDON;
    if (id == s_current) return;
    s_current = id;
    Preferences p;
    if (p.begin("pui", false)) {  /* read-write */
        p.putUChar("theme", (uint8_t)id);
        p.end();
    }
}

theme_id_t theme_current_id(void) { return s_current; }
const poseidon_theme_t &theme(void) { return THEMES[s_current]; }
