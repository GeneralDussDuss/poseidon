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
#include "theme.h"

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
extern void feat_ble_whisperpair(void);
extern void feat_ir_tvbgone(void);
extern void feat_ir_remote(void);
extern void feat_mesh(void);
extern void feat_triton(void);
extern void feat_c5_status(void);
extern void feat_c5_scan_5g(void);
extern void feat_c5_scan_zb(void);
extern void feat_c5_deauth_5g(void);
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
extern void feat_lora_scan(void);
extern void feat_lora_beacon(void);
extern void feat_lora_meshtastic(void);
extern void feat_lora_spectrum(void);
extern void feat_mesh_chat(void);
extern void feat_mesh_nodes(void);
extern void feat_mesh_page(void);
extern void feat_mesh_position(void);
extern void feat_gps_fix(void);
extern void feat_subghz_scan(void);
extern void feat_subghz_record(void);
extern void feat_subghz_replay(void);
extern void feat_subghz_spectrum(void);
extern void feat_subghz_bruteforce(void);
extern void feat_subghz_jammer(void);
extern void feat_subghz_broadcast(void);
extern void feat_subghz_jam_detect(void);
extern void feat_nrf24_sniffer(void);
extern void feat_nrf24_mousejack(void);
extern void feat_nrf24_ble_spam(void);
extern void feat_nrf24_scanner(void);
extern void feat_nrf24_jammer(void);
extern void feat_nrf24_finder(void);
extern void feat_subghz_finder(void);
extern void feat_mimir(void);
extern void feat_theme_picker(void);
extern void feat_sfx_settings(void);

/* SaltyJack — LAN attack suite, homage to @7h30th3r0n3's Evil-M5Project.
 * See src/features/saltyjack/ for credits + implementation. */
extern void feat_saltyjack_root(void);
extern void feat_saltyjack_info(void);
extern void feat_saltyjack_dhcp_starve(void);
extern void feat_saltyjack_dhcp_rogue_sta(void);
extern void feat_saltyjack_dhcp_rogue_ap(void);
extern void feat_saltyjack_responder(void);
extern void feat_saltyjack_wpad(void);
extern void feat_saltyjack_ntlm_crack(void);
extern void feat_trident(void);
extern void feat_uart_shell(void);
extern void feat_tcp_tunnel(void);
extern void feat_honeypot(void);
extern void feat_dead_drop(void);
extern void feat_printer(void);
extern void feat_ssdp_poison(void);
extern void feat_dhcp_starve(void);
extern void feat_dhcp_rogue_sta(void);
extern void feat_dhcp_rogue_ap(void);
extern void feat_net_hijack(void);
extern void feat_wpad_abuse(void);
extern void feat_autodiscover(void);
extern void feat_wifi_ciw(void);

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
    { 'z', "CIW Zeroclick", "SSID injection payload broadcast", nullptr, feat_wifi_ciw,
      "Broadcasts beacon frames with SSID payloads targeting WiFi driver "
      "vulnerabilities: command injection, buffer overflow, format strings, "
      "Log4Shell JNDI, XSS, CRLF, path traversal, heap spray, ANSI escape, "
      "template injection, NoSQL injection. 14 categories, ~120 payloads. "
      "Rotates SSID on configurable interval. +/- adjust speed." },
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
    { 'd', "Deauth 5G", "5 GHz deauth attack via C5", nullptr, feat_c5_deauth_5g,
      "ESP32-S3 cannot transmit on 5 GHz — but the C5 can. Pick a 5 GHz AP from "
      "the dual-band scan, or hit X to broadcast-deauth every AP on its channel. "
      "C5 streams live frame counts back so you see attack rate in real time. "
      "First pocket tool that can deauth 5 GHz networks." },
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
    { 'w', "WhisperPair", "CVE-2025-36911 Fast Pair probe", nullptr, feat_ble_whisperpair,
      "Scans for Google Fast Pair accessories (AirPods rivals from Sony, "
      "JBL, Jabra, Pixel Buds, Nothing, OnePlus). Classifies each as "
      "pairable or in-use, then writes a bogus Key-Based Pairing blob to "
      "the FE2C service. Response = VULNERABLE. Silent drop = PATCHED. "
      "Verdicts logged to /poseidon/whisperpair.csv. Probe only — the "
      "ESP32-S3 has no BR/EDR radio so we never complete the bond. "
      "Credit: COSIC KU Leuven." },
    { 0, nullptr, nullptr, nullptr, nullptr, nullptr },
};

static const menu_node_t MENU_NET_ATTACKS[] = {
    { 'u', "UART Shell", "Serial bridge to UART1", nullptr, feat_uart_shell,
      "Bridges the Cardputer keyboard to UART1 (GPIO1/2 or Grove G1/G2 on ADV). "
      "Auto-detect baud or manual selection. Scrolling terminal display." },
    { 't', "TCP Tunnel", "Reverse connect-back TCP client", nullptr, feat_tcp_tunnel,
      "Connects a WiFiClient to user-specified host:port and relays keyboard "
      "input as commands. Displays response. Simple reverse-shell relay." },
    { 'h', "Honeypot", "Fake telnet server on :23", nullptr, feat_honeypot,
      "Starts a WiFiServer on port 23 with a fake Ubuntu login banner. Logs "
      "all usernames, passwords, and commands to /poseidon/honeypot.log on SD." },
    { 'd', "Dead Drop", "Hidden AP for anonymous file drops", nullptr, feat_dead_drop,
      "Creates a hidden AP with captive portal. Connected devices get a web page "
      "for uploading files and posting anonymous notes. Stored on SD." },
    { 'p', "Printer", "Scan + print to network printers", nullptr, feat_printer,
      "Scans the /24 subnet for hosts with port 9100 (JetDirect) open. "
      "Pick a printer, sends /poseidon/print.txt from SD." },
    { 's', "SSDP Poison", "Flood LAN with fake UPnP devices", nullptr, feat_ssdp_poison,
      "Broadcasts fake SSDP NOTIFY ssdp:alive packets and responds to M-SEARCH "
      "queries with spoofed device descriptions. Pollutes device lists." },
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
    { 'x', "Attacks", "UART / TCP / honeypot / printer / SSDP", MENU_NET_ATTACKS, nullptr,
      "Offensive network features: UART shell bridge, reverse TCP tunnel, "
      "telnet honeypot, WiFi dead drop, printer detection + print, "
      "and SSDP poisoner." },
    { 'h', "DHCP Starve", "Exhaust DHCP pool via random MACs", nullptr, feat_dhcp_starve,
      "Floods DHCP DISCOVER from spoofed random MACs using known OUIs. Tracks "
      "OFFER/ACK/NAK counts and pool exhaustion percentage. NAK threshold "
      "indicates successful starvation. Requires STA connection." },
    { 'g', "Rogue DHCP STA", "Hijack gateway+DNS via STA", nullptr, feat_dhcp_rogue_sta,
      "After starvation, answers DISCOVER/REQUEST with attacker-controlled "
      "gateway and DNS pointing to our IP. Injects WPAD URL for proxy "
      "autoconfig. STA mode — requires existing WiFi connection." },
    { 'j', "Rogue DHCP AP", "Hijack gateway+DNS via AP", nullptr, feat_dhcp_rogue_ap,
      "Same as Rogue DHCP STA but creates a SoftAP and stops the built-in "
      "ESP DHCP server, then serves our own. Hands out attacker-controlled "
      "gateway and DNS to anyone who connects." },
    { 'k', "Net Hijack", "Starve+Rogue+Portal auto chain", nullptr, feat_net_hijack,
      "One-button full MitM chain: DHCP starvation to exhaust the legitimate "
      "pool, then rogue DHCP to hand out attacker gateway/DNS, then launches "
      "the captive portal for credential capture." },
    { 'w', "WPAD Abuse", "Proxy autoconfig NTLM capture", nullptr, feat_wpad_abuse,
      "Creates a SoftAP with DNS wildcard, serves wpad.dat pointing all "
      "traffic through our proxy, then challenges with NTLM 407 to capture "
      "NTLMv2 hashes. Saved to /poseidon/ntlm_hashes.txt for hashcat 5600." },
    { 'e', "Autodiscover", "Exchange cred capture (Basic+NTLM)", nullptr, feat_autodiscover,
      "Fake Exchange Autodiscover endpoint. Offers Basic Auth first (plaintext "
      "capture on older Outlook) with NTLM fallback (NTLMv2 hash). Returns "
      "valid XML so Outlook thinks it succeeded. Creds saved to SD." },
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

/* HUNT bundle — unified menu for every hot/cold RSSI locator + tracker
 * detector. Previously scattered across BLE, SubGHz, and nRF24 menus;
 * bundled here so an operator on a job site can just pick "what kind of
 * emitter am I chasing?" without remembering which radio category it
 * lives under. Each entry delegates to the existing per-radio finder. */
static const menu_node_t MENU_HUNT[] = {
    { 'b', "BLE tracker", "Geiger-style BLE tracker hunt", nullptr, feat_ble_finder,
      "Pick a tracker from the list, then HUNT mode turns the screen into a "
      "big proximity meter. Beep rate speeds up as you get closer, like a "
      "metal detector. Ideal for locating a surveillance tracker on you." },
    { 't', "Tracker scan", "Detect AirTag/SmartTag/Tile", nullptr, feat_ble_tracker,
      "Passive scan for Apple Find My, Samsung SmartTag, and Tile. Shows "
      "distance class + signal bar + MAC. Feeds the BLE tracker hunt." },
    { 'g', "Sub-GHz finder", "CC1101 hot/cold locator", nullptr, feat_subghz_finder,
      "Walk around — big thermometer bar goes from blue (cold/far) to red "
      "(hot/close). Beep rate increases near the source. Find sub-GHz key "
      "fobs, garage remotes, hidden transmitters." },
    { 'n', "2.4 GHz finder", "nRF24 hot/cold locator", nullptr, feat_nrf24_finder,
      "Same proximity UI on 2.4 GHz. Hunt wireless HID devices, rogue "
      "dongles, ISM-band sources." },
    { 0, nullptr, nullptr, nullptr, nullptr, nullptr },
};

static const menu_node_t MENU_TOOLS[] = {
    { 'h', "Hunt",       "Unified hot/cold proximity hunter", MENU_HUNT, nullptr,
      "Every RF proximity locator in one submenu — BLE tracker hunt, BLE "
      "tracker passive scan, sub-GHz (CC1101) finder, 2.4 GHz (nRF24) "
      "finder. Previously scattered across BLE/SubGHz/nRF24 menus; here "
      "so you can just pick 'what am I hunting?' first." },
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

static const menu_node_t MENU_LORA[] = {
    { 's', "Scan", "Passive RX on selected band", nullptr, feat_lora_scan,
      "Tunes the SX1262 to 433/868/915 MHz or Meshtastic LongFast and "
      "listens for LoRa packets. RSSI + SNR + payload hex logged to SD." },
    { 'b', "Beacon", "TX POSEIDON ping every 3s", nullptr, feat_lora_beacon,
      "Transmits POSEIDON:<uptime>:<lat>,<lon> on the chosen band every "
      "3 seconds. Range-testing with a second LoRa device." },
    { 'm', "Mesh LF", "Meshtastic LongFast US listener", nullptr, feat_lora_meshtastic,
      "Tunes 906.875 MHz with LongFast US params (SF11 BW250 CR4/5). "
      "Parses Meshtastic packet headers: FROM/TO nodeId, packet ID." },
    { 'a', "Analyzer", "LoRa band spectrum + waterfall + scope", nullptr, feat_lora_spectrum,
      "Three visualization modes for LoRa bands: bar spectrum with gradient "
      "RSSI + peak hold + dBm grid, waterfall spectrogram heatmap, and "
      "live oscilloscope waveform. Covers 430-440, 860-870, 900-930 MHz." },
    { 'c', "Mesh Chat", "Meshtastic text chat — send + receive", nullptr, feat_mesh_chat,
      "Live feed of received Meshtastic text messages on the default "
      "LongFast channel, with a text input to broadcast back. POSEIDON "
      "participates as a real mesh node with a MAC-derived node ID." },
    { 'n', "Mesh Nodes", "Live roster of seen Meshtastic nodes", nullptr, feat_mesh_nodes,
      "Scrollable list of all detected mesh nodes with short name, "
      "node ID, SNR/RSSI, hops, last-seen, and GPS pin indicator. "
      "ENTER on a node opens the direct-message page screen." },
    { 'p', "Mesh Page", "Send a direct message to a specific node", nullptr, feat_mesh_page,
      "Type a hex node ID (!xxxxxxxx) and a text message, send as a "
      "unicast Meshtastic packet. Or pick a node from the roster first." },
    { 'g', "Mesh Pos", "Toggle our own position broadcasts", nullptr, feat_mesh_position,
      "When enabled, POSEIDON broadcasts NodeInfo every 30min and Position "
      "every 15min (if GPS has a fix). We'll show up as a pin on other "
      "Meshtastic apps within range." },
    { 0, nullptr, nullptr, nullptr, nullptr, nullptr },
};

static const menu_node_t MENU_SUBGHZ[] = {
    { 'b', "Broadcast", "Categorized .sub file TX library", nullptr, feat_subghz_broadcast,
      "Browse /poseidon/signals/ on SD by category: cars, pranks, Tesla, "
      "home automation, custom. Pick a Flipper .sub file and transmit. "
      "Waveform preview + multi-play." },
    { 's', "Scan/Copy", "Freq scanner + signal decoder", nullptr, feat_subghz_scan,
      "Sweeps sub-GHz frequencies (300-928 MHz), locks the strongest, "
      "decodes with RCSwitch. Shows value, protocol, RSSI. Logs to SD." },
    { 'r', "Record RAW", "RMT pulse capture (up to 20s)", nullptr, feat_subghz_record,
      "Records raw signal pulses from CC1101 GDO0 using the ESP-IDF RMT "
      "peripheral at 1us resolution. Saves as Flipper-compatible .sub file." },
    { 'p', "Play .sub", "Replay any .sub file from SD", nullptr, feat_subghz_replay,
      "Loads .sub files from /poseidon/ on SD and replays the RAW pulse "
      "data through CC1101 TX. Supports Flipper + Bruce formats." },
    { 'a', "Analyzer", "Spectrum + waterfall + oscilloscope", nullptr, feat_subghz_spectrum,
      "Three visualization modes: bar spectrum with color-coded RSSI + "
      "peak hold, waterfall spectrogram heatmap, live waveform scope." },
    { 'f', "Brute force", "Came/Nice/Linear/Chamberlain", nullptr, feat_subghz_bruteforce,
      "Iterates all possible codes for fixed-code protocols: Came 12bit, "
      "Nice 12bit, Chamberlain 9bit, Linear 10bit, Holtek 12bit, Ansonic." },
    { 'j', "Jammer", "Sub-GHz intermittent + full TX", nullptr, feat_subghz_jammer,
      "Intermittent jammer with random-width pulses or continuous carrier. "
      "20-second safety cap. Adjustable frequency." },
    { 'h', "Finder", "Hot/cold signal locator", nullptr, feat_subghz_finder,
      "Walk around with the device — big thermometer bar goes from blue "
      "(cold/far) to red (hot/close). Beep rate increases near the source. "
      "Find hidden transmitters, cameras, key fobs." },
    { 'd', "Jam Detect", "RSSI anomaly monitor", nullptr, feat_subghz_jam_detect,
      "Learns the noise floor on a picked frequency for 10 seconds, then "
      "alerts on sustained spikes above baseline +15 dBm — the signature of "
      "a jammer or sustained TX in the band. Fires a red overlay + siren, "
      "logs timestamp + peak RSSI to /poseidon/jamdetect.csv. Peer to the "
      "WiFi deauth detector. Use it to verify your jammer actually works "
      "or to flag hostile RF interference near a target." },
    { 0, nullptr, nullptr, nullptr, nullptr, nullptr },
};

static const menu_node_t MENU_NRF24[] = {
    { 's', "Sniffer", "Promiscuous ESB device discovery", nullptr, feat_nrf24_sniffer,
      "Travis Goodspeed promiscuous trick — discovers wireless HID devices "
      "(Logitech, Microsoft, Dell). Shows address, channel, device type. "
      "CRC16-validated packet capture with auto-fingerprinting." },
    { 'm', "MouseJack", "HID keystroke injection", nullptr, feat_nrf24_mousejack,
      "Injects keystrokes into discovered wireless keyboards. Supports "
      "Logitech Unifying (0xC1 HID) and Microsoft (XOR checksum). "
      "Type a string and inject as if typed on the target." },
    { 'b', "BLE Spam", "Fake BLE advertisements via nRF24", nullptr, feat_nrf24_ble_spam,
      "Broadcasts spoofed BLE ADV_IND packets with random MACs and fake "
      "device names (AirPods, Galaxy Buds, etc). Full CRC24 + whitening. "
      "Floods all 3 BLE advertising channels." },
    { 'a', "Scanner", "2.4 GHz spectrum + protocol ID", nullptr, feat_nrf24_scanner,
      "126-channel RPD sweep with gradient color bars, peak hold, and "
      "protocol identification markers (WiFi ch1/6/11, BLE adv, Zigbee)." },
    { 'j', "Jammer", "CW carrier + data flood, 7 presets", nullptr, feat_nrf24_jammer,
      "Two jam modes: continuous wave carrier (most effective) or data "
      "flood. Presets: BLE, WiFi ch1/6/11, Zigbee, drone RC, wireless "
      "HID. 20s safety cap." },
    { 'h', "Finder", "Hot/cold 2.4 GHz locator", nullptr, feat_nrf24_finder,
      "Walk around — the meter shows signal strength on a specific channel. "
      "Blue=cold, red=hot. Beep rate increases as you approach the source. "
      "Find hidden cameras, rogue APs, wireless devices." },
    { 0, nullptr, nullptr, nullptr, nullptr, nullptr },
};

static const menu_node_t MENU_RADIO[] = {
    { 'l', "LoRa", "SX1262 sub-GHz long range (CAP hat)", MENU_LORA, nullptr,
      "LoRa features for the M5Stack CAP-LoRa1262 hat. Passive scan, "
      "beacon TX, and Meshtastic LongFast listener." },
    { 's', "Sub-GHz", "CC1101 scan/record/replay/analyze", MENU_SUBGHZ, nullptr,
      "CC1101 sub-GHz radio from the Hydra RF Cap 424. Frequency scanner "
      "with RCSwitch decode, RAW recording, .sub file replay, spectrum "
      "analyzer with waterfall, brute force, and jammer." },
    { 'n', "nRF24", "2.4 GHz spectrum/MouseJack/jam", MENU_NRF24, nullptr,
      "nRF24L01+ 2.4 GHz radio from the Hydra RF Cap 424. ISM band "
      "spectrum analyzer, MouseJack HID sniff/inject, band jammer." },
    { 'g', "GPS fix", "Live GNSS position page", nullptr, feat_gps_fix,
      "Shows current fix from the ATGM336H: lat, lon, alt, sats, HDOP, "
      "speed, UTC. Background NMEA poller runs from boot." },
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
    { 't', "Theme", "Switch color palette", nullptr, feat_theme_picker,
      "Choose from 6 visual themes: POSEIDON (cyan/magenta), PHANTOM "
      "(purple), MATRIX (green), AMBER (retro), E-INK (paper), TRON "
      "(neon cyberpunk). Live preview with color swatches." },
    { 'n', "Sound", "Speaker volume + mute + SFX test", nullptr, feat_sfx_settings,
      "Adjust SFX volume 0-10, toggle global mute. Every menu click, "
      "deauth, handshake capture, and splash boot sequence has a tone. "
      "Settings persist to NVS." },
    { 'a', "About", "Build info", nullptr, feat_about,
      "Version, tagline, repo URL." },
    { 0, nullptr, nullptr, nullptr, nullptr, nullptr },
};

/* SaltyJack owns its own RaspyJack-style renderer — see
 * src/features/saltyjack/saltyjack_menu.cpp. The root 'j' entry below
 * dispatches directly to feat_saltyjack_root() instead of using a
 * POSEIDON menu_node_t tree. */

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
    { 'j', "SaltyJack", "LAN attack suite (homage to 7h30th3r0n3)", nullptr, feat_saltyjack_root,
      "DHCP Starvation, Rogue DHCP, Responder LLMNR/NBNS/SMB NTLMv2, WPAD "
      "harvest, on-device NTLMv2 cracker. Direct port of @7h30th3r0n3's "
      "Evil-M5Project Cardputer firmware — credit prominently displayed in "
      "every file. AUTHORIZED TESTING ONLY." },
    { 'r', "Radio", "LoRa sub-GHz + GNSS (CAP-LoRa1262)", MENU_RADIO, nullptr,
      "Sub-GHz LoRa radio + GPS from the M5Stack CAP-LoRa1262 Cardputer-Adv "
      "hat. Passive scan across 433/868/915, POSEIDON beacon TX, Meshtastic "
      "LongFast listener, and a live GPS fix page. GNSS runs in the background "
      "so Wardrive always has a fresh position." },
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
    { 'x', "MIMIR", "Control MIMIR drop-box via USB-C", nullptr, feat_mimir,
      "Connect to MIMIR pentest drop-box over USB-C cable. Drives scans, "
      "attacks (deauth, handshake, PMKID, evil twin, beacon spam), and "
      "retrieves cracked credentials. Pocket-mode opsec: no wireless link." },
    { 'p', "PC Bridge", "TRIDENT screen mirror + remote KB", nullptr, feat_trident,
      "Streams the Cardputer screen to the TRIDENT desktop app over USB-C. "
      "PC can send keystrokes remotely. 10 fps RGB565 framebuffer capture. "
      "Run 'trident' on your PC first, then enter this mode." },
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
    ui_force_clear_body();
    auto &d = M5Cardputer.Display;

    /* Title with count + scroll indicator. */
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2);
    d.printf("%s", parent->label);
    int tw = d.textWidth(parent->label);
    d.drawFastHLine(4, BODY_Y + 12, tw + 6, T_ACCENT);

    int n = count_children(parent);

    const int rows       = 7;
    const int row_h      = 13;
    const int first_y    = BODY_Y + 18;
    int first = cursor - rows / 2;
    if (first < 0) first = 0;
    if (first + rows > n) first = max(0, n - rows);

    if (n > rows) {
        char pos[12];
        snprintf(pos, sizeof(pos), "%d/%d", cursor + 1, n);
        int pw = d.textWidth(pos);
        d.setTextColor(T_DIM, T_BG);
        d.setCursor(SCR_W - pw - 4, BODY_Y + 2);
        d.print(pos);
    }

    for (int r = 0; r < rows && first + r < n; ++r) {
        int i = first + r;
        const menu_node_t *c = &parent->children[i];
        int y = first_y + r * row_h;
        bool sel = (i == cursor);
        if (sel) {
            d.fillRoundRect(2, y - 1, SCR_W - 4, 12, 2, T_SEL_BG);
            d.drawRoundRect(2, y - 1, SCR_W - 4, 12, 2, T_SEL_BD);
            d.drawRoundRect(3, y,     SCR_W - 6, 10, 2, T_ACCENT);
        }
        uint16_t line_bg = sel ? T_SEL_BG : T_BG;
        d.setTextColor(sel ? T_SEL_BD : T_ACCENT, line_bg);
        d.setCursor(6, y + 1);
        d.printf("[%c]", toupper(c->hotkey));
        d.setTextColor(sel ? T_FG : T_DIM, line_bg);
        d.setCursor(30, y + 1);
        d.print(c->label);
        d.setTextColor(sel ? T_ACCENT2 : T_DIM, line_bg);
        d.setCursor(SCR_W - 12, y + 1);
        d.print(c->action ? "." : ">");
    }

    if (first > 0) {
        d.fillTriangle(SCR_W - 7, first_y - 3,
                       SCR_W - 3, first_y - 3,
                       SCR_W - 5, first_y - 6, T_ACCENT2);
    }
    if (first + rows < n) {
        int ay = first_y + rows * row_h - 2;
        d.fillTriangle(SCR_W - 7, ay,
                       SCR_W - 3, ay,
                       SCR_W - 5, ay + 3, T_ACCENT2);
    }

    /* Hint strip for the selected item, below the visible rows. */
    if (cursor >= 0 && cursor < n) {
        const menu_node_t *sel = &parent->children[cursor];
        if (sel->hint) {
            int visible = (n < rows) ? n : rows;
            int y = first_y + visible * row_h + 2;
            if (y < FOOTER_Y - 10) {
                d.setTextColor(T_DIM, T_BG);
                d.setCursor(4, y);
                d.print("» ");
                d.print(sel->hint);
            }
        }
    }
}

/* Set by run_submenu before invoking a feature's action, so the feature
 * can look up its own long-form help via ui_show_current_help(). */
const menu_node_t *g_current_feature_item = nullptr;

/* Forward-decl so ui_show_current_help can delegate. */
static void show_info(const menu_node_t *item);

void ui_show_current_help(void)
{
    if (!g_current_feature_item) {
        ui_toast("no help available", T_WARN, 800);
        return;
    }
    show_info(g_current_feature_item);
}

/* Show detailed info for the selected item until any key pressed. */
static void show_info(const menu_node_t *item)
{
    auto &d = M5Cardputer.Display;
    ui_force_clear_body();
    d.setTextColor(T_ACCENT2, T_BG);
    d.setCursor(4, BODY_Y + 2);
    d.printf("[%c] %s", toupper(item->hotkey), item->label);
    d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT2);

    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 18);
    d.printf("> %s", item->hint ? item->hint : "");

    /* Word-wrapped info paragraph, ~38 chars per line at 6px font. */
    if (item->info) {
        d.setTextColor(T_FG, T_BG);
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
        d.setTextColor(T_DIM, T_BG);
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
                g_current_feature_item = sel;
                sel->action();
                g_current_feature_item = nullptr;
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
                        g_current_feature_item = ch;
                        ch->action();
                        g_current_feature_item = nullptr;
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
