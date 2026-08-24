/*
 * picons.cpp - anti-aliased menu icon renderer. See picons.h.
 *
 * WHY THE LOOKUP IS MENU-AWARE:
 *   Hotkeys are only unique WITHIN a menu. 's' is Scan in WiFi, Sniffer in
 *   nRF24, Settings in System, Scan/Copy in Sub-GHz. A single global
 *   hotkey-to-icon table therefore cannot serve submenus; it can only ever be
 *   correct for one of them. The icon is resolved from (menu, hotkey), with the
 *   menu identified by its parent node's label.
 *
 * WHY IT LOOKS CLEAN:
 *   Icons are 96x96 EIGHT-BIT COVERAGE maps (picons_data.h) drawn 1:1 and
 *   alpha-blended over the background. The original renderer drew one filled
 *   scale-by-scale rectangle per set bit of a 1-bit 24x24 bitmap, which is what
 *   made the menu look like stair-stepped blocks. Nothing is upscaled here.
 */
#include "picons.h"
#include "picons_data.h"
#include "../theme.h"
#include <M5Cardputer.h>
#include <string.h>

/* Composite scratch, sized for one icon at native resolution. Static rather
 * than stack: 96*96*2 = 18 KB would obliterate a task stack. */
static uint16_t s_buf[PICON_W * PICON_H];

/* ---- (menu, hotkey) -> sheet cell ----------------------------------------
 * Each table maps one menu's hotkeys onto its sheet, indexed row-major 0..15.
 * A hotkey absent from the table draws the letter instead, which reads fine now
 * that the fallback uses a real font. */
/* `sheet` is normally nullptr, meaning "use this menu's default sheet". A few
 * entries borrow a better-fitting icon from another sheet (the root NFC and EMV
 * entries want the literal contactless-card art from the TOOL sheet). */
struct icon_map_t { char key; int8_t cell; const uint8_t *const *sheet; };

/* ROOT sheet: wifi, bluetooth, tower, screen, remote, broadcast, waveform, pin,
 * folder, code, gear, clock, chain, sliders, rj45, radio tower. */
static const icon_map_t MAP_ROOT[] = {
    {'w',0,nullptr},{'b',1,nullptr},{'r',15,nullptr},{'i',4,nullptr},{'n',14,nullptr},{'m',12,nullptr},{'o',13,nullptr},{'s',10,nullptr},
    {'d',8,nullptr},{'u',9,nullptr},{'t',6,nullptr},{'p',3,nullptr},{'j',5,nullptr},{'5',7,nullptr},
    /* Borrowed from the TOOL sheet: a literal contactless card and a card with
     * a binary dump read far better here than the generic root glyphs. */
    {'k',3,PICON_SHEET_TOOL},{'c',0,PICON_SHEET_TOOL},{'e',1,PICON_SHEET_TOOL},{0,0,nullptr}
};

/* WIFI sheet: radar, network map, broken link, tower burst, shield+keyhole,
 * twin APs, spiderweb, magnifier+trace, open padlock, geo pin, ghost, bars,
 * lighthouse, eye, shield+check, usb cable warning. */
static const icon_map_t MAP_WIFI[] = {
    {'s',0,nullptr},{'l',1,nullptr},{'o',1,nullptr},{'d',2,nullptr},{'x',2,nullptr},{'e',13,nullptr},{'c',5,nullptr},{'p',6,nullptr},
    {'i',10,nullptr},{'t',3,nullptr},{'k',6,nullptr},{'b',3,nullptr},{'r',7,nullptr},{'m',4,nullptr},{'g',11,nullptr},{'w',9,nullptr},
    {'n',12,nullptr},{'y',14,nullptr},{'u',15,nullptr},{'v',13,nullptr},{'z',8,nullptr},{0,0,nullptr}
};

/* BLE sheet: bt+waves, chat bubbles, keyboard, airtag, gauge, drone, doc list,
 * beacon tower, two phones, node tree, inbound arrows, mask, apple+bolt,
 * earbuds, key+devices, skull. */
static const icon_map_t MAP_BLE[] = {
    {'s',0,nullptr},{'p',1,nullptr},{'h',2,nullptr},{'t',3,nullptr},{'f',4,nullptr},{'d',5,nullptr},{'n',6,nullptr},{'b',7,nullptr},
    {'c',8,nullptr},{'g',9,nullptr},{'x',10,nullptr},{'k',11,nullptr},{'a',12,nullptr},{'y',3,nullptr},{'w',13,nullptr},{'q',14,nullptr},
    {'l',2,nullptr},{0,0,nullptr}
};

/* RADIO sheet: fob, garage, waterfall, record, play, freq digits, lock+keys,
 * jammer, finder, tower, dial, device-to-device, warning, sub library, scope,
 * dish. Serves both Sub-GHz and nRF24. */
static const icon_map_t MAP_SUBGHZ[] = {
    {'b',13,nullptr},{'s',5,nullptr},{'r',3,nullptr},{'p',4,nullptr},{'a',2,nullptr},{'f',6,nullptr},{'j',7,nullptr},{'h',8,nullptr},{'d',12,nullptr},{0,0,nullptr}
};
static const icon_map_t MAP_NRF24[] = {
    {'s',2,nullptr},{'m',11,nullptr},{'b',9,nullptr},{'a',14,nullptr},{'j',7,nullptr},{'h',8,nullptr},{0,0,nullptr}
};

/* TOOL sheet: nfc card, card+binary, usb stick, key+fingerprint, torch,
 * stopwatch, dice, calculator, morse, microSD, folder tree, gear+wrench,
 * battery, palette, speaker, trident. */
static const icon_map_t MAP_TOOLS[] = {
    {'l',4,nullptr},{'s',5,nullptr},{'d',6,nullptr},{'c',7,nullptr},{'m',8,nullptr},{'f',9,nullptr},{0,0,nullptr}
};
static const icon_map_t MAP_SYS[] = {
    {'f',10,nullptr},{'s',11,nullptr},{'h',12,nullptr},{'t',13,nullptr},{'n',14,nullptr},{'a',15,nullptr},{'c',5,nullptr},{0,0,nullptr}
};
static const icon_map_t MAP_IR[] = {
    {'r',4,nullptr},{'c',11,nullptr},{'l',3,nullptr},{'t',6,nullptr},{0,0,nullptr}
};

/* Menu label -> (sheet, map). Matching on the parent label keeps this file free
 * of menu.cpp's static arrays. */
struct menu_icons_t {
    const char           *label;   /* nullptr = root menu */
    const uint8_t *const *sheet;
    const icon_map_t     *map;
};

static const menu_icons_t MENU_ICONS[] = {
    /* The root node is MENU_ROOT in menu.cpp, whose label is "POSEIDON" -- NOT
     * null. Matching only on nullptr meant the entire top level menu resolved to
     * no icon and drew hotkey letters instead. Accept both. */
    { nullptr,     PICON_SHEET_ROOT,  MAP_ROOT   },
    { "POSEIDON",  PICON_SHEET_ROOT,  MAP_ROOT   },
    { "WiFi",      PICON_SHEET_WIFI,  MAP_WIFI   },
    { "Bluetooth", PICON_SHEET_BLE,   MAP_BLE    },
    { "Sub-GHz",   PICON_SHEET_RADIO, MAP_SUBGHZ },
    { "nRF24",     PICON_SHEET_RADIO, MAP_NRF24  },
    { "IR",        PICON_SHEET_TOOL,  MAP_IR     },
    { "Tools",     PICON_SHEET_TOOL,  MAP_TOOLS  },
    { "System",    PICON_SHEET_TOOL,  MAP_SYS    },
};

/* Resolve the coverage map for one menu entry, or nullptr for "no icon". */
static const uint8_t *icon_for(const char *menu_label, char hotkey)
{
    for (unsigned m = 0; m < sizeof(MENU_ICONS) / sizeof(MENU_ICONS[0]); ++m) {
        const menu_icons_t &mi = MENU_ICONS[m];
        const bool hit = (mi.label == nullptr)
                       ? (menu_label == nullptr)
                       : (menu_label && strcmp(menu_label, mi.label) == 0);
        if (!hit) continue;
        for (const icon_map_t *e = mi.map; e->key; ++e)
            if (e->key == hotkey && e->cell >= 0 && e->cell < 16)
                return (e->sheet ? e->sheet : mi.sheet)[e->cell];
        return nullptr;      /* correct menu, but no icon for this entry */
    }
    return nullptr;
}

/* Blend fg over bg by 8-bit coverage, in RGB565. */
static inline uint16_t blend(uint16_t bg, uint16_t fg, uint8_t a)
{
    if (a == 0)   return bg;
    if (a == 255) return fg;
    const uint32_t ia = 255u - a;
    const uint32_t r = (((fg >> 11) & 0x1F) * a + ((bg >> 11) & 0x1F) * ia) / 255;
    const uint32_t g = (((fg >>  5) & 0x3F) * a + ((bg >>  5) & 0x3F) * ia) / 255;
    const uint32_t b = (( fg        & 0x1F) * a + ( bg        & 0x1F) * ia) / 255;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static inline uint8_t sample(const uint8_t *ic, int sx, int sy)
{
    if (sx < 0) sx = 0;
    if (sx >= PICON_W) sx = PICON_W - 1;
    if (sy < 0) sy = 0;
    if (sy >= PICON_H) sy = PICON_H - 1;
    return ic[sy * PICON_W + sx];
}

static bool draw_icon(const uint8_t *ic, int cx, int cy, int size,
                      uint16_t c1, uint16_t c2, uint16_t bg)
{
    if (!ic || size < 4) return false;
    if (size > PICON_W) size = PICON_W;          /* never upscale past native */
    auto &d = M5Cardputer.Display;

    for (int y = 0; y < size; ++y) {
        const int32_t fy = ((int32_t)y * 2 + 1) * PICON_H * 128 / (size * 2) - 128;
        const int sy = (int)(fy >> 7);
        const uint8_t wy = (uint8_t)((fy & 0x7F) << 1);

        for (int x = 0; x < size; ++x) {
            const int32_t fx = ((int32_t)x * 2 + 1) * PICON_W * 128 / (size * 2) - 128;
            const int sx = (int)(fx >> 7);
            const uint8_t wx = (uint8_t)((fx & 0x7F) << 1);

            const uint16_t a00 = sample(ic, sx,     sy);
            const uint16_t a10 = sample(ic, sx + 1, sy);
            const uint16_t a01 = sample(ic, sx,     sy + 1);
            const uint16_t a11 = sample(ic, sx + 1, sy + 1);
            const uint16_t top = (uint16_t)((a00 * (255 - wx) + a10 * wx) / 255);
            const uint16_t bot = (uint16_t)((a01 * (255 - wx) + a11 * wx) / 255);
            const uint8_t  a   = (uint8_t) ((top * (255 - wy) + bot * wy) / 255);

            const uint8_t t = (uint8_t)((x * 255) / (size - 1));
            s_buf[y * size + x] = blend(bg, blend(c1, c2, t), a);
        }
    }

    /* rgb565_t cast: the bare uint16_t* overload is the TFT_eSPI-compatible one
     * and reads the buffer as BYTE-SWAPPED 565, which turns every anti-aliased
     * ramp into chaotic hue jumps. Colour-key on bg so the empty ~80% of the
     * tile stays transparent and the animated background shows through rather
     * than being stamped over with a box. */
    d.pushImage(cx - size / 2, cy - size / 2, size, size,
                reinterpret_cast<const lgfx::rgb565_t *>(s_buf),
                lgfx::rgb565_t(bg));
    return true;
}

bool picon_draw_menu(const char *menu_label, char hotkey, int cx, int cy,
                     int size, uint16_t c1, uint16_t c2, uint16_t bg)
{
    return draw_icon(icon_for(menu_label, hotkey), cx, cy, size, c1, c2, bg);
}

bool picon_draw_tinted(char hotkey, int cx, int cy, int size,
                       uint16_t c1, uint16_t c2, uint16_t bg)
{
    return picon_draw_menu(nullptr, hotkey, cx, cy, size, c1, c2, bg);
}

bool picon_draw_hotkey(char hotkey, int cx, int cy, int scale,
                       uint16_t colour, int glow)
{
    (void)glow;   /* the halo is baked into the artwork coverage now */
    return picon_draw_tinted(hotkey, cx, cy, 24 * (scale < 1 ? 1 : scale),
                             colour, colour, T_BG);
}
