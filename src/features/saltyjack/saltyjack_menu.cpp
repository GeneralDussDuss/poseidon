/*
 * SaltyJack — custom submenu renderer.
 *
 * RaspyJack-faithful UX:
 *   - Arrow-nav list with solid highlight rect behind cursor row
 *   - Short blurb under the list explaining the current item (RaspyJack's
 *     sub-label hint)
 *   - `i` opens a full in-depth info page for the selected tool
 *   - `v` toggles between three view modes: LIST / GRID / CAROUSEL
 *   - After 30s of no input, an animated screensaver takes over
 *
 * POSEIDON keyboard convention (no physical arrow keys on Cardputer):
 *   ; = up     . = down
 *   ENTER  = launch
 *   `      = back (PK_ESC)
 *   i      = info page for selected tool
 *   v      = cycle view mode
 */
#include "../../app.h"
#include "../../ui.h"
#include "../../input.h"
#include "saltyjack.h"
#include "saltyjack_style.h"
#include "saltyjack_icons.h"
#include "saltyjack_splash.h"
#include <Arduino.h>
#include <esp_random.h>

struct sj_menu_item_t {
    sj_icon_fn  icon;            /* procedural icon — scales 1x/2x/3x */
    const char *label;
    const char *blurb;           /* short one-liner — fits ~34 chars */
    const char *desc;            /* long description, '\n'-separated lines */
    void (*run)(void);
};

static const sj_menu_item_t SJ_ITEMS[] = {
    {
        icon_flag,
        "About",
        "Homage + arsenal overview",
        "SaltyJack is POSEIDON's LAN\n"
        "attack suite, ported direct\n"
        "from @7h30th3r0n3's Evil-M5\n"
        "and RaspyJack Pi firmware.\n"
        "\n"
        "This page credits him and\n"
        "lists the ported attacks.\n"
        "Go star both of his repos.",
        feat_saltyjack_info
    },
    {
        icon_skull,
        "DHCP Starve",
        "Exhaust DHCP pool w/ random MACs",
        "Requires: joined to target\n"
        "WiFi with our own lease.\n"
        "\n"
        "Spews Discover+Request with\n"
        "random plausible client MACs\n"
        "until the real DHCP server\n"
        "runs out of IPs and starts\n"
        "NAK'ing. New legitimate\n"
        "clients can't join.\n"
        "\n"
        "Pair with Rogue DHCP (STA)\n"
        "to race the broken server.",
        feat_saltyjack_dhcp_starve
    },
    {
        icon_swords,
        "Rogue DHCP (STA)",
        "Race real DHCP, poison leases",
        "Requires: joined to target\n"
        "WiFi.\n"
        "\n"
        "Binds :67 and races the real\n"
        "DHCP server. Our lease says\n"
        "WE are gateway + DNS + WPAD,\n"
        "so client traffic pivots\n"
        "through us.\n"
        "\n"
        "Best after DHCP Starve has\n"
        "emptied the real pool.",
        feat_saltyjack_dhcp_rogue_sta
    },
    {
        icon_wheel,
        "Rogue DHCP (AP)",
        "Evil-twin SoftAP + DHCP server",
        "Stands up an open SoftAP\n"
        "named POSEIDON-SaltyJack\n"
        "and serves DHCP ourselves.\n"
        "\n"
        "Any client that joins gets\n"
        "us as gateway, DNS, and\n"
        "WPAD (option 252). Every\n"
        "attack in this suite then\n"
        "lines up automatically.\n"
        "\n"
        "Pairs with Responder+WPAD\n"
        "for one-shot credential\n"
        "harvest.",
        feat_saltyjack_dhcp_rogue_ap
    },
    {
        icon_horn,
        "Responder",
        "Poison LLMNR/NBNS, SMB hashes",
        "Listens on:\n"
        "  LLMNR  udp/5355 mcast\n"
        "  NBT-NS udp/137   bcast\n"
        "  SMB    tcp/445\n"
        "\n"
        "Replies to ANY name lookup\n"
        "pointing the client at us,\n"
        "then runs the NTLMv2 Type\n"
        "1/2/3 dance on SMB and\n"
        "saves hashcat-format hashes\n"
        "to /poseidon/saltyjack/\n"
        "ntlm_hashes.txt",
        feat_saltyjack_responder
    },
    {
        icon_web,
        "WPAD NTLM",
        "PAC + 407 Proxy NTLM harvest",
        "HTTP :80 server.\n"
        "\n"
        "Serves /wpad.dat PAC telling\n"
        "browsers to proxy through\n"
        "us, then returns 407 with\n"
        "Proxy-Authenticate: NTLM.\n"
        "Windows auto-sends creds.\n"
        "\n"
        "We base64-decode the Type-3\n"
        "message and write hashcat\n"
        "format to ntlm_hashes.txt.\n"
        "\n"
        "Pair with Rogue DHCP and/or\n"
        "Responder so WPAD resolves.",
        feat_saltyjack_wpad
    },
    {
        icon_key,
        "NTLMv2 Crack",
        "On-device HMAC-MD5 wordlist",
        "Reads captured hashes from\n"
        "ntlm_hashes.txt and brute\n"
        "forces each against the\n"
        "wordlist in ntlm_wordlist.txt.\n"
        "\n"
        "Pure on-device HMAC-MD5 +\n"
        "MD4. Hits append to\n"
        "ntlm_found.txt.\n"
        "\n"
        "Keys:\n"
        " ENTER - skip user\n"
        " `     - abort run\n"
        "\n"
        "Default wordlist seeded\n"
        "first run.",
        feat_saltyjack_ntlm_crack
    },
};
#define SJ_ITEMS_N   (sizeof(SJ_ITEMS) / sizeof(SJ_ITEMS[0]))
#define SJ_WINDOW    5   /* rows visible in list view */
#define SJ_IDLE_MS   30000UL  /* screensaver after 30s idle */

/* ===== view modes ===== */
enum view_mode_t { VIEW_LIST, VIEW_GRID, VIEW_CAROUSEL };

/* ===== LIST view ===== */
static void draw_list(int cursor, int offset)
{
    auto &d = M5Cardputer.Display;
    sj_frame("SaltyJack");

    /* Row must fit a 16x16 sprite. */
    const int row_h = 16;
    int row_y0 = SJ_CONTENT_Y;

    int visible = (int)SJ_ITEMS_N < SJ_WINDOW ? (int)SJ_ITEMS_N : SJ_WINDOW;
    for (int i = 0; i < visible; ++i) {
        int idx = offset + i;
        if (idx >= (int)SJ_ITEMS_N) break;
        int y = row_y0 + i * row_h;
        bool sel = (idx == cursor);

        if (sel) {
            d.fillRect(SJ_FRAME_X + SJ_FRAME_TH + 1, y,
                       SJ_FRAME_W - 2 * SJ_FRAME_TH - 2, row_h, SJ_SEL_BG);
        }
        /* 16x16 pixel-art sprite at left. */
        SJ_ITEMS[idx].icon(SJ_CONTENT_X, y, 0, 1);

        d.setTextColor(sel ? SJ_SEL_FG : SJ_FG, sel ? SJ_SEL_BG : SJ_BG);
        d.setCursor(SJ_CONTENT_X + 20, y + 4);
        d.print(SJ_ITEMS[idx].label);
    }

    /* Scroll hints */
    if (offset > 0) {
        d.setTextColor(SJ_FG_DIM, SJ_BG);
        d.setCursor(SJ_FRAME_X + SJ_FRAME_W - 10, row_y0);
        d.print("^");
    }
    if (offset + SJ_WINDOW < (int)SJ_ITEMS_N) {
        d.setTextColor(SJ_FG_DIM, SJ_BG);
        d.setCursor(SJ_FRAME_X + SJ_FRAME_W - 10, row_y0 + (SJ_WINDOW - 1) * row_h);
        d.print("v");
    }

    /* Blurb line under the list — RaspyJack's sub-label hint. */
    int blurb_y = row_y0 + SJ_WINDOW * row_h + 4;
    d.drawFastHLine(SJ_CONTENT_X, blurb_y - 2,
                    SJ_FRAME_W - 2 * SJ_FRAME_TH - 4, SJ_ACCENT_DIM);
    d.setTextColor(SJ_FG_DIM, SJ_BG);
    d.setCursor(SJ_CONTENT_X, blurb_y);
    d.print(SJ_ITEMS[cursor].blurb);

    sj_footer(";/.  ent=go  i=info  v=view  `=back");
}

/* ===== GRID view (2 cols × N rows of tiles) ===== */
static void draw_grid(int cursor)
{
    auto &d = M5Cardputer.Display;
    sj_frame("SaltyJack");

    const int tile_w = (SJ_FRAME_W - 2 * SJ_FRAME_TH - 8) / 2;
    const int tile_h = 28;
    const int grid_x = SJ_FRAME_X + SJ_FRAME_TH + 3;
    const int grid_y = SJ_CONTENT_Y + 2;

    for (int i = 0; i < (int)SJ_ITEMS_N; ++i) {
        int col = i & 1;
        int row = i >> 1;
        int x = grid_x + col * (tile_w + 4);
        int y = grid_y + row * (tile_h + 3);
        if (y + tile_h > SJ_FRAME_Y + SJ_FRAME_H - 14) break;

        bool sel = (i == cursor);

        if (sel) d.fillRect(x, y, tile_w, tile_h, SJ_SEL_BG);
        d.drawRect(x, y, tile_w, tile_h, sel ? SJ_SEL_FG : SJ_ACCENT_DIM);

        /* 16x16 icon centered in the top half of the tile. */
        int ix = x + (tile_w - 16) / 2;
        int iy = y + 2;
        SJ_ITEMS[i].icon(ix, iy,
                         sel ? SJ_SEL_FG : SJ_ACCENT, 1);

        /* Label below */
        d.setTextColor(sel ? SJ_SEL_FG : SJ_FG, sel ? SJ_SEL_BG : SJ_BG);
        const char *lbl = SJ_ITEMS[i].label;
        int lw = (int)strlen(lbl) * 6;
        int lx = x + (tile_w - lw) / 2;
        d.setCursor(lx, y + tile_h - 10);
        d.print(lbl);
    }

    sj_footer(";/.  ent=go  i=info  v=view  `=back");
}

/* ===== CAROUSEL view — one giant icon + label centered ===== */
static void draw_carousel(int cursor)
{
    auto &d = M5Cardputer.Display;
    sj_frame("SaltyJack");

    /* Huge 48x48 procedural icon centered. */
    int ix = (SCR_W - 16 * 3) / 2;
    int iy = SJ_CONTENT_Y + 4;
    SJ_ITEMS[cursor].icon(ix, iy, SJ_ACCENT, 3);

    /* Label centered below icon */
    d.setTextColor(SJ_SEL_FG, SJ_BG);
    const char *lbl = SJ_ITEMS[cursor].label;
    int lw = (int)strlen(lbl) * 6;
    d.setCursor((SCR_W - lw) / 2, iy + 52);
    d.print(lbl);

    /* Blurb */
    d.setTextColor(SJ_FG_DIM, SJ_BG);
    const char *blurb = SJ_ITEMS[cursor].blurb;
    int bw = (int)strlen(blurb) * 6;
    d.setCursor((SCR_W - bw) / 2, iy + 62);
    d.print(blurb);

    /* Position dots at bottom (like iOS page indicator) */
    int dots_y = SJ_FRAME_Y + SJ_FRAME_H - 8;
    int dots_total_w = SJ_ITEMS_N * 6;
    int dots_x0 = (SCR_W - dots_total_w) / 2;
    for (int i = 0; i < (int)SJ_ITEMS_N; ++i) {
        if (i == cursor) d.fillCircle(dots_x0 + i * 6 + 2, dots_y, 2, SJ_ACCENT);
        else             d.drawCircle(dots_x0 + i * 6 + 2, dots_y, 2, SJ_ACCENT_DIM);
    }

    sj_footer(";/.  ent=go  i=info  v=view  `=back");
}

/* ===== INFO PAGE (per-tool deep dive) ===== */
static void show_info_page(int idx)
{
    auto &d = M5Cardputer.Display;
    char title[32];
    snprintf(title, sizeof(title), "%s info", SJ_ITEMS[idx].label);

    while (true) {
        sj_frame(title);

        /* Blurb at top (highlighted) */
        d.fillRect(SJ_FRAME_X + SJ_FRAME_TH + 1, SJ_CONTENT_Y - 1,
                   SJ_FRAME_W - 2 * SJ_FRAME_TH - 2, 10, SJ_SEL_BG);
        d.setTextColor(SJ_SEL_FG, SJ_SEL_BG);
        d.setCursor(SJ_CONTENT_X, SJ_CONTENT_Y);
        d.print(SJ_ITEMS[idx].blurb);

        /* Body text — pre-formatted with \n line breaks */
        int y = SJ_CONTENT_Y + 14;
        const int max_y = SJ_FRAME_Y + SJ_FRAME_H - 14;
        const char *p = SJ_ITEMS[idx].desc;
        char line[64];
        size_t li = 0;
        d.setTextColor(SJ_FG, SJ_BG);
        while (*p && y < max_y) {
            if (*p == '\n' || li >= sizeof(line) - 1) {
                line[li] = '\0';
                d.setCursor(SJ_CONTENT_X, y);
                d.print(line);
                y += 9;
                li = 0;
                if (*p == '\n') ++p;
                continue;
            }
            line[li++] = *p++;
        }
        if (li > 0 && y < max_y) {
            line[li] = '\0';
            d.setCursor(SJ_CONTENT_X, y);
            d.print(line);
        }

        sj_footer("ent/`=back");

        /* Any key dismisses */
        while (true) {
            uint16_t k = input_poll();
            if (k == PK_NONE) { delay(20); continue; }
            return;
        }
    }
}

/* ===== SCREENSAVER — procedural ocean waves ===== */
static void run_screensaver(void)
{
    auto &d = M5Cardputer.Display;

    /* Independent drifting wave columns. */
    struct wave_col { int x; int y; int speed; uint16_t color; };
    const int N = 14;
    wave_col cols[N];
    for (int i = 0; i < N; ++i) {
        cols[i].x = (SCR_W * i) / N + (esp_random() % 6);
        cols[i].y = (int)(esp_random() % BODY_H);
        cols[i].speed = 1 + (esp_random() % 3);
        uint16_t choices[] = { SJ_FG_DIM, SJ_FG, SJ_ACCENT_DIM, SJ_ACCENT };
        cols[i].color = choices[esp_random() % 4];
    }

    uint32_t frame = 0;
    const char *wave = (const char *)"\xE2\x89\x8B";  /* ≋ */

    while (true) {
        /* Plain black wash each frame — cheap, 240x(BODY_H) is ~16KB pixels
         * but the ST7789 SPI DMA handles it fast enough at ~20 fps. */
        d.fillRect(0, BODY_Y, SCR_W, BODY_H, SJ_BG);

        /* Drifting wave columns */
        for (int i = 0; i < N; ++i) {
            cols[i].y += cols[i].speed;
            if (cols[i].y > BODY_Y + BODY_H) {
                cols[i].y = BODY_Y - 10;
                cols[i].x = (int)(esp_random() % SCR_W);
            }
            d.setTextColor(cols[i].color, SJ_BG);
            d.setCursor(cols[i].x, cols[i].y);
            d.print(wave);
        }

        /* Pulsing centered SaltyJack title — color cycles with frame */
        uint16_t pulse_col;
        int phase = (frame / 2) % 60;
        if      (phase < 20) pulse_col = SJ_FG;
        else if (phase < 40) pulse_col = SJ_ACCENT;
        else                 pulse_col = SJ_SEL_FG;

        d.setTextSize(2);
        d.setTextColor(pulse_col, SJ_BG);
        const char *title = "SaltyJack";
        int tw = (int)strlen(title) * 12;
        d.setCursor((SCR_W - tw) / 2, BODY_Y + BODY_H / 2 - 8);
        d.print(title);
        d.setTextSize(1);

        /* Dim hint */
        d.setTextColor(SJ_FG_DIM, SJ_BG);
        const char *hint = "any key";
        int hw = (int)strlen(hint) * 6;
        d.setCursor((SCR_W - hw) / 2, BODY_Y + BODY_H / 2 + 12);
        d.print(hint);

        /* Break on any input */
        if (input_poll() != PK_NONE) return;

        delay(50);
        frame++;
    }
}

/* ===== BOOT SPLASH =====
 *
 * Full-screen RGB565 PNG baked to flash (saltyjack_splash.h). We take
 * over the entire 240x135 display for a moment — status bar + footer
 * included — so the splash reads like a real boot screen. Any key
 * skips; otherwise auto-advances after ~1.5s.
 */
static void run_boot_splash(void)
{
    auto &d = M5Cardputer.Display;

    d.fillScreen(0x0000);
    d.pushImage(0, 0, SALTYJACK_SPLASH_W, SALTYJACK_SPLASH_H, saltyjack_splash);

    uint32_t start = millis();
    while (millis() - start < 1500) {
        if (input_poll() != PK_NONE) break;
        delay(20);
    }
}

/* ===== ROOT — dispatch everything ===== */
void feat_saltyjack_root(void)
{
    run_boot_splash();

    int cursor = 0;
    int offset = 0;
    view_mode_t view = VIEW_LIST;
    uint32_t last_input = millis();

    while (true) {
        /* Keep cursor visible in list view. */
        if (cursor < offset) offset = cursor;
        else if (cursor >= offset + SJ_WINDOW) offset = cursor - SJ_WINDOW + 1;

        /* Draw current view. */
        switch (view) {
            case VIEW_LIST:     draw_list(cursor, offset);   break;
            case VIEW_GRID:     draw_grid(cursor);           break;
            case VIEW_CAROUSEL: draw_carousel(cursor);       break;
        }

        /* Wait for a key with idle-timeout → screensaver. */
        uint16_t k;
        while (true) {
            k = input_poll();
            if (k != PK_NONE) break;
            if (millis() - last_input > SJ_IDLE_MS) {
                run_screensaver();
                last_input = millis();
                break;  /* redraw, don't process key */
            }
            delay(20);
        }
        if (k == PK_NONE) continue;
        last_input = millis();

        if (k == ';' || k == PK_UP) {
            cursor = (cursor - 1 + SJ_ITEMS_N) % SJ_ITEMS_N;
        } else if (k == '.' || k == PK_DOWN) {
            cursor = (cursor + 1) % SJ_ITEMS_N;
        } else if (k == PK_ENTER) {
            if (SJ_ITEMS[cursor].run) SJ_ITEMS[cursor].run();
        } else if (k == 'i' || k == 'I') {
            show_info_page(cursor);
        } else if (k == 'v' || k == 'V') {
            view = (view_mode_t)((view + 1) % 3);
        } else if (k == PK_ESC) {
            return;
        }
    }
}
