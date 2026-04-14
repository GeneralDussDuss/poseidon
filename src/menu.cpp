/*
 * menu.cpp — hierarchical menu tree + runtime.
 *
 * The tree is declared below with letter mnemonics per level. Feature
 * implementations live in features/ — they expose a single entry point
 * that the menu invokes.
 */
#include "menu.h"
#include "ui.h"
#include "input.h"
#include "radio.h"

/* ---- forward decls for feature entry points ---- */
extern void feat_wifi_scan(void);
extern void feat_wifi_deauth(void);
extern void feat_wifi_portal(void);
extern void feat_wifi_beacon_spam(void);
extern void feat_ble_scan(void);
extern void feat_ble_spam(void);
extern void feat_ble_hid(void);
extern void feat_ir_tvbgone(void);
extern void feat_ir_remote(void);
extern void feat_file_browser(void);
extern void feat_settings(void);
extern void feat_about(void);

/* ---- menu tree ---- */

static const menu_node_t MENU_WIFI[] = {
    { 's', "Scan",        "Scan + list nearby APs with RSSI",     nullptr, feat_wifi_scan },
    { 'd', "Deauth",      "Jam target AP (type BSSID or pick)",   nullptr, feat_wifi_deauth },
    { 'p', "Portal",      "Evil captive portal",                   nullptr, feat_wifi_portal },
    { 'b', "Beacon spam", "Broadcast fake SSIDs",                  nullptr, feat_wifi_beacon_spam },
    { 0,   nullptr,       nullptr, nullptr, nullptr },
};

static const menu_node_t MENU_BLE[] = {
    { 's', "Scan",      "Discover BLE devices",          nullptr, feat_ble_scan },
    { 'p', "Spam",      "Apple/Samsung/etc popups",      nullptr, feat_ble_spam },
    { 'h', "Bad-KB",    "BLE HID keyboard attack",       nullptr, feat_ble_hid },
    { 0,   nullptr, nullptr, nullptr, nullptr },
};

static const menu_node_t MENU_IR[] = {
    { 't', "TV-B-Gone", "Kill nearby TVs",                nullptr, feat_ir_tvbgone },
    { 'r', "Remote",    "Virtual Samsung remote",         nullptr, feat_ir_remote },
    { 0,   nullptr, nullptr, nullptr, nullptr },
};

static const menu_node_t MENU_SYS[] = {
    { 'f', "Files",     "SD card browser",                nullptr, feat_file_browser },
    { 's', "Settings",  "Config + preferences",           nullptr, feat_settings },
    { 'a', "About",     "Build info",                     nullptr, feat_about },
    { 0,   nullptr, nullptr, nullptr, nullptr },
};

const menu_node_t MENU_ROOT_CHILDREN[] = {
    { 'w', "WiFi",      "WiFi recon and attacks",         MENU_WIFI, nullptr },
    { 'b', "Bluetooth", "BLE scan / spam / HID",          MENU_BLE,  nullptr },
    { 'i', "IR",        "Infrared blaster",               MENU_IR,   nullptr },
    { 's', "System",    "Files, settings, about",         MENU_SYS,  nullptr },
    { 0,   nullptr, nullptr, nullptr, nullptr },
};

const menu_node_t MENU_ROOT = {
    '/', "POSEIDON", "press letter to enter",
    MENU_ROOT_CHILDREN, nullptr
};

/* -------------------- render + nav -------------------- */

static int count_children(const menu_node_t *parent)
{
    int n = 0;
    for (const menu_node_t *c = parent->children; c && c->hotkey; ++c) ++n;
    return n;
}

static void draw_menu(const menu_node_t *parent, int cursor)
{
    ui_clear_body();
    auto &d = M5Cardputer.Display;

    /* Title line. */
    d.setTextColor(COL_ACCENT, COL_BG);
    d.setCursor(4, BODY_Y + 2);
    d.printf("> %s", parent->label);

    int n = count_children(parent);
    int i = 0;
    for (const menu_node_t *c = parent->children; c && c->hotkey; ++c, ++i) {
        int y = BODY_Y + 16 + i * 12;
        bool sel = (i == cursor);
        uint16_t fg = sel ? COL_BG : COL_FG;
        uint16_t bg = sel ? COL_ACCENT : COL_BG;
        if (sel) d.fillRect(0, y - 1, SCR_W, 11, bg);
        d.setTextColor(fg, bg);
        d.setCursor(4, y);
        d.printf("[%c] %s", toupper(c->hotkey), c->label);
    }

    /* Hint row — pulled from selected item. */
    if (cursor >= 0 && cursor < n) {
        const menu_node_t *sel = &parent->children[cursor];
        if (sel->hint) {
            d.setTextColor(COL_DIM, COL_BG);
            int y = BODY_Y + 16 + n * 12 + 4;
            if (y < FOOTER_Y - 10) {
                d.setCursor(4, y);
                d.print(sel->hint);
            }
        }
    }
}

static void run_submenu(const menu_node_t *parent)
{
    int cursor = 0;
    int n = count_children(parent);

    ui_draw_status(radio_name(), "");
    ui_draw_footer("letter=go  ;/.=move  ENTER=select  ESC=back");
    draw_menu(parent, cursor);

    while (true) {
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(10); continue; }

        if (k == PK_ESC) return;
        if (k == PK_UP)    { cursor = (cursor - 1 + n) % n; draw_menu(parent, cursor); continue; }
        if (k == PK_DOWN)  { cursor = (cursor + 1) % n;     draw_menu(parent, cursor); continue; }
        if (k == PK_ENTER) {
            const menu_node_t *sel = &parent->children[cursor];
            if (sel->action) { sel->action(); }
            else if (sel->children) { run_submenu(sel); }
            ui_draw_status(radio_name(), "");
            ui_draw_footer("letter=go  ;/.=move  ENTER=select  ESC=back");
            draw_menu(parent, cursor);
            continue;
        }

        /* Letter mnemonic — jump + execute. */
        if (k >= 0x20 && k < 0x7F) {
            char c = (char)tolower((int)k);
            int i = 0;
            for (const menu_node_t *ch = parent->children; ch && ch->hotkey; ++ch, ++i) {
                if (ch->hotkey == c) {
                    cursor = i;
                    draw_menu(parent, cursor);
                    if (ch->action) { ch->action(); }
                    else if (ch->children) { run_submenu(ch); }
                    ui_draw_status(radio_name(), "");
                    ui_draw_footer("letter=go  ;/.=move  ENTER=select  ESC=back");
                    draw_menu(parent, cursor);
                    break;
                }
            }
        }
    }
}

void menu_run(void)
{
    run_submenu(&MENU_ROOT);
}
