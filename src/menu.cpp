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
extern void feat_wifi_deauth_broadcast(void);
extern void feat_wifi_deauth_detect(void);
extern void feat_wifi_portal(void);
extern void feat_wifi_apclone(void);
extern void feat_wifi_beacon_spam(void);
extern void feat_wifi_wardrive(void);
extern void feat_wifi_probe(void);
extern void feat_wifi_karma(void);
extern void feat_wifi_pmkid(void);
extern void feat_wifi_connect(void);
extern void feat_ble_scan(void);
extern void feat_ble_spam(void);
extern void feat_ble_hid(void);
extern void feat_ble_tracker(void);
extern void feat_ble_sniff(void);
extern void feat_ble_beacon(void);
extern void feat_ir_tvbgone(void);
extern void feat_ir_remote(void);
extern void feat_mesh(void);
extern void feat_file_browser(void);
extern void feat_settings(void);
extern void feat_about(void);
extern void feat_badusb(void);
extern void feat_net_portscan(void);
extern void feat_net_ping(void);
extern void feat_net_dns(void);
extern void feat_clock(void);

/* ---- menu tree ---- */

static const menu_node_t MENU_WIFI[] = {
    { 's', "Scan",        "Scan + list nearby APs",                nullptr, feat_wifi_scan },
    { 'd', "Deauth",      "Jam target AP (typed or picked)",       nullptr, feat_wifi_deauth },
    { 'x', "Deauth all",  "Broadcast deauth all clients of AP",    nullptr, feat_wifi_deauth_broadcast },
    { 'e', "Deauth det.", "Passive deauth frame detector",         nullptr, feat_wifi_deauth_detect },
    { 'c', "AP Clone",    "Mirror scanned AP, lure clients",       nullptr, feat_wifi_apclone },
    { 'p', "Portal",      "Evil captive portal (4 templates)",      nullptr, feat_wifi_portal },
    { 'k', "Karma",       "Auto-respond to probe requests",        nullptr, feat_wifi_karma },
    { 'b', "Beacon spam", "Broadcast fake SSIDs",                  nullptr, feat_wifi_beacon_spam },
    { 'r', "Probe sniff", "Log probe requests + clients",          nullptr, feat_wifi_probe },
    { 'm', "PMKID cap",   "EAPOL M1 -> hashcat 22000",             nullptr, feat_wifi_pmkid },
    { 'w', "Wardrive",    "Channel hop + GPS -> WiGLE CSV",        nullptr, feat_wifi_wardrive },
    { 'n', "Connect",     "Join saved WiFi network",                nullptr, feat_wifi_connect },
    { 0,   nullptr,       nullptr, nullptr, nullptr },
};

static const menu_node_t MENU_MESH[] = {
    { 's', "Status",    "Live peer table + broadcast",    nullptr, feat_mesh },
    { 0,   nullptr, nullptr, nullptr, nullptr },
};

static const menu_node_t MENU_BLE[] = {
    { 's', "Scan",      "Discover BLE devices",          nullptr, feat_ble_scan },
    { 'p', "Spam",      "Apple/Samsung/etc popups",      nullptr, feat_ble_spam },
    { 'h', "Bad-KB",    "BLE HID keyboard attack",       nullptr, feat_ble_hid },
    { 't', "Tracker",   "Detect AirTag/SmartTag/Tile",   nullptr, feat_ble_tracker },
    { 'n', "Sniffer",   "Log all BLE adv -> SD CSV",     nullptr, feat_ble_sniff },
    { 'b', "iBeacon",   "Broadcast an iBeacon",          nullptr, feat_ble_beacon },
    { 0,   nullptr, nullptr, nullptr, nullptr },
};

static const menu_node_t MENU_NET[] = {
    { 'p', "Port scan", "TCP portscan a host",            nullptr, feat_net_portscan },
    { 'i', "Ping",      "ICMP echo loop",                 nullptr, feat_net_ping },
    { 'd', "DNS",       "Lookup a hostname",              nullptr, feat_net_dns },
    { 'c', "Connect",   "Join saved WiFi network",        nullptr, feat_wifi_connect },
    { 0,   nullptr, nullptr, nullptr, nullptr },
};

static const menu_node_t MENU_IR[] = {
    { 't', "TV-B-Gone", "Kill nearby TVs",                nullptr, feat_ir_tvbgone },
    { 'r', "Remote",    "Virtual Samsung remote",         nullptr, feat_ir_remote },
    { 0,   nullptr, nullptr, nullptr, nullptr },
};

static const menu_node_t MENU_SYS[] = {
    { 'f', "Files",     "SD card browser",                nullptr, feat_file_browser },
    { 'c', "Clock",     "Uptime / GPS clock",             nullptr, feat_clock },
    { 's', "Settings",  "Config + preferences",           nullptr, feat_settings },
    { 'a', "About",     "Build info",                     nullptr, feat_about },
    { 0,   nullptr, nullptr, nullptr, nullptr },
};

const menu_node_t MENU_ROOT_CHILDREN[] = {
    { 'w', "WiFi",      "WiFi recon + attacks + wardrive", MENU_WIFI, nullptr },
    { 'b', "Bluetooth", "BLE scan / spam / HID / tracker", MENU_BLE,  nullptr },
    { 'i', "IR",        "Infrared blaster + remote",       MENU_IR,   nullptr },
    { 'u', "BadUSB",    "USB-HID payload runner",          nullptr,   feat_badusb },
    { 'n', "Network",   "Port scan / ping / DNS / connect", MENU_NET,  nullptr },
    { 'm', "Mesh",      "PigSync ESP-NOW peer mesh",       MENU_MESH, nullptr },
    { 's', "System",    "Files, clock, settings",          MENU_SYS,  nullptr },
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

    /* Title: bold accent + underline bar. */
    d.setTextColor(COL_ACCENT, COL_BG);
    d.setCursor(4, BODY_Y + 2);
    d.printf("%s", parent->label);
    int tw = d.textWidth(parent->label);
    d.drawFastHLine(4, BODY_Y + 12, tw + 6, COL_ACCENT);

    int n = count_children(parent);
    int i = 0;
    for (const menu_node_t *c = parent->children; c && c->hotkey; ++c, ++i) {
        int y = BODY_Y + 18 + i * 13;
        bool sel = (i == cursor);
        /* Rounded-rect highlight for the selected row. */
        if (sel) {
            d.fillRoundRect(2, y - 1, SCR_W - 4, 12, 2, 0x18C7);
            d.drawRoundRect(2, y - 1, SCR_W - 4, 12, 2, COL_ACCENT);
        }
        uint16_t fg = sel ? COL_ACCENT : COL_FG;
        /* Hotkey in accent color, always. */
        d.setTextColor(sel ? COL_WARN : COL_ACCENT, sel ? 0x18C7 : COL_BG);
        d.setCursor(6, y + 1);
        d.printf("[%c]", toupper(c->hotkey));
        d.setTextColor(fg, sel ? 0x18C7 : COL_BG);
        d.setCursor(30, y + 1);
        d.print(c->label);
        /* Chevron on submenus, dot on actions. */
        d.setTextColor(COL_DIM, sel ? 0x18C7 : COL_BG);
        d.setCursor(SCR_W - 12, y + 1);
        d.print(c->action ? "." : ">");
    }

    /* Hint strip for the selected item. */
    if (cursor >= 0 && cursor < n) {
        const menu_node_t *sel = &parent->children[cursor];
        if (sel->hint) {
            int y = BODY_Y + 18 + n * 13 + 4;
            if (y < FOOTER_Y - 10) {
                d.setTextColor(COL_DIM, COL_BG);
                d.setCursor(4, y);
                d.print("» ");
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
    ui_draw_footer("letter=go  ;/.=move  ENTER=select  `=back");
    draw_menu(parent, cursor);

    while (true) {
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(10); continue; }

        if (k == PK_ESC) return;
        if (k == PK_ENTER) {
            const menu_node_t *sel = &parent->children[cursor];
            if (sel->action) { sel->action(); }
            else if (sel->children) { run_submenu(sel); }
            ui_draw_status(radio_name(), "");
            ui_draw_footer("letter=go  ;/.=move  ENTER=select  `=back");
            draw_menu(parent, cursor);
            continue;
        }

        /* Menu-level nav translation: ; = up, . = down (no FN needed).
         * These chars also appear in text input — but input_line()
         * handles those raw, so only menus see these as arrows. */
        if (k == ';' || k == PK_UP)    { cursor = (cursor - 1 + n) % n; draw_menu(parent, cursor); continue; }
        if (k == '.' || k == PK_DOWN)  { cursor = (cursor + 1) % n;     draw_menu(parent, cursor); continue; }

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
                    ui_draw_footer("letter=go  ;/.=move  ENTER=select  `=back");
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
