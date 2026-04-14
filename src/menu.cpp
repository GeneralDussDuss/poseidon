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
extern void feat_wifi_clients(void);
extern void feat_wifi_clients_all(void);
extern void feat_wifi_portal(void);
extern void feat_wifi_apclone(void);
extern void feat_wifi_beacon_spam(void);
extern void feat_wifi_wardrive(void);
extern void feat_wifi_probe(void);
extern void feat_wifi_karma(void);
extern void feat_wifi_pmkid(void);
extern void feat_wifi_spectrum(void);
extern void feat_wifi_connect(void);
extern void feat_ble_scan(void);
extern void feat_ble_spam(void);
extern void feat_ble_hid(void);
extern void feat_ble_tracker(void);
extern void feat_ble_sniff(void);
extern void feat_ble_beacon(void);
extern void feat_ble_clone(void);
extern void feat_ble_finder(void);
extern void feat_ble_gatt(void);
extern void feat_ble_flood(void);
extern void feat_ble_karma(void);
extern void feat_ble_sourapple(void);
extern void feat_ble_findmy(void);
extern void feat_ble_toys(void);
extern void feat_ir_tvbgone(void);
extern void feat_ir_remote(void);
extern void feat_mesh(void);
extern void feat_triton(void);
extern void feat_c5_status(void);
extern void feat_c5_scan_5g(void);
extern void feat_c5_scan_zb(void);
extern void feat_tool_sd_format(void);
extern void feat_tool_flashlight(void);
extern void feat_tool_screen_test(void);
extern void feat_tool_stopwatch(void);
extern void feat_tool_chance(void);
extern void feat_tool_morse(void);
extern void feat_tool_mac_rand(void);
extern void feat_tool_calc(void);
extern void feat_file_browser(void);
extern void feat_settings(void);
extern void feat_about(void);
extern void feat_badusb(void);
extern void feat_net_portscan(void);
extern void feat_net_ping(void);
extern void feat_net_dns(void);
extern void feat_net_responder(void);
extern void feat_net_ssdp(void);
extern void feat_net_lanrecon(void);
extern void feat_clock(void);

/* ---- menu tree ---- */

static const menu_node_t MENU_WIFI[] = {
    { 's', "Scan", "Scan + list nearby APs", nullptr, feat_wifi_scan,
      "Actively scans 2.4 GHz for nearby WiFi APs. Shows SSID, BSSID, "
      "channel, RSSI, and auth type. Press ENTER on an AP to open details "
      "where hotkeys D/X/C/P jump straight into attacks against that AP." },
    { 'l', "Clients", "Hunt ALL clients (all channels)", nullptr, feat_wifi_clients_all,
      "Channel-hops 1-13 in promisc mode, catalogs every STA-BSSID pair. "
      "Hotkeys per selected client: D=unicast deauth, X=broadcast deauth the "
      "whole AP, L=lock channel, H=resume hop." },
    { 'o', "AP Clients", "List STAs on last-scanned AP only", nullptr, feat_wifi_clients,
      "Channel-locks to one selected AP and lists only the clients associated "
      "with it. Faster than the global hunt when you already know your target." },
    { 'd', "Deauth", "Jam target AP (typed or picked)", nullptr, feat_wifi_deauth,
      "Sends 802.11 deauthentication frames spoofing the target AP. Disconnects "
      "clients repeatedly. Can type a BSSID manually or hand off from Scan." },
    { 'x', "Deauth all", "Broadcast deauth all clients of AP", nullptr, feat_wifi_deauth_broadcast,
      "Broadcast deauth addressed to FF:FF:FF:FF:FF:FF — kicks every client "
      "of the target AP simultaneously. Combine with Handshake capture for fast "
      "4-way grabs on reconnect." },
    { 'e', "Deauth det.", "Passive deauth frame detector", nullptr, feat_wifi_deauth_detect,
      "Sniffs the air for deauth/disassoc frames. Shows live rate + last source "
      "BSSID. Catches other attackers near you." },
    { 'c', "AP Clone", "Mirror scanned AP, lure clients", nullptr, feat_wifi_apclone,
      "Spins up a SoftAP using the last-scanned target's SSID. Devices that "
      "saved the real network may auto-roam to us. Pair with Portal for creds." },
    { 'p', "Portal", "Evil captive portal (4 templates)", nullptr, feat_wifi_portal,
      "Captive portal with DNS hijack. Templates: Google, Facebook, Microsoft, "
      "Free WiFi. Logs creds to /poseidon/creds.log on SD." },
    { 'k', "Karma", "Auto-respond to probe requests", nullptr, feat_wifi_karma,
      "Sniffs probe requests, then spins up a SoftAP named with whatever SSID "
      "the target was asking for. Phones may auto-connect to saved networks." },
    { 'b', "Beacon spam", "Broadcast fake SSIDs", nullptr, feat_wifi_beacon_spam,
      "Pumps out fake beacon frames so clients see a ton of SSIDs that don't "
      "exist. Built-in meme list + rickroll + custom typed entries." },
    { 'r', "Probe sniff", "Log probe requests + clients", nullptr, feat_wifi_probe,
      "Passive: logs which SSIDs each nearby device is probing for. Great for "
      "profiling — you learn the networks a target has saved." },
    { 'm', "PMKID cap", "EAPOL M1 -> hashcat 22000", nullptr, feat_wifi_pmkid,
      "Captures both PMKIDs (passive) AND full 4-way handshakes (active). "
      "Output is hashcat mode-22000 format on SD. H toggles HUNT mode which "
      "deauths every seen AP to force reconnections." },
    { 'g', "Spectrum", "2.4 GHz live channel activity bars", nullptr, feat_wifi_spectrum,
      "Real-time RF spectrum. Hops channels 1-13, shows peak RSSI per channel "
      "as colored bars. Red = strong signal. R resets peaks." },
    { 'w', "Wardrive", "Channel hop + GPS -> WiGLE CSV", nullptr, feat_wifi_wardrive,
      "Channel-hopping beacon logger with GPS from the LoRa-GNSS HAT. Output "
      "is WiGLE v1.6 CSV — upload to wigle.net for points." },
    { 'n', "Connect", "Join saved WiFi network", nullptr, feat_wifi_connect,
      "Saves an SSID+password to Preferences and connects STA. Required for "
      "Network tools (port scan, ping, DNS) to work." },
    { 0, nullptr, nullptr, nullptr, nullptr, nullptr },
};

static const menu_node_t MENU_MESH[] = {
    { 's', "Status", "Live peer table + broadcast", nullptr, feat_mesh,
      "ESP-NOW presence beacon + peer table. Broadcasts a HELLO frame every 5s "
      "with name/heap/GPS. Eviction after 30s silence. The enabler for the "
      "C5 drop-node C2 concept when those boards arrive." },
    { 0, nullptr, nullptr, nullptr, nullptr, nullptr },
};

static const menu_node_t MENU_C5[] = {
    { 's', "Status", "Show connected C5 nodes", nullptr, feat_c5_status,
      "Live peer table of C5 nodes. Auto-connects via ESP-NOW HELLO. P pings "
      "every peer, S broadcasts STOP to halt any running scan. Status dot in "
      "the corner is green when any C5 is online, red when none are." },
    { '5', "Scan 5G+2G", "Remote dual-band WiFi scan", nullptr, feat_c5_scan_5g,
      "Sends CMD_SCAN_5G to every C5 peer. Each C5 runs a dual-band WiFi scan "
      "(2.4 + 5 GHz) and streams results back in batches of 4 APs per ESP-NOW "
      "frame. We dedup by BSSID and sort by RSSI. 5G APs tagged with a "
      "magenta 5G badge, 2G in cyan. First pocket tool that can see 5 GHz." },
    { 'z', "Zigbee sniff", "Remote 802.15.4 capture", nullptr, feat_c5_scan_zb,
      "Sends CMD_SCAN_ZB with 0xFF (channel hop 11-26). C5 puts its 802.15.4 "
      "radio in promisc and streams frame summaries (channel, RSSI, type, PAN, "
      "addresses) back over ESP-NOW. Catches Zigbee beacons, Thread packets, "
      "smart locks/bulbs, anything on 802.15.4 in range." },
    { 0, nullptr, nullptr, nullptr, nullptr, nullptr },
};

static const menu_node_t MENU_BLE[] = {
    { 's', "Scan", "Discover BLE devices", nullptr, feat_ble_scan,
      "Passive NimBLE scan with device identification. ENTER on a device "
      "opens details where G/C/H/X/P hotkeys fire GATT/Clone/Bad-KB/Flood/Spam "
      "against it. / filters by name. R rescans." },
    { 'p', "Spam", "Apple/Samsung/etc popups", nullptr, feat_ble_spam,
      "Broadcasts fake advertisements that trigger native pairing popups on "
      "nearby phones. Pick brand: Apple (AirPods), Samsung (SmartTag), Google "
      "(FastPair), Microsoft (Swift Pair), or All to cycle." },
    { 'h', "Bad-KB", "BLE HID keyboard attack", nullptr, feat_ble_hid,
      "Advertises as a BLE HID keyboard with a random disguise name. Pair from "
      "your target device (settings -> Bluetooth). Once paired: T=type freeform, "
      "R=rickroll, L=lock workstation." },
    { 't', "Tracker", "Detect AirTag/SmartTag/Tile", nullptr, feat_ble_tracker,
      "Scans for Apple Find My, Samsung SmartTag, and Tile trackers. Shows "
      "distance class (CLOSE/NEAR/FAR), signal bar, MAC. New detection "
      "triggers a red border flash + two-tone chirp." },
    { 'f', "Finder", "Hunt a rogue tracker (geiger)", nullptr, feat_ble_finder,
      "Pick a tracker from the list, then HUNT mode turns the screen into a "
      "big proximity meter. Beep rate speeds up as you get closer, like a "
      "metal detector. Use to physically locate a tracker following you." },
    { 'n', "Sniffer", "Log all BLE adv -> SD CSV", nullptr, feat_ble_sniff,
      "Dumps every BLE advertisement to /poseidon/blesniff-ts.csv with "
      "timestamp, MAC, RSSI, name, and raw adv hex. Useful for passive "
      "reconnaissance or offline analysis." },
    { 'b', "iBeacon", "Broadcast an iBeacon", nullptr, feat_ble_beacon,
      "Broadcasts a standard iBeacon advertisement with a fixed UUID + major + "
      "minor. Apps like Locate Beacon on iOS will pick it up." },
    { 'c', "Clone", "Rebroadcast last scanned MAC", nullptr, feat_ble_clone,
      "Takes the last-scanned device (from Scan) and rebroadcasts its MAC + "
      "name as a CONNECTABLE advertisement with a minimal GATT server. "
      "Phones can enumerate our shell — good for honeypots." },
    { 'g', "GATT", "Connect + enumerate + r/w", nullptr, feat_ble_gatt,
      "Pocket nRF-Connect. Connects to the last-scanned device, walks its "
      "services and characteristics, lets you READ values or WRITE hex bytes. "
      "Essential for probing smart locks, IoT devices, and auth bypass testing." },
    { 'x', "Flood", "DoS connection storm → target", nullptr, feat_ble_flood,
      "Hammers the target's BLE connection queue with rapid connect attempts "
      "from random MACs. Effective against embedded peripherals (smart locks, "
      "fitness trackers, speakers) that hold only a few connections. Does NOT "
      "pop notifications on phones — use Spam or SourApple for that." },
    { 'k', "Karma", "Rotate identity, lure pairings", nullptr, feat_ble_karma,
      "Cycles through 16 popular device names every 2s, each advertised as a "
      "connectable SoftAP with a fresh random MAC. Phones scanning for a known "
      "device may auto-pair with us." },
    { 'a', "SourApple", "iOS 17 notification DoS", nullptr, feat_ble_sourapple,
      "CVE-2023-42941. Spams Apple Continuity Nearby Action frames with "
      "cycling action bytes. Crashes iOS 17 pre-17.2. On 17.2+ you get "
      "persistent unclosable popup dialogs. Credit ECTO-1A + RapierXbox." },
    { 'y', "Find My", "Fake AirTag broadcaster", nullptr, feat_ble_findmy,
      "Broadcasts fake Apple Find My / AirTag advertisements with random "
      "rotating keys. Passing iPhones with Find My enabled relay your 'tags' "
      "to iCloud's location service. Modes: 1 tag, flock of 8, flock of 32." },
    { 'd', "Salty Deep", "Wireless toy scanner + controller", nullptr, feat_ble_toys,
      "Scans for Lovense / WeVibe / Satisfyer / Svakom / Kiiroo / Lelo / "
      "Magic Motion devices. Connect to a Lovense and control vibration "
      "intensity 0-20 via the keyboard. Number keys 1-9 jump to a level; "
      "; and . nudge up/down; SPACE or 0 stops." },
    { 0, nullptr, nullptr, nullptr, nullptr, nullptr },
};

static const menu_node_t MENU_NET[] = {
    { 'p', "Port scan", "TCP portscan a host", nullptr, feat_net_portscan,
      "Connects to a range of TCP ports on a host IP/name. Requires you be "
      "joined to a WiFi network first (use Network -> Connect)." },
    { 'i', "Ping", "ICMP echo loop", nullptr, feat_net_ping,
      "Live ping. Shows round-trip time and sequence. Runs until you ESC. "
      "Requires STA connection first." },
    { 'd', "DNS", "Lookup a hostname", nullptr, feat_net_dns,
      "Resolve a hostname to its A record. Requires STA connection." },
    { 'c', "Connect", "Join saved WiFi network", nullptr, feat_wifi_connect,
      "Same as WiFi > Connect — saves credentials, joins a network as STA." },
    { 'r', "Responder", "Poison LLMNR/NBT-NS/mDNS", nullptr, feat_net_responder,
      "Classic pentest credential-capture trick. When DNS fails on a LAN, "
      "Windows/macOS/Linux fall back to LLMNR, NBT-NS, and mDNS — we answer "
      "every query with our IP. Targets that trust the reply send us an NTLM "
      "auth challenge which we log to /poseidon/ntlm.log for hashcat mode 5600." },
    { 'a', "LAN Recon", "Auto sweep + portscan + banners", nullptr, feat_net_lanrecon,
      "RaspyJack-style drop-box auto recon. Once you're joined to a WiFi "
      "network, this chains: ARP sweep of the /24 to find live hosts → "
      "TCP portscan of 16 common ports per host → banner grab on HTTP/SSH/"
      "Telnet → OUI vendor lookup on every MAC → full CSV export to "
      "/poseidon/lan.csv. Result list is scrollable; ENTER on a host shows "
      "its full port map + banner." },
    { 'u', "UPnP scan", "Discover LAN UPnP devices", nullptr, feat_net_ssdp,
      "Sends SSDP M-SEARCH to 239.255.255.250:1900 and collects responses. "
      "Fetches each device's XML description to pull friendlyName + modelName. "
      "Great for mapping internal IoT: routers, printers, smart TVs, cameras, "
      "NAS, Sonos, Chromecasts. Saves to /poseidon/ssdp.csv." },
    { 0, nullptr, nullptr, nullptr, nullptr, nullptr },
};

static const menu_node_t MENU_IR[] = {
    { 't', "TV-B-Gone", "Kill nearby TVs", nullptr, feat_ir_tvbgone,
      "Cycles power-off IR codes for Sony, Samsung, LG, Panasonic, Philips, "
      "Vizio. Point the top edge of the Cardputer at the TV. Runs until ESC." },
    { 'r', "Remote", "Virtual Samsung remote", nullptr, feat_ir_remote,
      "Virtual Samsung TV remote. P=power, M=mute, +/-=volume, ;/.=channel, "
      "1-9=digits, I=source, H=home, B=back." },
    { 0, nullptr, nullptr, nullptr, nullptr, nullptr },
};

static const menu_node_t MENU_TOOLS[] = {
    { 'l', "Flashlight", "Full-screen white torch", nullptr, feat_tool_flashlight,
      "Fills the screen with white. Simple panic light." },
    { 's', "Stopwatch", "Timer with laps", nullptr, feat_tool_stopwatch,
      "Classic stopwatch. SPACE starts/stops, L records a lap (up to 4), "
      "R resets." },
    { 'd', "Dice/8ball", "Dice, coin flip, magic 8-ball", nullptr, feat_tool_chance,
      "Randomizers. M cycles mode: 2d6 dice sum, heads/tails coin, or "
      "magic 8-ball answer. SPACE rolls." },
    { 'm', "Morse", "Type text, sends in morse", nullptr, feat_tool_morse,
      "Type a string and it blinks the screen cyan + beeps the speaker in "
      "Morse code. Dot = 100ms, dash = 300ms." },
    { 'r', "MAC rand", "Randomize WiFi MAC", nullptr, feat_tool_mac_rand,
      "Generates a random locally-administered MAC and applies it to the WiFi "
      "station interface. Resets on reboot." },
    { 'c', "Calc", "Simple calculator", nullptr, feat_tool_calc,
      "Left-to-right +-*/ evaluator on a typed expression. No operator "
      "precedence — parenthesize mentally. Supports decimals." },
    { 't', "Screen test", "RGB cycle + gradient", nullptr, feat_tool_screen_test,
      "Cycles through solid R/G/B/W/K/cyan/amber plus a gradient bar. "
      "Any key advances, ESC exits." },
    { 'f', "SD format", "WIPE microSD card", nullptr, feat_tool_sd_format,
      "Deep-deletes every file and directory on the SD card. Requires typing "
      "YES to confirm. Cannot be undone." },
    { 0, nullptr, nullptr, nullptr, nullptr, nullptr },
};

static const menu_node_t MENU_SYS[] = {
    { 'f', "Files", "SD card browser", nullptr, feat_file_browser,
      "Simple SD card tree view. ENTER opens a directory, D deletes the "
      "selected file, ` goes up one level." },
    { 'c', "Clock", "Uptime / GPS clock", nullptr, feat_clock,
      "Big uptime display. Will show GPS time when the LoRa-GNSS HAT is "
      "attached and has a fix." },
    { 's', "Settings", "Config + preferences", nullptr, feat_settings,
      "Saved WiFi management, clear creds log, format prefs, reboot." },
    { 'a', "About", "Build info", nullptr, feat_about,
      "Version, tagline, repo URL." },
    { 0, nullptr, nullptr, nullptr, nullptr, nullptr },
};

const menu_node_t MENU_ROOT_CHILDREN[] = {
    { 'w', "WiFi", "WiFi recon + attacks + wardrive", MENU_WIFI, nullptr,
      "2.4 GHz WiFi recon and attacks. Everything from passive scanning to "
      "handshake capture, evil portal, deauth, beacon spam, client hunting, "
      "spectrum analyzer, and GPS wardriving." },
    { 'b', "Bluetooth", "BLE scan / spam / HID / tracker", MENU_BLE, nullptr,
      "Bluetooth Low Energy toolkit. Scan + identify, GATT explorer, HID "
      "keyboard spoofing, notification spam, tracker detection/hunting, "
      "Sour Apple, Find My emulator, clone, flood." },
    { 'i', "IR", "Infrared blaster + remote", MENU_IR, nullptr,
      "Drives the Cardputer's IR LED. TV-B-Gone cycles power-off codes, "
      "virtual Samsung remote for live TV control." },
    { 't', "Triton", "gotchi pet that hunts handshakes", nullptr, feat_triton,
      "Autonomous handshake-hunter companion. Channel-hops, sniffs EAPOLs, "
      "deauths seen APs to force reconnects, captures PMKIDs and full 4-ways. "
      "Has a personality + mood face. Includes a simple RL layer that learns "
      "which channels produce the most captures in your environment." },
    { 'u', "BadUSB", "USB-HID payload runner", nullptr, feat_badusb,
      "Emulates a USB HID keyboard when plugged into a computer. Runs "
      "DuckyScript-lite payloads from the built-in library (Hello, Notepad, "
      "Rickroll, Lock, Terminal) or type freeform to send as keystrokes." },
    { 'n', "Network", "Port scan / ping / DNS / connect", MENU_NET, nullptr,
      "LAN tools that require joining a WiFi network first. TCP port scanner, "
      "live ping, DNS lookup." },
    { 'o', "Tools", "Flashlight / stopwatch / dice / ...", MENU_TOOLS, nullptr,
      "Miscellaneous utilities in the Flipper tradition. Flashlight, "
      "stopwatch, calculator, dice/coin/8-ball, morse sender, MAC "
      "randomizer, screen test, SD format." },
    { 'm', "Mesh", "PigSync ESP-NOW peer mesh", MENU_MESH, nullptr,
      "ESP-NOW presence beacon. Broadcasts our name/heap/GPS to other "
      "POSEIDON or compatible devices in range. Foundation for the "
      "multi-device C2 concept." },
    { '5', "C5 nodes", "Remote 5GHz + Zigbee via C5 mesh", MENU_C5, nullptr,
      "Control external ESP32-C5 drop-nodes over ESP-NOW. C5 is the only "
      "ESP chip with 5 GHz WiFi + 802.15.4 radios. When your C5 node boots "
      "nearby it auto-connects (green dot in status bar). Commands stream "
      "results back: dual-band scan, Zigbee/Thread sniff, remote deauth." },
    { 's', "System", "Files, clock, settings", MENU_SYS, nullptr,
      "Device utilities: SD browser, clock, settings (WiFi creds, prefs, "
      "reboot), about." },
    { 0, nullptr, nullptr, nullptr, nullptr, nullptr },
};

const menu_node_t MENU_ROOT = {
    '/', "POSEIDON", "press letter to enter",
    MENU_ROOT_CHILDREN, nullptr,
    "POSEIDON — keyboard-first pentesting firmware for M5Stack Cardputer."
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

    /* Title with count + scroll indicator. */
    d.setTextColor(COL_ACCENT, COL_BG);
    d.setCursor(4, BODY_Y + 2);
    d.printf("%s", parent->label);
    int tw = d.textWidth(parent->label);
    d.drawFastHLine(4, BODY_Y + 12, tw + 6, COL_ACCENT);

    int n = count_children(parent);

    /* Windowed list: first row at BODY_Y + 18, 13px per row, 7 rows fit. */
    const int rows       = 7;
    const int row_h      = 13;
    const int first_y    = BODY_Y + 18;
    int first = cursor - rows / 2;
    if (first < 0) first = 0;
    if (first + rows > n) first = max(0, n - rows);

    /* Position indicator in the title bar (e.g., "3/13"). */
    if (n > rows) {
        char pos[12];
        snprintf(pos, sizeof(pos), "%d/%d", cursor + 1, n);
        int pw = d.textWidth(pos);
        d.setTextColor(COL_DIM, COL_BG);
        d.setCursor(SCR_W - pw - 4, BODY_Y + 2);
        d.print(pos);
    }

    for (int r = 0; r < rows && first + r < n; ++r) {
        int i = first + r;
        const menu_node_t *c = &parent->children[i];
        int y = first_y + r * row_h;
        bool sel = (i == cursor);
        uint16_t sel_bg = 0x3007;  /* deep cyan-purple */
        if (sel) {
            d.fillRoundRect(2, y - 1, SCR_W - 4, 12, 2, sel_bg);
            d.drawRoundRect(2, y - 1, SCR_W - 4, 12, 2, 0xF81F);
            d.drawRoundRect(3, y,     SCR_W - 6, 10, 2, 0x07FF);
        }
        uint16_t line_bg = sel ? sel_bg : COL_BG;
        d.setTextColor(sel ? 0xF81F : COL_ACCENT, line_bg);
        d.setCursor(6, y + 1);
        d.printf("[%c]", toupper(c->hotkey));
        d.setTextColor(sel ? 0xFFFF : COL_FG, line_bg);
        d.setCursor(30, y + 1);
        d.print(c->label);
        d.setTextColor(sel ? 0xF81F : COL_DIM, line_bg);
        d.setCursor(SCR_W - 12, y + 1);
        d.print(c->action ? "." : ">");
    }

    /* Scroll arrows on the right edge when there's more above/below. */
    if (first > 0) {
        d.fillTriangle(SCR_W - 7, first_y - 3,
                       SCR_W - 3, first_y - 3,
                       SCR_W - 5, first_y - 6, 0xF81F);
    }
    if (first + rows < n) {
        int ay = first_y + rows * row_h - 2;
        d.fillTriangle(SCR_W - 7, ay,
                       SCR_W - 3, ay,
                       SCR_W - 5, ay + 3, 0xF81F);
    }

    /* Hint strip for the selected item, below the visible rows. */
    if (cursor >= 0 && cursor < n) {
        const menu_node_t *sel = &parent->children[cursor];
        if (sel->hint) {
            int visible = (n < rows) ? n : rows;
            int y = first_y + visible * row_h + 2;
            if (y < FOOTER_Y - 10) {
                d.setTextColor(COL_DIM, COL_BG);
                d.setCursor(4, y);
                d.print("» ");
                d.print(sel->hint);
            }
        }
    }
}

/* Show detailed info for the selected item until any key pressed. */
static void show_info(const menu_node_t *item)
{
    auto &d = M5Cardputer.Display;
    ui_clear_body();
    d.setTextColor(0xF81F, COL_BG);
    d.setCursor(4, BODY_Y + 2);
    d.printf("[%c] %s", toupper(item->hotkey), item->label);
    d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, 0xF81F);

    d.setTextColor(COL_ACCENT, COL_BG);
    d.setCursor(4, BODY_Y + 18);
    d.printf("> %s", item->hint ? item->hint : "");

    /* Word-wrapped info paragraph, ~38 chars per line at 6px font. */
    if (item->info) {
        d.setTextColor(COL_FG, COL_BG);
        const char *p = item->info;
        int y = BODY_Y + 34;
        while (*p && y < FOOTER_Y - 8) {
            /* Find a wrap point within 38 chars. */
            int take = 0, last_space = -1;
            while (p[take] && take < 38) {
                if (p[take] == ' ') last_space = take;
                take++;
            }
            if (p[take] && last_space > 0) take = last_space;
            char line[40];
            strncpy(line, p, take);
            line[take] = '\0';
            d.setCursor(4, y);
            d.print(line);
            y += 10;
            p += take;
            if (*p == ' ') p++;
        }
    } else {
        d.setTextColor(COL_DIM, COL_BG);
        d.setCursor(4, BODY_Y + 34);
        d.print("(no detailed info)");
    }

    ui_draw_footer("any key = back");
    while (true) {
        uint16_t k = input_poll();
        if (k != PK_NONE) return;
        delay(40);
    }
}

/* Slide-transition trampoline. ui_slide_transition takes a void(void)
 * painter; we stash parent+cursor here so draw_menu has its args. */
static const menu_node_t *s_slide_parent;
static int                s_slide_cursor;
static void slide_paint(void) { draw_menu(s_slide_parent, s_slide_cursor); }
static void slide_to(const menu_node_t *p, int c, int dir) {
    s_slide_parent = p;
    s_slide_cursor = c;
    ui_slide_transition(slide_paint, dir);
}

#define FOOTER_HINTS "letter=go  ;/.=move  ENTER=sel  ==info  `=back"

static void run_submenu(const menu_node_t *parent)
{
    int cursor = 0;
    int n = count_children(parent);

    ui_draw_status(radio_name(), "");
    ui_draw_footer(FOOTER_HINTS);
    draw_menu(parent, cursor);

    while (true) {
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(10); continue; }

        if (k == PK_ESC) return;
        if (k == '=' || k == '?') {
            show_info(&parent->children[cursor]);
            draw_menu(parent, cursor);
            ui_draw_footer(FOOTER_HINTS);
            continue;
        }
        if (k == PK_ENTER) {
            const menu_node_t *sel = &parent->children[cursor];
            if (sel->action) {
                sel->action();
                ui_draw_status(radio_name(), "");
                ui_draw_footer(FOOTER_HINTS);
                draw_menu(parent, cursor);
            } else if (sel->children) {
                /* Slide into the child submenu. */
                slide_to(sel, 0, +1);
                run_submenu(sel);
                /* Slide back to parent after child returns. */
                ui_draw_status(radio_name(), "");
                ui_draw_footer(FOOTER_HINTS);
                slide_to(parent, cursor, -1);
            }
            continue;
        }

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
                    if (ch->action) {
                        ch->action();
                        ui_draw_status(radio_name(), "");
                        ui_draw_footer(FOOTER_HINTS);
                        draw_menu(parent, cursor);
                    } else if (ch->children) {
                        slide_to(ch, 0, +1);
                        run_submenu(ch);
                        ui_draw_status(radio_name(), "");
                        ui_draw_footer(FOOTER_HINTS);
                        slide_to(parent, cursor, -1);
                    }
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
