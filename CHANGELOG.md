# Changelog

All notable changes to POSEIDON are documented here.
Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
versioning follows [SemVer](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Fixed — WiFi deauth correctness (2026-04-17)

Testers reported POSEIDON's deauth was noticeably weaker than Flipper Marauder,
Ghost ESP, and aircrack-ng. Audit of the deauth pipeline found the primary path
(`feat_wifi_deauth`) was firing frames self-addressed to the AP's own BSSID —
no client ever saw a frame addressed to it or broadcast. Plus several
secondary issues that added up to real-world ineffectiveness.

**Core fix:**
- `wifi_deauth.cpp` — `addr1` (destination) now correctly set to broadcast
  `FF:FF:FF:FF:FF:FF` for the sweep phase, and to specific STA MACs for unicast
  rounds. Was previously hardcoded to the target BSSID, which is a no-op.

**Attack pipeline hardening:**
- Every deauth frame is now paired with a disassoc frame (subtype 0xA0,
  reason 8). Mirrors aircrack-ng `--deauth`, Marauder, and Ghost ESP patterns.
  Some client drivers ignore one but honor the other.
- Sequence Control field now increments per frame (starts at a random 12-bit
  seed). Previously static zero, which caused modern client drivers and AP
  firmware to rate-limit or drop our frames as apparent duplicates.
- `esp_wifi_80211_tx` return value now checked at every call site. A `drops:`
  counter surfaces driver-level rejects to the UI so you can see when the
  blob is filtering frames vs actually sending them.
- Targeted mode (`feat_wifi_deauth`) now runs a promiscuous sniffer in parallel
  with the attack to harvest connected STA MACs, then alternates broadcast
  bursts + unicast bursts to each learned client. Matches aircrack-ng's
  64-frame alternating pattern.
- Channel-hopper in `wifi_clients_all` no longer races unicast/broadcast
  bursts. The hotkey deauth handlers lock the hopper for the duration of the
  burst, restore after.

**New safety:**
- PMF / 802.11w / WPA3 warning screen before firing targeted deauth against
  WPA3-PSK, WPA2/WPA3 transition, WPA2-Enterprise, or WPA3-ENT-192 targets.
  These use Protected Management Frames which cryptographically drop plain
  deauth — previously the UI showed "flooding 40fps" with zero actual kicks.

**New shared code:**
- `src/features/wifi_deauth_frame.h` — single source of truth for frame
  construction. Deauth + disassoc pair builder, correct 802.11-2016 Sequence
  Control encoding, PMF detection helper. All four deauth sites now use it.

**Docs:**
- `docs/deauth-injection-patch.md` — explains why stock ESP-IDF's WiFi blob
  filters some spoofed-addr2 frames at the TX FIFO, and documents the
  platform-fork approach for full parity with Marauder/Ghost ESP on-air
  effectiveness. Not required for the above fixes to land; the blob patch
  is a multiplier, not a replacement.

**Triton integration:**
- Triton's background hunt task now routes all broadcast deauths through the
  shared `wifi_deauth_broadcast` helper — so TM_HUNT, TM_STORM, and TM_SURGICAL
  modes inherit the disassoc pair + seq increment. Previously Triton was
  firing deauth-only frames with static seq=0, same way the interactive
  features used to. Triton's effectiveness against stubborn clients should
  jump commensurately.

**Testers:** rebuild + reflash. No config changes required. You should see
the `drops:` counter in the targeted deauth UI and an `sta:N` counter showing
learned clients. Feedback welcome — specifically whether previously stubborn
targets (modern iPhones / Androids, OpenWrt APs) now kick.

## [0.1.0] - 2026-04-14

First tagged release. Everything up to this point.

### Added

- **Core shell:** hierarchical letter-mnemonic menu, slide transitions
  between submenus, scrollable lists with cursor-centered windowing,
  per-item info panels via the `=` key, lazy radio-domain switcher
  (WiFi ↔ BLE ↔ off) to survive ESP32-S3 RAM constraints.
- **Visuals:** Hokusai Great Wave splash with magenta scanline materialization
  + title glow-in, per-feature dashboard chrome (hex storm backdrop,
  magenta title bar, cyan frequency bars, corner radar sweep, smooth
  border-fade strobe on events), full-screen action overlays for
  capture moments (matrix rain / radar / waves / glitch blocks).
- **WiFi arsenal:** scan with open-only filter, per-AP client list + global
  client hunter (vendor + DHCP hostname), targeted deauth + broadcast
  deauth (now auto-scans + nukes every AP without selection), deauth
  detector, AP clone, 4-template Evil Portal, beacon spam, probe request
  sniff, karma, PMKID + full 4-way EAPOL handshake capture, 2.4 GHz
  spectrum analyzer, WiGLE 1.6 CSV wardriving, saved-WiFi connect.
- **BLE arsenal:** classifying scan (OUI + Continuity subtype + Fast Pair
  IDs + SIG UUIDs via `ble_db`), spam (Apple / Samsung / Google / Windows),
  HID Bad-KB with disguise picker, AirTag / Find-My tracker detect +
  Geiger-counter locator, GATT explorer, connection flood with
  kill-scan-first fix, karma, Sour Apple (CVE-2023-42941), Find-My
  broadcaster (1/8/32 flock), Salty Deep Lovense/WeVibe controller,
  connectable GATT clone, iBeacon, passive sniffer CSV.
- **Network tools:** port scan, ping, DNS, LLMNR+NBT-NS+mDNS Responder
  (+ SMB:445 stub), UPnP/SSDP scanner, RaspyJack-style LAN Recon
  (ARP sweep → portscan → banner grab → OUI → CSV).
- **Triton:** autonomous handshake gotchi with ε-greedy softmax RL
  channel picker, mood-driven ASCII face, hashcat 22000 output, learned
  brain persisted to SD. Now uses C5 in parallel: rotates through 5 GHz
  deauths every 6s to cascade dual-band clients back to 2.4.
- **C5 companion:** ESP-IDF v5.5.4 firmware for the ESP32-C5 with dual-band
  WiFi scan (2.4 + 5 GHz, country=US for full UNII coverage), 802.15.4
  Zigbee sniffer with active beacon-request injection + live channel hop
  indicator, 5 GHz deauth (first pocket tool to do this), WS2812
  NeoPixel status LED with mode-driven palette.
- **Other:** IR TV-B-Gone + generic IR remote, ESP-NOW mesh, DuckyScript-lite
  BadUSB runner, file browser, clock, flashlight/stopwatch/dice/morse/
  MAC-rand/calc/screen-test tools, GPS NMEA parser, per-MAC hostname
  cache via DHCP Option 12 parsing.
- Firmware version string (`v0.1.0`) on splash, About screen, and serial boot.
- GitHub Actions build CI with firmware artifact upload on tag push.

### Changed

- `Deauth all` no longer prompts for AP selection. Scans first, then
  rotates through every AP found blasting broadcast deauths. Deeper
  bursts per AP (32 frames × 6ms) so the attack actually lands before
  rotation.
- Dashboard border strobe is now smooth 500 ms magenta fade instead of
  hard 60 ms on/off, rate-limited to min 900 ms between flashes.
- WiFi scan + clients lists remember cursor across menu exits.

### Fixed

- C5 status reboot: all C5 features now enter via `radio_switch(RADIO_WIFI)`
  so BLE tears down cleanly before WiFi mode flips. Raw `WiFi.mode(WIFI_STA)`
  was racing with in-flight NimBLE callbacks.
- C5 peer / result arrays were mutated by the ESP-NOW recv callback
  while the UI printed names → half-written entries lacked null terminators
  → printf walked past buffer → TWDT reset. Now all access is under a
  `portMUX` critical section and the UI uses a safe `c5_peer_name_copy()`.
- BLE init on ESP32-C6 boards no longer calls
  `esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT)` — that was
  corrupting NimBLE init.
- NimBLE UUID extraction was reading chars 32-35 of the canonical form
  (`"34fb"`); fixed to chars 4-7 (`"XXXX"`).
- BLE flood stopping any lingering scan + canceling in-flight connects.
- Probe/deauth frame MAC byte order now matches NimBLE LE.
- Dropped dead OPI/QSPI PSRAM init (fails on this Cardputer unit);
  codebase no longer assumes PSRAM exists.
- SD card mount: all features now go through `sd_helper::sd_mount()`
  which uses the Cardputer's actual SPI pinout (40/39/14/12). `SD.begin()`
  with defaults was silently failing everywhere.

### Security notes

POSEIDON is a pentesting tool. It is authorized for use on networks and
devices you own or have written permission to test. See README "Legal"
section.

[Unreleased]: https://github.com/GeneralDussDuss/poseidon/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/GeneralDussDuss/poseidon/releases/tag/v0.1.0
