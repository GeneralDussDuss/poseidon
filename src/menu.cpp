/*
 * menu.cpp — hierarchical menu tree + runtime.
 *
 * The tree is declared below with letter mnemonics per level. Feature
 * implementations live in features/ — they expose a single entry point
 * that the menu invokes.
 */
#include "menu.h"
#include "ui.h"
#include "ui_ambient.h"
#include "input.h"
#include "radio.h"
#include "theme.h"
#include "c5_cmd.h"
#include "menu_carousel.h"
#include "heap_budget.h"
#include "screensaver.h"
#include <Preferences.h>

/* ---- menu render style: NVS-backed terminal/carousel toggle ---- */
static menu_style_t s_style        = MENU_STYLE_TERMINAL;
static bool         s_style_loaded = false;

menu_style_t menu_style_get(void)
{
    if (!s_style_loaded) {
        Preferences p;
        if (p.begin("pui", true)) {
            uint8_t v = p.getUChar("mnustyle", (uint8_t)MENU_STYLE_TERMINAL);
            p.end();
            if (v >= MENU_STYLE__COUNT) v = MENU_STYLE_TERMINAL;
            s_style = (menu_style_t)v;
        }
        s_style_loaded = true;
    }
    return s_style;
}

void menu_style_set(menu_style_t s)
{
    if (s >= MENU_STYLE__COUNT) s = MENU_STYLE_TERMINAL;
    s_style        = s;
    s_style_loaded = true;
    Preferences p;
    if (p.begin("pui", false)) {
        p.putUChar("mnustyle", (uint8_t)s);
        p.end();
    }
}

/* ---- forward decls for feature entry points ---- */
extern void feat_wifi_scan(void);
extern void feat_wifi_deauth(void);
extern void feat_wifi_deauth_broadcast(void);
extern void feat_wifi_deauth_detect(void);
extern void feat_wifi_clients(void);
extern void feat_wifi_clients_all(void);
extern void feat_wifi_portal(void);
extern void feat_wifi_apclone(void);
extern void feat_evil_twin(void);
extern void feat_wifi_beacon_spam(void);
extern void feat_wifi_wardrive(void);
extern void feat_wifi_probe(void);
extern void feat_wifi_karma(void);
extern void feat_wifi_pmkid(void);
extern void feat_wifi_spectrum(void);
extern void feat_wifi_connect(void);
extern void feat_ap_signal_test(void);
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
extern void feat_ble_blueducky(void);
extern void feat_ir_tvbgone(void);
extern void feat_ir_remote(void);
extern void feat_ir_prank_power_bomb(void);
extern void feat_ir_prank_channel_scramble(void);
extern void feat_ir_prank_volume_bomb(void);
extern void feat_ir_prank_source_roulette(void);
extern void feat_ir_prank_mute_torture(void);
extern void feat_ir_prank_permanent_power(void);
extern void feat_ir_prank_cable_is_out(void);
extern void feat_ir_prank_caption_chaos(void);
extern void feat_ir_prank_sleep_timer(void);
extern void feat_ir_prank_ac_chaos(void);
extern void feat_mesh(void);
extern void feat_triton(void);
extern void feat_c5_status(void);
extern void feat_c5_scan_5g(void);
extern void feat_c5_scan_zb(void);
extern void feat_c5_deauth_5g(void);
extern void feat_c5_pmkid_5g(void);
extern void feat_c5_nuke_5g(void);
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
extern void feat_heap_census(void);
extern void feat_about(void);
extern void feat_badusb(void);
extern void feat_badusb_win(void);
extern void feat_badusb_mac(void);
extern void feat_badusb_linux(void);
extern void feat_badusb_android(void);
extern void feat_badusb_chrome(void);
extern void feat_badusb_pranks(void);
extern void feat_kerberos(void);
extern void feat_net_portscan(void);
extern void feat_net_ping(void);
extern void feat_net_dns(void);
extern void feat_net_responder(void);
extern void feat_net_ssdp(void);
extern void feat_net_lanrecon(void);
extern void feat_cctv_scan(void);
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
extern void feat_surveillance_hunter(void);
extern void feat_satcom(void);
extern void feat_defensive_monitor(void);
extern void feat_usb_guard(void);
extern void feat_ir_clone(void);
extern void feat_drone_remoteid(void);
extern void feat_nrf52_scan(void);
extern void feat_nrf52_sniff(void);
extern void feat_nrf52_longrange(void);
extern void feat_nrf52_zigbee(void);
extern void feat_nrf52_mitm(void);
extern void feat_nrf52_ble_mitm_relay(void);
extern void feat_nrf52_scout_strike(void);
extern void feat_nrf52_wifi_ble_combo(void);
extern void feat_mimir(void);
extern void feat_theme_picker(void);
extern void feat_sfx_settings(void);
extern void feat_ambient_preview(void);
extern void feat_menu_style_toggle(void);
extern void feat_screensaver_toggle(void);
extern void feat_screensaver_picker(void);
/* extern void feat_ota_update(void); — REMOVED 2026-05-27 */
/* feat_phone_bridge disabled — see platformio.ini comment + phone_bridge.cpp.disabled */

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
extern void feat_mass_storage(void);
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
    { 'p', "Portal", "Evil captive portal (16 templates)", nullptr, feat_wifi_portal,
      "Captive portal with DNS hijack. 16 templates: Google, Facebook, "
      "Microsoft, Free WiFi, Apple ID, Office 365, LinkedIn, Amazon, Netflix, "
      "Instagram, Hotel, Starbucks, Airport, Router Admin, Zoom, Company SSO. "
      "Logs creds to /poseidon/creds.log on SD. NOTE: brings up the AP via "
      "raw IDF (Bruce libs crash WiFi.softAP), which releases the BTDM heap "
      "one-way -- BLE features will be dead until reboot." },
    { 'i', "Evil Twin", "Clone AP + portal + auto-deauth chain", nullptr, feat_evil_twin,
      "One-button chained attack against the last-scanned AP. Time-slices "
      "the radio: 5s DEAUTH burst (raw STA mode, broadcast deauth at the "
      "target BSSID on its channel) then 25s PORTAL (raw AP mode beaconing "
      "the target SSID + DNS hijack + Free WiFi captive page) repeating "
      "until ESC. Kicked clients re-associate to the rogue, hit the portal, "
      "leak creds to /poseidon/creds.log. Concurrent APSTA is fragile on "
      "Bruce libs -- time-slicing is the only stable path. PMF/WPA3 targets "
      "won't be kicked (deauth dropped) but the portal still works for any "
      "client that joins. 5G targets unsupported (S3 is 2.4 GHz only). "
      "Releases BTDM heap one-way -- BLE features dead until reboot." },
    { 't', "AP Signal Test", "Diagnostic: is the AP actually broadcasting?", nullptr, feat_ap_signal_test,
      "Permanent diagnostic for the SoftAP path. Brings up SSID "
      "'POSEIDON-SIGTEST' (open, no password) on channel 1/6/11 (;/. "
      "cycles) using the raw-IDF recipe (Bruce libs crash WiFi.softAP). "
      "Live readout: BSSID, channel, uptime, connected STAs, TX power. "
      "If the SSID does NOT appear when you scan from your phone or run "
      "`netsh wlan show networks mode=bssid` from Windows, the AP path "
      "itself is broken -- not the higher-level feature. ESC tears down "
      "cleanly. NOTE: like Portal, this releases the BTDM heap one-way; "
      "BLE features dead until reboot." },
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
    { 'y', "DefMon", "Defensive WiFi+BLE anomaly monitor", nullptr, feat_defensive_monitor,
      "Passive multi-class detector: deauth flood (rate-debounced), broadcast-zero "
      "deauth (Spacehuhn/deauther.cc signature), evil twin (dup SSID/diff BSSID), "
      "beacon spam, WiFi Karma (probe-resp w/o beacon), BLE spoof (dup name/diff "
      "MAC), BLE flood. Time-slices WiFi promisc + NimBLE scan. Alerts → "
      "/poseidon/defmon-<ts>.jsonl with GPS coords. Audio cue on each new class." },
    { 'u', "Cable Guard", "Detect malicious USB cable/charger RF implants", nullptr, feat_usb_guard,
      "Two-phase RF delta scan. Baseline 2.4 GHz APs with the suspect cable "
      "UNPLUGGED, then plug it in and re-scan: any radio that switched on with "
      "the cable is flagged + scored against implant signatures (Espressif/ESP "
      "OUIs, O.MG/WiFiDuck/EvilCrow SSIDs, hidden-AP-on-plug, spoofed MAC, open "
      "auth). Verdict CLEAN/SUSPICIOUS/DANGEROUS. NOTE: catches active implants "
      "& DIY ESP injectors; stealth/dormant cables can still hide — no hit is "
      "NOT a safety guarantee." },
    { 'v', "Surveil", "Flock/Raven surveillance detector", nullptr, feat_surveillance_hunter,
      "Passive 2.4 GHz channel-hop scan that fingerprints Flock Safety "
      "ALPR cameras and ShotSpotter Raven gunshot sensors by OUI + SSID "
      "patterns + wildcard-probe behavior. GPS-tagged hits stream to "
      "/poseidon/surv-<ts>.csv (WiGLE format) and .jsonl (Plume-compatible). "
      "5s dedup per BSSID. Source signatures: colonelpanichacks/flock-you, "
      "zmattmanz/Plume, DeFlockJoplin research." },
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
    { 'p', "PMKID 5G", "5 GHz PMKID capture via C5", nullptr, feat_c5_pmkid_5g,
      "The ESP32-S3 side can't receive on 5 GHz — which means it physically "
      "cannot capture handshakes from 5 GHz networks. C5 closes that gap. Pick "
      "a 5 GHz target, C5 locks its promisc receiver to the channel + BSSID and "
      "watches for EAPOL-Key M1 frames carrying a PMKID KDE. Hits stream back "
      "over ESP-NOW and write directly to /poseidon/hashcat.22000 in WPA*01* "
      "format for offline cracking. Pair with Deauth 5G to force clients to "
      "reconnect and cough up a fresh M1." },
    { 'n', "Nuke 5G + HS", "Deauth-all + handshake capture, 5 GHz", nullptr, feat_c5_nuke_5g,
      "The 5 GHz equivalent of the WiFi -> Deauth all feature. Scans 5 GHz, "
      "then rotates every 6 s through every AP found: C5 fires a 4-second "
      "broadcast deauth AND starts a 5-second HS capture listening on the "
      "same channel. Clients that get kicked re-auth right into the capture "
      "window. Both handshakes (WPA*02*) and PMKIDs (WPA*01*) that land go "
      "straight to /poseidon/hashcat.22000. Runs until ESC. Glitch splash "
      "aesthetic — live target + hit counts on-screen." },
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
    { 'd', "Drone RID", "Detect FAA Remote ID broadcasts", nullptr, feat_drone_remoteid,
      "Passive ASTM F3411-22a Drone Remote ID listener (BT Service UUID "
      "0xFFFA). Decodes UAS ID + drone lat/lon/alt + operator location for "
      "any Part-89-compliant drone in range. Live target list + JSONL log "
      "with GPS-tagged observations. Source: GhostBLE (SmonSE)." },
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
    { 'a', "SourApple", "Multi-vendor BLE spam: iOS/Android/Win/Pixel", nullptr, feat_ble_sourapple,
      "Rotating BLE advertisement spam across seven packet types: Apple "
      "AirPods/Beats ProximityPair popup (iOS 18), Apple Nearby Action "
      "modal (iOS 17/18), Apple AirTag detected notification, Samsung "
      "Galaxy Buds EasySetup, Samsung Galaxy Watch EasySetup, Microsoft "
      "Swift Pair (Win 10/11), Google Fast Pair (Pixel Buds / Sony / Bose / "
      "JBL — pops 'Pair NAME?' on EVERY Android with Play Services). "
      "Mix randomizes per packet so every nearby phone/laptop gets hit "
      "from multiple angles. Live per-mode counters on screen. Original "
      "iOS 17.0-17.1 crash variant patched in 17.2 — current build pops "
      "modals instead. Credits: ECTO-1A, RapierXbox, ckcr4lyf, Spooks4576, "
      "simondankelmann, Flipper ble-spam." },
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
    { 'q', "BlueDucky", "BLE HID inject (CVE-2023-45866, Android)", nullptr, feat_ble_blueducky,
      "BLE HID keystroke injection against unpatched Android (CVE-2023-45866, "
      "fixed in Dec 2023 security patch). Advertises as a BLE keyboard with "
      "a phone-friendly disguise; on a vulnerable target, the BLE HID stack "
      "accepts input reports WITHOUT confirming the pair dialog. Pick a "
      "DuckyScript payload from BADUSB / ANDROID (or the prank slice), wait "
      "for the target to attach (~30s window), and the script executes into "
      "whatever app has focus. Typing capped at ~45 keys/sec so Android's "
      "input queue doesn't drop keys. Screen must be ON and Bluetooth ON on "
      "the target; range is normal BLE (~5-10m). Credit: BlueDucky "
      "(pentestfunctions), Marc Newlin / SkySafe (CVE disclosure). "
      "Personal-use only." },
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
    { 'v', "CCTV Toolkit", "IP camera recon: ports/brand/rtsp/creds", nullptr, feat_cctv_scan,
      "Credit: @7h30th3r0n3's Evil-M5Project. Scans targets for open camera "
      "ports (80 / 554 / 8080-83 / 8443 / 8554), HTTP-fingerprints the "
      "brand (Hikvision, Dahua, Axis, Vivotek, Panasonic, CPPlus), sprays "
      "a 10-entry default-cred list on 401 responses (admin/admin, "
      "admin/12345, admin/888888, root/root, etc.), and walks RTSP "
      "OPTIONS + DESCRIBE against common vendor stream paths "
      "(/Streaming/Channels/1, /cam/realmonitor, /live, etc.). "
      "Three modes: LAN /24 sweep, single IP, or read targets from "
      "/poseidon/cctv-targets.txt. Hits go to /poseidon/cctv-<ts>.csv "
      "with ports mask / brand / cred / stream URL." },
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

extern void feat_ir_test(void);

/* IR Pranks — 10 one-shot drivers from ir_extras_data.h. Each fires
 * for 3-30 sec depending on the prank; UI blocks until done or ESC. */
static const menu_node_t MENU_IR_PRANKS[] = {
    { 'b', "Power Bomb", "All 27 brands' power codes in a burst",
      nullptr, feat_ir_prank_power_bomb,
      "Fires every brand's power-off code back-to-back (~6-8 sec). Combined "
      "with TV-B-Gone covers ~95% of TVs / projectors / soundbars in a "
      "typical room. Most displays in line of sight blink off within 5 sec." },
    { 'c', "Ch Scramble", "Spam Ch+ on Samsung+LG",
      nullptr, feat_ir_prank_channel_scramble,
      "Hammers Ch+ rapidly for ~5 sec, alternating Samsung and LG. ~50 "
      "channel changes before the user can navigate back. On QAM-only cable "
      "boxes you'll hit dozens of channels." },
    { 'v', "Volume Bomb", "Vol+ x30 on Samsung+LG",
      nullptr, feat_ir_prank_volume_bomb,
      "Slams Samsung and LG Vol+ thirty times in under 3 sec. TVs cap at "
      "~100; receivers and soundbars go to max. Pairs nicely with a loud "
      "commercial. Use responsibly." },
    { 'r', "Source Roulette", "Cycle TV inputs 12x",
      nullptr, feat_ir_prank_source_roulette,
      "Presses Source 12 times with variable delays mimicking a confused "
      "human. Cycles HDMI1/2/3, USB, AV, ATV, DTV. User sees 'No Signal' "
      "+ black screens, takes 30+ sec to get back to HDMI1." },
    { 'm', "Mute Torture", "Toggle mute every 800ms for 30s",
      nullptr, feat_ir_prank_mute_torture,
      "Toggles mute every 800ms for 30 sec on Samsung + LG. Audio "
      "stutters on a frustratingly predictable cadence. User usually "
      "gives up after 5-10 mutes." },
    { 'p', "Permanent Off", "Power-bomb every 3s for 5 min",
      nullptr, feat_ir_prank_permanent_power,
      "Fires the full power-bomb (all 27 brands) every 3 sec for 5 min. "
      "Whoever's TV is in the room cannot stay on for more than ~3s. "
      "Classic 'the cable is out' simulation." },
    { 'o', "Cable Flakes", "3 input-loss cycles, 45s apart",
      nullptr, feat_ir_prank_cable_is_out,
      "Three input-loss simulations spaced ~45 sec apart. Switches AWAY "
      "from the cable box twice, then cycles back through 5 inputs. Looks "
      "like the cable box keeps losing signal every minute." },
    { 'a', "Caption Chaos", "Toggle CC every 600ms",
      nullptr, feat_ir_prank_caption_chaos,
      "Toggles closed-captioning rapidly (Samsung 0x39, LG 0x39). CC text "
      "appears/disappears every 600ms. On some Samsung models language "
      "swaps between English/Spanish/Off mid-prank." },
    { 's', "Sleep 180", "Set Samsung sleep timer to 180 min",
      nullptr, feat_ir_prank_sleep_timer,
      "Cycles Samsung sleep-timer 5x rapidly to land on 180-min auto-off. "
      "TV powers off 3h later with no apparent trigger. Repeat for several "
      "nights before the target figures it out." },
    { 'k', "AC Chaos", "Daikin/Mitsubishi/LG AC power toggle",
      nullptr, feat_ir_prank_ac_chaos,
      "Captured-state replays for Daikin / Mitsubishi / LG AC units. "
      "~50% hit rate per brand (depends on the AC's stored state matching "
      "the capture); usually one lands. Office HVAC roulette." },
    { 0, nullptr, nullptr, nullptr, nullptr, nullptr },
};

static const menu_node_t MENU_IR[] = {
    { 't', "TV-B-Gone", "Kill nearby TVs", nullptr, feat_ir_tvbgone,
      "Cycles power-off IR codes for Sony, Samsung, LG, Panasonic, Philips, "
      "Vizio. Point the top edge of the Cardputer at the TV. Runs until ESC." },
    { 'r', "Remote", "Virtual Samsung remote", nullptr, feat_ir_remote,
      "Virtual Samsung TV remote. P=power, M=mute, +/-=volume, ;/.=channel, "
      "1-9=digits, I=source, H=home, B=back." },
    { 'c', "Clone", "Multi-profile IR remote (Samsung/LG/Sony)", nullptr, feat_ir_clone,
      "Pre-installed Samsung TV / LG TV / Sony TV remote profiles. Pick a "
      "profile, hit the labeled keys to fire IR codes via the TX LED on GPIO 44. "
      "v2 will add capture-from-real-remote + Flipper-compatible .ir files." },
    { 'p', "Pranks", "10 one-shot IR pranks (power/vol/mute/cable/AC)",
      MENU_IR_PRANKS, nullptr,
      "Curated one-shot IR pranks layered on top of TV-B-Gone. Power-bomb "
      "extends coverage to 27 brands + projectors + soundbars; channel/mute/ "
      "source/sleep/caption pranks target Samsung+LG live TVs; AC chaos "
      "replays Daikin/Mitsu/LG climate-control state." },
    { 'x', "LED Test", "Verify IR LED pin + polarity", nullptr, feat_ir_test,
      "Hardware diagnostic — drives candidate pins / polarities for 4 sec each. "
      "Point a phone camera at the IR LED and report which step lights it up "
      "purple/white. Tells us if GPIO 44 is right, if the LED is active-LOW, "
      "or if it's on a different pin entirely." },
    { 0, nullptr, nullptr, nullptr, nullptr, nullptr },
};

/* BadUSB sub-arsenal. Original 5 built-in payloads stay behind
 * "Built-in"; the per-OS submenus pull from badusb_extras.h (106
 * UberGuidoZ-derived); the Pranks entry opens the vibe-organized
 * highlight reel (badusb_pranks_data.h, 59 entries × 7 categories). */
static const menu_node_t MENU_BADUSB[] = {
    { 'b', "Built-in", "5 starter payloads (Hello/Notepad/Rickroll/Lock/Terminal)",
      nullptr, feat_badusb,
      "The original POSEIDON BadUSB shortlist. Hello (types a greeting), "
      "Notepad (opens Notepad and types a pwned message), Rickroll (opens "
      "the YouTube link via Win+R), Lock (Win+L instant lockscreen), "
      "Terminal (Ctrl+Alt+T then writes a file). Pick a number 1-5 to fire, "
      "or T to type freeform. Best for quick demos." },
    { 'p', "Pranks", "Vibe-organized chaos: Gaslight / Chaos / Lockers / Doom / Hack / Loud / Classic",
      nullptr, feat_badusb_pranks,
      "The showpiece. 59 prank payloads across 7 vibe categories — "
      "GASLIGHT (subtle slow-burn like auto-correct sabotage + cursor jiggle), "
      "VISIBLE CHAOS (notepad spam, explorer bomb), LOCKERS (instant lock + "
      "delayed-fuse), BROWSER DOOM (rickrolls, tab floods, hampter), "
      "FAKE HACK (BSOD drops, matrix rain, ransom sim), LOUD (TTS + siren), "
      "CLASSIC (the all-time greats). Each prank tagged with target OS — "
      "pick one that matches the target's machine." },
    { 'w', "Windows", "44 payloads — info-gathering, opens, vandalism",
      nullptr, feat_badusb_win,
      "44 Windows-specific payloads from the UberGuidoZ library. Run-dialog "
      "openers, WiFi-profile dumpers (local file only — no exfil), browser "
      "opens, theme/wallpaper changes, system-info printouts to Desktop. "
      "All payloads are self-contained: no URLs to attacker-hosted servers, "
      "no edit-the-script placeholders, no credential exfil." },
    { 'm', "macOS", "24 payloads — Spotlight openers, Terminal scripts",
      nullptr, feat_badusb_mac,
      "24 macOS payloads — Spotlight Cmd-Space openers, Terminal scripts that "
      "run local-only commands, AppleScript snippets, system-info to "
      "~/Desktop. Same filtering as Windows: self-contained, no exfil, no "
      "edit-required placeholders." },
    { 'l', "Linux", "19 payloads — terminal openers, info commands",
      nullptr, feat_badusb_linux,
      "19 Linux payloads — Ctrl+Alt+T terminal openers, harmless info-print "
      "commands (uname / ip a / dmesg | tail), local-only file ops, and a "
      "few package-manager-curious one-liners. Assumes a desktop distro "
      "with a graphical terminal." },
    { 'a', "Android", "11 payloads — App search openers, Settings shortcuts",
      nullptr, feat_badusb_android,
      "11 Android payloads — requires a USB-OTG cable AND target has USB "
      "debugging or HID-class accepted. App-drawer search openers, "
      "Settings deep-links, screenshot triggers. Works with stock Android "
      "11+ when the phone accepts the Cardputer as a HID keyboard." },
    { 'c', "ChromeOS", "8 payloads — Chrome shortcuts + crosh openers",
      nullptr, feat_badusb_chrome,
      "8 ChromeOS payloads — Chrome browser shortcuts, crosh terminal "
      "opener (Ctrl+Alt+T), Settings deep-links, screenshot triggers. "
      "Useful for school-issued Chromebooks where you can plug in a "
      "USB-C HID device." },
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
    { 't', "SATCOM", "Satellite tracker (TLE + SGP4)", nullptr, feat_satcom,
      "Live satellite tracking with TLE auto-fetch from Celestrak. Pick "
      "ISS / Tiangong / NOAA / ham birds from a favorites list, see live "
      "polar skyplot + az/el/lat/lon/alt, predict next 24h passes >10°. "
      "Observer from GPS, time from NTP or GPS. Source patterned after "
      "adammelancon/cardputer-satellite-tracker." },
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
    { 'h', "Heap", "Live internal-heap census", nullptr, feat_heap_census,
      "Internal-heap free, largest contiguous block, and lifetime low-water "
      "mark. R reclaims recoverable caches (frees the ARGUS sprite etc.). "
      "The same numbers the portal and rf_preflight log to serial." },
    { 't', "Theme", "POSEIDON / MATRIX / E-INK", nullptr, feat_theme_picker,
      "Three curated palettes: POSEIDON (cyberpunk cyan/magenta/purple, "
      "default), MATRIX (souped-up hacker green-on-black with cinematic "
      "rain), E-INK (paper white for daylight / minimal mode). Live "
      "preview, ENTER to commit." },
    { 'b', "Ambient", "Live ambient motion preview", nullptr, feat_ambient_preview,
      "Live preview of the active theme's ambient: POSEIDON paints TRON "
      "grid + cyan/magenta motes + magenta L-path packet, MATRIX runs "
      "souped-up phosphor rain, E-INK is intentionally clean. [A] toggles "
      "the NVS-backed enable flag globally if you ever want it off." },
    { 'l', "Layout", "Terminal / Carousel toggle", nullptr, feat_menu_style_toggle,
      "Flip the menu render style. TERMINAL is the dense 7-row list with "
      "letter mnemonics — fast for power-users. CAROUSEL is the big-card "
      "single-focus layout with corner brackets, pulsing hotkey badge, "
      "size-2 label, slide animation between siblings. Letter mnemonics "
      "still work in Carousel mode. Persists to NVS." },
    { 'v', "Screensaver", "Toggle 2-min idle takeover", nullptr, feat_screensaver_toggle,
      "Flip the screensaver on/off. After 2 minutes of no input, takes "
      "over the screen with a painter from the pool. Any key wakes it." },
    { 'p', "Saver Pick", "Choose specific screensaver / shuffle", nullptr, feat_screensaver_picker,
      "Pool of 10 theme-tinting screensavers: WARDRIVE cinema, MATRIX rain, "
      "BREATHING wordmark, DEEP SCAN sonar, PORT SCAN visualizer, HEX CASCADE "
      "with decoded reveals, TERMINAL CRACK fake hashcat, NEURAL ARC pulsing "
      "mesh, GLITCH BSOD chromatic flashes, TIDE WAVES drift. SHUFFLE = random "
      "rotation excluding the last-shown one. P key in the picker previews the "
      "currently-highlighted painter; ENTER commits to NVS." },
    { 'n', "Sound", "Speaker volume + mute + SFX test", nullptr, feat_sfx_settings,
      "Adjust SFX volume 0-10, toggle global mute. Every menu click, "
      "deauth, handshake capture, and splash boot sequence has a tone. "
      "Settings persist to NVS." },
    /* Phone Bridge menu entry disabled — feature pulled out for heap-
     * budget reasons (broke Triton). Restore alongside the lib_deps in
     * platformio.ini when re-enabling. */
    { 'a', "About", "Build info", nullptr, feat_about,
      "Version, tagline, repo URL." },
    /* OTA Update removed 2026-05-27 — flow was buggy. */
    { 0, nullptr, nullptr, nullptr, nullptr, nullptr },
};

/* SaltyJack owns its own RaspyJack-style renderer — see
 * src/features/saltyjack/saltyjack_menu.cpp. The root 'j' entry below
 * dispatches directly to feat_saltyjack_root() instead of using a
 * POSEIDON menu_node_t tree. */

static const menu_node_t MENU_NRF52[] = {
    { 's', "Scan", "BLE 5 full scan (all PHYs)", nullptr, feat_nrf52_scan,
      "Adafruit Bluefruit Feather nRF52840 over UART1 (G3 TX / G4 RX). "
      "Active scan on 1M + 2M + Coded PHYs. Lists MAC, RSSI, name, PHY. "
      "Captures BLE 5 LR beacons the ESP32-S3 NimBLE stack can't see." },
    { 'l', "Long Range", "BLE 5 Coded PHY S=8 (4x range)", nullptr, feat_nrf52_longrange,
      "Coded PHY S=8 scan — picks up advertisers up to ~4x typical BLE "
      "range. Useful for finding distant LR-mode trackers, industrial "
      "sensors, and LE Audio devices broadcasting on Coded." },
    { 'n', "Sniffer", "All BLE adv + decode + pcap", nullptr, feat_nrf52_sniff,
      "Passive sniff every BLE advertisement on every channel. Streams "
      "decoded packets to the screen and dumps a pcap to SD for offline "
      "Wireshark." },
    { 'z', "Zigbee", "802.15.4 sniffer (Zigbee/Thread)", nullptr, feat_nrf52_zigbee,
      "IEEE 802.15.4 sniffer. Picks up Zigbee, Thread, OpenThread, "
      "Matter-over-Thread mesh traffic. Alternative to C5 if you don't "
      "have the C5 node nearby." },
    { 'm', "MITM", "BLE 5 connection follow + hijack", nullptr, feat_nrf52_mitm,
      "Selectively follow a BLE connection, jam the slave on its hop "
      "channel and hijack the link. BTLEJack-style, but with full BLE 5 "
      "PHY support. Requires nRF52 FW v2." },
    { 'r', "MITM Relay", "Active BLE GATT proxy", nullptr, feat_nrf52_ble_mitm_relay,
      "Connect outbound to a real BLE peripheral while advertising a "
      "clone of it to the victim. Relays every GATT op between them, "
      "logs to SD." },
    { 'k', "Scout+Strike", "Recon → pick → kill workflow", nullptr, feat_nrf52_scout_strike,
      "Two-phase attack pipeline: scout (passive scan + classify), "
      "operator picks target, strike (jam / disconnect / deauth via "
      "ESP32 fallback)." },
    { 'c', "WiFi+BLE Combo", "Deauth → BLE pairing capture", nullptr, feat_nrf52_wifi_ble_combo,
      "ESP32-S3 deauths a target smart-home device off WiFi; nRF52 is "
      "already listening to capture the BLE provisioning handshake when "
      "the device falls back to BLE setup mode. Unique to POSEIDON." },
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
    { 't', "Argus", "gotchi pet that hunts handshakes", nullptr, feat_triton,
      "Autonomous handshake-hunter companion. Channel-hops, sniffs EAPOLs, "
      "deauths seen APs to force reconnects, captures PMKIDs and full 4-ways. "
      "When a C5/TRIDENT satellite is online it also hunts 5 GHz handshakes "
      "via the satellite. Has a personality + Argus mood face, plus a simple "
      "RL layer that learns which channels produce the most captures." },
    { 'f', "Feather", "nRF52840 BLE5 / Zigbee / MITM", MENU_NRF52, nullptr,
      "Adafruit Bluefruit Feather nRF52840 via UART hat (G3 TX / G4 RX). "
      "Unlocks BLE 5 raw PHY sniffing (incl. Coded PHY long-range), "
      "802.15.4 / Zigbee / Thread sniff+inject, and BLE connection MITM — "
      "capabilities the ESP32-S3 NimBLE stack can't reach. Mutually "
      "exclusive with LoRa / Hydra hats." },
    { 'u', "BadUSB", "USB-HID payload runner + 165+ payloads", MENU_BADUSB, nullptr,
      "USB HID keyboard emulator. Plug into a target computer and it appears "
      "as a real keyboard — types DuckyScript-lite payloads from the built-in "
      "library, the OS-organized UberGuidoZ subset, or the curated PRANK reel. "
      "Pick the OS the target is running for highest-success scripts; pick "
      "Pranks for the showpiece vibe-organized chaos library." },
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
    { 'd', "Mass Storage", "microSD -> USB drive (card reader)", nullptr, feat_mass_storage,
      "Exposes the microSD to a USB host as a removable drive over USB-C — "
      "the Cardputer becomes a card reader. Plug into a PC and the card shows "
      "up as a USB stick for drag-and-drop file transfer. While active the "
      "host owns the card and POSEIDON's own SD logging pauses; ESC ejects "
      "cleanly and hands the card back (no reboot). Serial console stays up." },
    { 'k', "KERBEROS", "FIDO2 security key (U2F)", nullptr, feat_kerberos,
      "Turns the Cardputer into a USB security key. Enter this mode, plug "
      "into a PC, and register it at a site's security key settings. The "
      "screen shows the sign in prompt and ENTER approves. Phase 1 speaks "
      "U2F, so it works as a second factor on Google, GitHub, and any site "
      "that accepts a security key." },
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

/* Lightweight idle-path companion to draw_menu. Repaints ONLY the gutter
 * strips that don't overlap row text — the strip between the magenta
 * underline and the first row, and the strip(s) below the last visible
 * row. ui_ambient_tick is called against the FULL body bounds inside a
 * setClipRect window so motes / grid / packet positions stay coherent
 * across frames instead of looping in a strip-sized box. Scroll arrows
 * are re-stamped because their bounding boxes overlap the strips. */
static void draw_menu_anim(const menu_node_t *parent, int cursor)
{
    if (!ui_ambient_enabled()) return;
    auto &d = M5Cardputer.Display;
    int n = count_children(parent);
    if (n <= 0) return;

    const int rows    = 7;
    const int row_h   = 13;
    const int first_y = BODY_Y + 18;
    const int visible = (n < rows) ? n : rows;
    const int last_row_bottom = first_y + visible * row_h;

    bool hint_drawn = false;
    int  hint_y     = last_row_bottom + 2;
    if (cursor >= 0 && cursor < n) {
        const menu_node_t *sel = &parent->children[cursor];
        if (sel->hint && hint_y < FOOTER_Y - 10) hint_drawn = true;
    }
    const int hint_bottom = hint_y + 8;

    auto paint_strip = [&](int y, int h) {
        if (h <= 0) return;
        d.fillRect(0, y, SCR_W, h, T_BG);
        d.setClipRect(0, y, SCR_W, h);
        ui_ambient_tick(0, BODY_Y, SCR_W, BODY_H);
    };

    /* Strip 1: between magenta underline (BODY_Y+13) and first row. */
    paint_strip(BODY_Y + 14, first_y - (BODY_Y + 14));
    /* Strip 2: below last visible row to either hint top or footer. */
    int s2_end = hint_drawn ? hint_y : (FOOTER_Y - 1);
    paint_strip(last_row_bottom, s2_end - last_row_bottom);
    /* Strip 3: below hint to footer. */
    if (hint_drawn) paint_strip(hint_bottom, (FOOTER_Y - 1) - hint_bottom);

    d.clearClipRect();

    /* Re-stamp scroll arrows — their triangles overlap the strip zones. */
    int first = cursor - rows / 2;
    if (first < 0) first = 0;
    if (first + rows > n) first = max(0, n - rows);
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
}

static const menu_node_t *s_menu_prev_parent = nullptr;
static int                s_menu_prev_cursor  = -1;
static int                s_menu_prev_first   = -1;
static bool               s_menu_force        = true;

static void draw_menu(const menu_node_t *parent, int cursor)
{
    auto &d = M5Cardputer.Display;

    int n = count_children(parent);

    const int rows       = 7;
    const int row_h      = 13;
    const int first_y    = BODY_Y + 18;
    int first = cursor - rows / 2;
    if (first < 0) first = 0;
    if (first + rows > n) first = max(0, n - rows);

    if (!s_menu_force && parent == s_menu_prev_parent &&
        cursor == s_menu_prev_cursor && first == s_menu_prev_first) return;
    s_menu_force       = false;
    s_menu_prev_parent = parent;
    s_menu_prev_cursor = cursor;
    s_menu_prev_first  = first;

    ui_force_clear_body();
    /* Paint theme-aware ambient motion BEFORE menu chrome — rows draw
     * over the top with their own opaque background so they remain
     * readable. No-op when the user has disabled ambient via
     * System -> Ambient. */
    ui_ambient_tick(0, BODY_Y, SCR_W, BODY_H);

    /* Title with count + scroll indicator. Title underline is a strategic
     * magenta splash — full body width, 2 px thick — so the cyberpunk
     * accent color reads from across the room as the dominant pop. */
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2);
    d.printf("%s", parent->label);
    (void)d.textWidth(parent->label);   /* kept for API parity with prior version */
    d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT2);
    d.drawFastHLine(4, BODY_Y + 13, SCR_W - 8, T_ACCENT2);

    /* Root-menu-only C5/TRIDENT status panel. Larger + more prominent
     * than the global status-bar badge so the user sees satellite
     * pairing state the instant POSEIDON boots. */
    if (parent == &MENU_ROOT) {
        int n5 = c5_any_online() ? c5_peer_count() : 0;
        int px = SCR_W - 72, py = BODY_Y + 1;
        d.drawRoundRect(px, py, 68, 11, 2, n5 > 0 ? T_GOOD : T_DIM);
        d.fillCircle(px + 5, py + 5, 2, n5 > 0 ? T_GOOD : 0x528A);
        d.setTextColor(n5 > 0 ? T_GOOD : T_DIM, T_BG);
        d.setCursor(px + 12, py + 2);
        if (n5 > 0) d.printf("C5 x%d ONLINE", n5);
        else        d.print("C5 not paired");
    }

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
    s_menu_force = true;
    ui_slide_transition(slide_paint, dir);
}

#define FOOTER_HINTS "letter=go  ;/.=move  ENTER=sel  ==info  `=back"

static void run_submenu(const menu_node_t *parent)
{
    /* Style dispatch — when the user has selected the carousel layout
     * we hand off to the standalone carousel renderer + input loop.
     * The keyboard semantics are identical (letter mnemonics, ENTER,
     * ESC) so calling code doesn't care which renderer is active. */
    if (menu_style_get() == MENU_STYLE_CAROUSEL) {
        carousel_run_submenu(parent);
        return;
    }

    int cursor = 0;
    int n = count_children(parent);

    ui_draw_status(radio_name(), "");
    ui_draw_footer(FOOTER_HINTS);
    draw_menu(parent, cursor);

    while (true) {
        uint16_t k = input_poll();
        if (k == PK_NONE) {
            /* Screensaver takeover when idle threshold passes. Returns
             * true if it ran — repaint the menu so the user sees their
             * cursor position again. */
            if (screensaver_check_idle()) {
                ui_status_invalidate();
                ui_draw_status(radio_name(), "");
                ui_draw_footer(FOOTER_HINTS);
                s_menu_force = true;
                draw_menu(parent, cursor);
            }
            /* Idle ambient tick — only repaints gutter strips between rows
             * so text never strobes. ~33 ms cadence matches carousel. */
            static uint32_t last_anim = 0;
            uint32_t now = millis();
            if (now - last_anim > 33) {
                last_anim = now;
                draw_menu_anim(parent, cursor);
            }
            delay(10);
            continue;
        }

        Serial.printf("[MK] k=0x%04X p=\"%s\" c=%d\n", k,
                      parent && parent->label ? parent->label : "?", cursor);
        if (k == PK_ESC) return;
        if (k == '=' || k == '?') {
            show_info(&parent->children[cursor]);
            s_menu_force = true;
            draw_menu(parent, cursor);
            ui_draw_footer(FOOTER_HINTS);
            continue;
        }
        if (k == PK_ENTER) {
            const menu_node_t *sel = &parent->children[cursor];
            if (sel->action) {
                /* Baseline is captured BEFORE reclaim so a cache freed here and
                 * lazily reallocated by the feature (e.g. the ARGUS sprite) nets
                 * ~0 in the exit delta instead of a phantom LEAK. Reclaim still
                 * runs so the feature starts from a maximally free heap.
                 * Note: the first use of a lazy-once feature (wardrive, cctv,
                 * ciw) legitimately allocates permanent state and will show a
                 * one-time negative delta -- an expected non-leak. */
                size_t hb_base = heap_free_internal();
                size_t hb_reclaimed = heap_reclaim_all();
                Serial.printf("[FEAT_ENTER] %s free=%u reclaimed=%u\n",
                              sel->label, (unsigned)heap_free_internal(),
                              (unsigned)hb_reclaimed);
                g_current_feature_item = sel;
                sel->action();
                g_current_feature_item = nullptr;
                /* Defensive IR park — IR features should self-park HIGH
                 * but if any path skips that, the LED stays glowing.
                 * Hard-set OFF here after every feature returns. */
                pinMode(44, OUTPUT); digitalWrite(44, HIGH);
                { size_t hb_now = heap_free_internal();
                  long hb_d = (long)hb_now - (long)hb_base;
                  Serial.printf("[FEAT_EXIT] %s free=%u delta=%ld%s\n",
                      sel->label, (unsigned)hb_now, hb_d, hb_d < -2048 ? " LEAK" : ""); }
                ui_draw_status(radio_name(), "");
                ui_draw_footer(FOOTER_HINTS);
                s_menu_force = true;
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
                        size_t hb_base = heap_free_internal();
                        size_t hb_reclaimed = heap_reclaim_all();
                        Serial.printf("[FEAT_ENTER] %s free=%u reclaimed=%u\n",
                                      ch->label, (unsigned)heap_free_internal(),
                                      (unsigned)hb_reclaimed);
                        g_current_feature_item = ch;
                        ch->action();
                        g_current_feature_item = nullptr;
                        /* Defensive IR park — same as above. */
                        pinMode(44, OUTPUT); digitalWrite(44, HIGH);
                        { size_t hb_now = heap_free_internal();
                          long hb_d = (long)hb_now - (long)hb_base;
                          Serial.printf("[FEAT_EXIT] %s free=%u delta=%ld%s\n",
                              ch->label, (unsigned)hb_now, hb_d, hb_d < -2048 ? " LEAK" : ""); }
                        ui_draw_status(radio_name(), "");
                        ui_draw_footer(FOOTER_HINTS);
                        s_menu_force = true;
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
