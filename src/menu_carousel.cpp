/*
 * menu_carousel.cpp — see menu_carousel.h.
 *
 * Card layout, all coordinates derived from SCR_W / SCR_H / BODY_Y /
 * BODY_H (never hardcoded), so the same drawing code adapts to both the
 * 320x170 T-Embed and the 240x135 Cardputer:
 *
 *  [STATUS BAR ... 12 px ........... magenta divider ........]
 *  parent-name                                     N / TOTAL
 *  ===================== magenta double rule ===================
 *  +--                                                       --+
 *  |                       (( icon, scale 3, glow ))            |
 *  |                            WiFi                            |
 *  |                       recon + attacks                      |
 *  <                                                             >
 *  |                                                             |
 *  +--                                                       --+
 *                          o o *o o o o
 *  [FOOTER ... ;/.=swipe  ENTER=open  letter=jump  `=back ...]
 *
 * Corner brackets in T_ACCENT2 frame the card (arms only — not a full
 * box). The icon is the glow renderer in ui/picons.*, falling back to
 * the big hotkey letter when a node has no bitmap (all submenu items).
 * A dot row along the card's bottom edge replaces the old "N / TOTAL"
 * text as the at-a-glance position readout; the title bar keeps the
 * text version too since it also names the parent domain.
 *
 * Slide animation: when ;/. flips siblings, the new card slides in
 * from the corresponding edge over 200 ms (linear ease-out) — only the
 * card content (brackets/icon/label/hint) slides, chevrons and the dot
 * row are chrome and stay put. Letter mnemonics jump instantly.
 */
#include "menu_carousel.h"
#include "menu_icons.h"
#include "ui/picons.h"
#include "ui.h"
#include "ui_ambient.h"
#include "input.h"
#include "radio.h"
#include "theme.h"
#include "app.h"
#include "screensaver.h"
#if defined(POSEIDON_BOARD_TEMBED)
#include "board/leds_tembed.h"
#endif
#include <M5Cardputer.h>
#include <Arduino.h>
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* g_current_feature_item is owned by menu.cpp — the carousel sets it
 * the same way the terminal renderer does so feature ?-help still
 * resolves to the right node when invoked from a card. */
extern const menu_node_t *g_current_feature_item;
extern void               ui_show_current_help(void);

/* Second copy of the footer hint strip (menu.cpp has its own). On an
 * encoder-driven board the Cardputer key names are noise. */
#if defined(POSEIDON_BOARD_TEMBED)
#define CAROUSEL_FOOTER "turn=browse   press=open   side=back"
#else
#define CAROUSEL_FOOTER ";/.=swipe  ENTER=open  =help  letter=jump  `=back"
#endif

/* Reserved band at the bottom of the body for the position-dot row,
 * below the card frame proper. Shared between layout and dot drawing
 * so the two never drift apart. */
static const int DOTS_AREA_H  = 14;
static const int MAX_DOTS     = 12;

static int count_children(const menu_node_t *parent)
{
    int n = 0;
    for (const menu_node_t *c = parent->children; c && c->hotkey; ++c) ++n;
    return n;
}

static int index_of(const menu_node_t *parent, char hotkey)
{
    int i = 0;
    char want = (char)tolower((int)hotkey);
    for (const menu_node_t *c = parent->children; c && c->hotkey; ++c, ++i) {
        if (c->hotkey == want) return i;
    }
    return -1;
}

/* Linear interpolation 8-bit channel blend used for the current-dot
 * heartbeat pulse. Returns RGB565 where `a` and `b` are blended by t
 * in [0..255]. */
static uint16_t blend565(uint16_t a, uint16_t b, uint8_t t)
{
    uint8_t ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    uint8_t br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
    uint8_t r  = (uint8_t)((ar * (255 - t) + br * t) / 255);
    uint8_t g  = (uint8_t)((ag * (255 - t) + bg * t) / 255);
    uint8_t bl = (uint8_t)((ab * (255 - t) + bb * t) / 255);
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

/* Pulse color for the current position dot at the current millis(). */
static uint16_t pulse_color_now(void)
{
    uint32_t now   = millis();
    float    phase = (float)(now % 1500) / 1500.0f;
    float    cosw  = (1.0f - cosf(phase * 2.0f * 3.14159f)) * 0.5f;
    uint8_t  blend = (uint8_t)(cosw * 255.0f);
    return blend565(T_ACCENT, T_ACCENT2, blend);
}

/* All card geometry in one place, derived from SCR_W/SCR_H/BODY_Y/
 * BODY_H, so draw_card_full() and draw_card_anim() can never disagree
 * about where anything is. Bigger icon/text on the 170-tall T-Embed;
 * a step down on the 135-tall Cardputer so label+hint still fit under
 * a scale-3 icon in the smaller body. */
struct carousel_layout_t {
    int card_x, card_y, card_w, card_h, card_bottom;
    int ck;                 /* corner bracket arm length */
    int icon_cx, icon_cy;
    int icon_scale, icon_half;   /* icon_half includes the glow margin */
    int fallback_text_size;
    int label_y, hint_y;
    int chevron_y, chevron_size;
    int dots_y;
};

static carousel_layout_t compute_layout(void)
{
    carousel_layout_t L{};
    bool big = (SCR_H >= 170);

    L.card_x      = 6;
    L.card_w      = SCR_W - 12;
    L.card_y      = BODY_Y + 16;                      /* below title + divider */
    L.card_bottom = BODY_Y + BODY_H - DOTS_AREA_H;     /* leave room for dots */
    L.card_h      = L.card_bottom - L.card_y;
    L.ck          = 12;

    L.icon_scale = big ? 3 : 2;
    L.fallback_text_size = big ? 4 : 3;
    int top_gap   = big ? 4 : 2;
    int label_gap = big ? 4 : 2;
    int hint_gap  = big ? 3 : 2;

    L.icon_half = (menu_icon_h() * L.icon_scale) / 2 + L.icon_scale;
    L.icon_cx   = L.card_x + L.card_w / 2;
    L.icon_cy   = L.card_y + top_gap + L.icon_half;

    L.label_y = L.icon_cy + L.icon_half + label_gap;   /* size-2 text baseline */
    L.hint_y  = L.label_y + 16 + hint_gap;              /* size-2 glyph = 16 px tall */

    L.chevron_y    = L.card_y + L.card_h / 2;
    L.chevron_size = big ? 2 : 1;

    L.dots_y = L.card_bottom + DOTS_AREA_H / 2;

    return L;
}

/* Windowed dot-row range: which sibling indices get a dot this frame,
 * and where the current one sits, shared by the full paint and the
 * idle pulse repaint so they never disagree. */
struct dot_layout_t {
    int start, end;      /* [start, end) sibling indices shown */
    int spacing;
    int x0;               /* x of the first shown dot */
    int cur_x;            /* x of the current-item dot */
};

static dot_layout_t compute_dots(int n, int cursor, const carousel_layout_t &L)
{
    dot_layout_t dl{};
    dl.start = 0;
    dl.end   = n;
    if (n > MAX_DOTS) {
        int half = MAX_DOTS / 2;
        dl.start = cursor - half;
        dl.end   = dl.start + MAX_DOTS;
        if (dl.start < 0)  { dl.start = 0; dl.end = MAX_DOTS; }
        if (dl.end > n)    { dl.end = n; dl.start = n - MAX_DOTS; }
    }
    int shown   = dl.end - dl.start;
    dl.spacing  = 8;
    int total_w = (shown > 0) ? (shown - 1) * dl.spacing : 0;
    dl.x0       = L.icon_cx - total_w / 2;
    dl.cur_x    = dl.x0 + (cursor - dl.start) * dl.spacing;
    return dl;
}

/* Centre a string at (cx, y) using the 6x8 glyph cell, truncating to
 * max_px if needed. Buffer is scratch owned by the caller. */
static void draw_centered_fit(int cx, int y, int textsize, uint16_t color,
                              const char *src, int max_px)
{
    auto &d = M5Cardputer.Display;
    char buf[48];
    int  max_chars = max_px / (6 * textsize);
    if (max_chars < 1)  max_chars = 1;
    if (max_chars > (int)sizeof(buf) - 1) max_chars = sizeof(buf) - 1;
    int n = (int)strlen(src);
    if (n > max_chars) n = max_chars;
    memcpy(buf, src, n);
    buf[n] = 0;

    int w = n * 6 * textsize;
    d.setTextSize(textsize);
    d.setTextColor(color, T_BG);
    d.setCursor(cx - w / 2, y);
    d.print(buf);
    d.setTextSize(1);
}

/* Full card paint — every layer, every glyph, every bracket. Called on
 * cursor change, on slide animation frames, and on initial entry. */
static void draw_card_full(const menu_node_t *parent, int cursor, int slide_x)
{
    auto &d = M5Cardputer.Display;
    int n = count_children(parent);
    if (n <= 0 || cursor < 0 || cursor >= n) return;
    const menu_node_t *item = &parent->children[cursor];

    /* Body background + ambient layer underneath. The carousel doesn't
     * piggyback on draw_menu's hook, so we wire ambient ourselves. */
    d.fillRect(0, BODY_Y, SCR_W, BODY_H, T_BG);
    ui_ambient_tick(0, BODY_Y, SCR_W, BODY_H);

    /* Title bar: parent name on the left, "N / TOTAL" position on the
     * right in magenta. Matches terminal-mode aesthetics; the dot row
     * below gives the at-a-glance version. */
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2);
    d.print(parent->label);
    char pos[16];
    snprintf(pos, sizeof(pos), "%d / %d", cursor + 1, n);
    int pw = d.textWidth(pos);
    d.setTextColor(T_ACCENT2, T_BG);
    d.setCursor(SCR_W - pw - 4, BODY_Y + 2);
    d.print(pos);

    /* Magenta double-divider directly under the title. */
    d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT2);
    d.drawFastHLine(4, BODY_Y + 13, SCR_W - 8, T_ACCENT2);

    carousel_layout_t L = compute_layout();
    int cx = L.card_x + slide_x;   /* card content slides; chrome doesn't */

    /* 4 corner brackets, arms only — cyberpunk framing, not a box. */
    d.drawFastHLine(cx,                 L.card_y,                    L.ck, T_ACCENT2);
    d.drawFastVLine(cx,                 L.card_y,                    L.ck, T_ACCENT2);
    d.drawFastHLine(cx + L.card_w - L.ck, L.card_y,                  L.ck, T_ACCENT2);
    d.drawFastVLine(cx + L.card_w - 1,  L.card_y,                    L.ck, T_ACCENT2);
    d.drawFastHLine(cx,                 L.card_y + L.card_h - 1,     L.ck, T_ACCENT2);
    d.drawFastVLine(cx,                 L.card_y + L.card_h - L.ck,  L.ck, T_ACCENT2);
    d.drawFastHLine(cx + L.card_w - L.ck, L.card_y + L.card_h - 1,   L.ck, T_ACCENT2);
    d.drawFastVLine(cx + L.card_w - 1,  L.card_y + L.card_h - L.ck,  L.ck, T_ACCENT2);

    /* Icon, glowing, centred in the upper portion of the card. Falls
     * back to the big hotkey letter for nodes with no bitmap (every
     * submenu item — only MENU_ROOT entries have icons). */
    int icon_cx = L.icon_cx + slide_x;
    if (!picon_draw_hotkey(item->hotkey, icon_cx, L.icon_cy, L.icon_scale, T_ACCENT, 2)) {
        d.setTextSize(L.fallback_text_size);
        d.setTextColor(T_ACCENT, T_BG);
        int cw_px = 6 * L.fallback_text_size;
        int ch_px = 8 * L.fallback_text_size;
        d.setCursor(icon_cx - cw_px / 2, L.icon_cy - ch_px / 2);
        d.printf("%c", toupper(item->hotkey));
        d.setTextSize(1);
    }

    /* Label + hint, centred under the icon, truncated to fit. */
    int text_max_px = SCR_W - 32;
    draw_centered_fit(icon_cx, L.label_y, 2, T_FG, item->label, text_max_px);
    if (item->hint) {
        draw_centered_fit(icon_cx, L.hint_y, 1, T_DIM, item->hint, text_max_px);
    }

    /* Chevrons at the card's vertical centre, near the screen edges.
     * Navigation always wraps, so there's no real boundary to dim for
     * — except the degenerate n==1 case, where neither direction goes
     * anywhere. */
    uint16_t chev_color = (n > 1) ? T_ACCENT2 : T_DIM;
    int chev_h = 8 * L.chevron_size;
    d.setTextSize(L.chevron_size);
    d.setTextColor(chev_color, T_BG);
    d.setCursor(2, L.chevron_y - chev_h / 2);
    d.print("<");
    int chev_w = 6 * L.chevron_size;
    d.setCursor(SCR_W - 4 - chev_w, L.chevron_y - chev_h / 2);
    d.print(">");
    d.setTextSize(1);

    /* Position dots along the bottom of the body — windowed if there
     * are more siblings than MAX_DOTS. Current item is bigger and in
     * T_ACCENT; the rest are small and T_DIM. */
    dot_layout_t dl = compute_dots(n, cursor, L);
    for (int i = dl.start; i < dl.end; ++i) {
        int dx = dl.x0 + (i - dl.start) * dl.spacing;
        if (i == cursor) {
            d.fillCircle(dx, L.dots_y, 2, T_ACCENT);
        } else {
            d.fillCircle(dx, L.dots_y, 1, T_DIM);
        }
    }
}

/* Lightweight idle paint — only touches the parts of the card that
 * actually change frame-to-frame: the ambient strip above the icon,
 * the ambient strip below the hint text, and the pulsing current-item
 * dot. Brackets, icon, label, hint, chevrons, and the rest of the dot
 * row are NOT touched, so they don't strobe. */
static void draw_card_anim(const menu_node_t *parent, int cursor)
{
    int n = count_children(parent);
    if (n <= 0 || cursor < 0 || cursor >= n) return;

    auto &d = M5Cardputer.Display;
    carousel_layout_t L = compute_layout();

    /* Ambient strips — inset 8 px from card edges so the corner
     * brackets are never erased. Clipped per strip so ambient motes
     * still compute against the full card bounds. */
    int amb_x = L.card_x + 8;
    int amb_w = L.card_w - 16;

    int top_y = L.card_y + L.ck;
    int top_h = (L.icon_cy - L.icon_half) - top_y;
    if (top_h > 0) {
        d.fillRect(amb_x, top_y, amb_w, top_h, T_BG);
        d.setClipRect(amb_x, top_y, amb_w, top_h);
        ui_ambient_tick(0, BODY_Y, SCR_W, BODY_H);
        d.clearClipRect();
    }

    int bot_y = L.hint_y + 8;
    int bot_h = (L.card_y + L.card_h - L.ck) - bot_y;
    if (bot_h > 0) {
        d.fillRect(amb_x, bot_y, amb_w, bot_h, T_BG);
        d.setClipRect(amb_x, bot_y, amb_w, bot_h);
        ui_ambient_tick(0, BODY_Y, SCR_W, BODY_H);
        d.clearClipRect();
    }

    /* Repaint just the current-item dot with its heartbeat pulse. */
    if (n > 0) {
        dot_layout_t dl = compute_dots(n, cursor, L);
        d.fillCircle(dl.cur_x, L.dots_y, 2, pulse_color_now());
    }
}

void carousel_run_submenu(const menu_node_t *parent)
{
    int      cursor       = 0;
    int      n            = count_children(parent);
    if (n <= 0) return;

    int      slide_dir    = 0;       /* -1 / 0 / +1 */
    uint32_t slide_start  = 0;
    bool     animating    = false;

    ui_status_invalidate();
    ui_screen_enter();
    ui_draw_status(radio_name(), "");
    ui_draw_footer(CAROUSEL_FOOTER);

    /* Initial paint. */
    draw_card_full(parent, cursor, 0);

    while (true) {
        uint32_t now = millis();

#if defined(POSEIDON_BOARD_TEMBED)
        /* Drive the encoder-ring animation. Self-rate-limits internally,
         * so calling it every pass is cheap. Without this the ring sits
         * at whatever leds_begin() left it at. */
        leds_tick();
#endif

        /* Animation tick — re-paint the card with a decaying x-offset
         * each frame until the 200 ms window expires. */
        if (animating) {
            uint32_t elapsed = now - slide_start;
            if (elapsed >= 200) {
                animating = false;
                draw_card_full(parent, cursor, 0);
            } else {
                int slide_x = (int)((int64_t)slide_dir * (200 - (int)elapsed) * SCR_W / 200);
                draw_card_full(parent, cursor, slide_x);
            }
        } else {
            /* Idle: only repaint the parts that actually animate — the
             * two ambient strips and the pulsing current-item dot. */
            static uint32_t last_idle_paint = 0;
            if (now - last_idle_paint > 33) {
                last_idle_paint = now;
                draw_card_anim(parent, cursor);
            }
        }

        uint16_t k = input_poll();
        if (k == PK_NONE) {
            /* Screensaver takeover on idle. After it returns, force a
             * full card repaint (menu state was clobbered). */
            if (screensaver_check_idle()) {
                ui_status_invalidate();
                ui_screen_enter();
                ui_draw_status(radio_name(), "");
                ui_draw_footer(CAROUSEL_FOOTER);
                draw_card_full(parent, cursor, 0);
            }
            delay(8);
            continue;
        }

        if (k == PK_ESC || k == '`') {
#if defined(POSEIDON_BOARD_TEMBED)
            leds_event(LED_EVENT_BACK);
#endif
            return;
        }

#if defined(POSEIDON_BOARD_TEMBED)
        /* Ring reacts to navigation: chase in the direction of travel,
         * flash on open. */
        if      (k == PK_UP   || k == ';') { leds_event(LED_EVENT_NAV_CCW); }
        else if (k == PK_DOWN || k == '.') { leds_event(LED_EVENT_NAV_CW); }
        else if (k == PK_ENTER)            { leds_event(LED_EVENT_SELECT); }
#endif

        /* Help — same key as terminal mode. Delegates to ui_show_current_help. */
        if (k == '=' || k == '?') {
            const menu_node_t *sel = &parent->children[cursor];
            g_current_feature_item = sel;
            ui_show_current_help();
            g_current_feature_item = nullptr;
            ui_screen_enter();
            ui_draw_status(radio_name(), "");
            ui_draw_footer(CAROUSEL_FOOTER);
            draw_card_full(parent, cursor, 0);
            continue;
        }

        /* Accept UP/DOWN as well as LEFT/RIGHT: a rotary encoder emits
         * UP/DOWN, and on this carousel both axes mean prev/next item.
         * Without UP/DOWN here the knob does nothing in the carousel while
         * working fine in list screens, which is exactly what happened. */
        if (k == ';' || k == PK_LEFT || k == PK_UP) {
            cursor = (cursor - 1 + n) % n;
            slide_dir   = -1;
            slide_start = now;
            animating   = true;
            continue;
        }
        if (k == '.' || k == PK_RIGHT || k == PK_DOWN) {
            cursor = (cursor + 1) % n;
            slide_dir   = +1;
            slide_start = now;
            animating   = true;
            continue;
        }

        if (k == PK_ENTER) {
            const menu_node_t *sel = &parent->children[cursor];
            if (sel->action) {
                Serial.printf("[FEAT_ENTER] %s\n", sel->label);
                g_current_feature_item = sel;
                sel->action();
                g_current_feature_item = nullptr;
                /* Defensive IR park — see menu.cpp comment. */
                pinMode(44, OUTPUT); digitalWrite(44, HIGH);
                Serial.printf("[FEAT_EXIT] %s\n", sel->label);
                ui_screen_enter();
                ui_draw_status(radio_name(), "");
                ui_draw_footer(CAROUSEL_FOOTER);
                draw_card_full(parent, cursor, 0);
            } else if (sel->children) {
                carousel_run_submenu(sel);
                ui_screen_enter();
                ui_draw_status(radio_name(), "");
                ui_draw_footer(CAROUSEL_FOOTER);
                draw_card_full(parent, cursor, 0);
            }
            continue;
        }

        /* Letter mnemonic — jump straight to that card and execute. No
         * slide animation here; jumps should feel instant. */
        if (k >= 0x20 && k < 0x7F) {
            int i = index_of(parent, (char)k);
            if (i >= 0) {
                cursor = i;
                draw_card_full(parent, cursor, 0);
                const menu_node_t *sel = &parent->children[cursor];
                if (sel->action) {
                    Serial.printf("[FEAT_ENTER] %s\n", sel->label);
                    g_current_feature_item = sel;
                    sel->action();
                    g_current_feature_item = nullptr;
                    /* Defensive IR park — same as regular path. */
                    pinMode(44, OUTPUT); digitalWrite(44, HIGH);
                    Serial.printf("[FEAT_EXIT] %s\n", sel->label);
                    ui_screen_enter();
                    ui_draw_status(radio_name(), "");
                    ui_draw_footer(CAROUSEL_FOOTER);
                    draw_card_full(parent, cursor, 0);
                } else if (sel->children) {
                    carousel_run_submenu(sel);
                    ui_screen_enter();
                    ui_draw_status(radio_name(), "");
                    ui_draw_footer(CAROUSEL_FOOTER);
                    draw_card_full(parent, cursor, 0);
                }
            }
        }
    }
}
