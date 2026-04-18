<!-- markdownlint-disable MD033 MD041 -->

<div align="center">

```
██████╗  ██████╗ ███████╗███████╗██╗██████╗  ██████╗ ███╗   ██╗
██╔══██╗██╔═══██╗██╔════╝██╔════╝██║██╔══██╗██╔═══██╗████╗  ██║
██████╔╝██║   ██║███████╗█████╗  ██║██║  ██║██║   ██║██╔██╗ ██║
██╔═══╝ ██║   ██║╚════██║██╔══╝  ██║██║  ██║██║   ██║██║╚██╗██║
██║     ╚██████╔╝███████║███████╗██║██████╔╝╚██████╔╝██║ ╚████║
╚═╝      ╚═════╝ ╚══════╝╚══════╝╚═╝╚═════╝  ╚═════╝ ╚═╝  ╚═══╝

              ≋≋≋   commander of the deep   ≋≋≋
```

**Keyboard-first pentesting firmware for the M5Stack Cardputer-Adv**

![target](https://img.shields.io/badge/target-Cardputer--Adv-red?style=flat-square)
![platform](https://img.shields.io/badge/framework-Arduino%2FPlatformIO-blue?style=flat-square)
![license](https://img.shields.io/badge/license-MIT-green?style=flat-square)
![features](https://img.shields.io/badge/features-90+-magenta?style=flat-square)
![release](https://img.shields.io/github/v/release/GeneralDussDuss/poseidon?style=flat-square)
![version](https://img.shields.io/badge/version-0.3.0-cyan?style=flat-square)

[**Download Latest .bin**](https://github.com/GeneralDussDuss/poseidon/releases/latest) — flash with M5Burner or esptool at offset 0x0

**v0.3.0 just shipped** — POSEIDON is now a full Meshtastic leaf node (send, receive, page, position) plus WiFi deauth correctness, LoRa crash fixes, and a rewritten spectrum analyzer with real packet capture. See [CHANGELOG](CHANGELOG.md) and [TESTERS.md](TESTERS.md) for what to stress-test.

**v0.4 incoming (master)** — platform fork lands. Stock ESP-IDF 5.3's WiFi blob filters deauth/disassoc frame subtypes, so every v0.3 deauth mode returned `ESP_ERR_INVALID_ARG` at TX. Master now builds against `pioarduino@55.03.38` (Core 3.3.8 / IDF 5.5.4) + [Bruce](https://github.com/pr3y/Bruce)'s patched `libnet80211.a` with the subtype filter NOP'd. See [`KnownDeauthBugFixSoon.md`](KnownDeauthBugFixSoon.md) and [`docs/v0.4-platform-migration.md`](docs/v0.4-platform-migration.md). v0.4.0 gets tagged once on-hardware verification confirms frames land.

**New — SaltyJack LAN attack suite** ported from [@7h30th3r0n3](https://github.com/7h30th3r0n3)'s [Evil-M5Project](https://github.com/7h30th3r0n3/Evil-M5Project) / [RaspyJack](https://github.com/7h30th3r0n3/Raspyjack). DHCP starvation shipping first; Rogue DHCP, Responder (LLMNR/NBNS/SMB-NTLMv2), WPAD harvest, and on-device NTLMv2 cracker coming in subsequent commits. All credit where credit is due — every file in `src/features/saltyjack/` has a prominent homage header.

</div>

---

## What is this?

POSEIDON is a pentesting firmware for the M5Stack Cardputer-Adv (ESP32-S3). 90+ features across WiFi, BLE, sub-GHz, 2.4 GHz, LoRa, IR, network attacks, and more. In the same family as Flipper Zero, Bruce, Evil-M5Project, and ESP32Marauder — but built around a **real keyboard** with letter mnemonics, typed parameters, and 6 swappable visual themes.

Supports three hardware hats (one at a time):
- **M5Stack CAP-LoRa1262** — LoRa (SX1262) + GNSS (GPS)
- **PINGEQUA Hydra RF Cap 424** — CC1101 sub-GHz + nRF24L01+ 2.4 GHz
- **ESP32-C5 companion node** — 5 GHz WiFi + Zigbee via ESP-NOW mesh

## Quick Start

```bash
# Build from source
git clone https://github.com/GeneralDussDuss/poseidon.git
cd poseidon
pio run -t upload

# Or flash the pre-built binary
esptool.py --chip esp32s3 write_flash 0x0 poseidon-v0.3.0-cardputer-adv.bin
```

## Feature Matrix (90+)

### WiFi (17)
Scan · Clients (all-channel + per-AP) · Deauth · Deauth All · Deauth Detector · AP Clone · Evil Portal (4 templates) · Karma · Beacon Spam · Probe Sniff · PMKID + 4-Way Handshake Capture · 2.4 GHz Spectrum · GPS Wardrive (WiGLE CSV) · Connect · **CIW Zeroclick** (157 SSID payloads: cmd injection, Log4Shell, XSS, buffer overflow)

### Bluetooth (14)
Scan (OUI + Apple Continuity + Fast Pair DB) · Spam (4 brands) · Bad-KB HID · Tracker Detect (AirTag/SmartTag/Tile) · Tracker Finder (Geiger) · Sniffer (CSV) · iBeacon · Clone · GATT Explorer · Flood · Karma · Sour Apple (CVE-2023-42941) · Find My Emulator · The Salty Deep (toy controller)

### Sub-GHz — CC1101 (9)
Scan/Copy (ISR capture + protocol decoder: Princeton, CAME, NICE, Linear) · Record RAW (Flipper .sub format) · Replay .sub Files · **Signal Broadcast Library** (3,190+ .sub files: cars, pranks, Tesla, home) · Spectrum Analyzer (bar + waterfall + oscilloscope) · Brute Force (Came/Nice/Linear/Chamberlain/Holtek/Ansonic) · Jammer · **Hot/Cold Signal Finder**

### 2.4 GHz — nRF24L01+ (6)
**Promiscuous ESB Sniffer** (Travis Goodspeed trick, CRC16-validated) · **MouseJack** (Logitech/Microsoft fingerprint + HID injection) · **BLE Spam** (ADV_IND via nRF24, CRC24 + whitening) · Spectrum Analyzer (WiFi/BLE/Zigbee markers) · **CW Carrier + Data Flood Jammer** (7 presets) · Hot/Cold Finder

### LoRa + GNSS — SX1262 (8)
LoRa Scan (passive RX, multi-band) · Beacon TX · Meshtastic LongFast Listener · LoRa Analyzer (bar meter + waterfall + scope with live packet capture) · **Meshtastic Chat** (send + receive text) · **Meshtastic Nodes** (live roster) · **Meshtastic Page** (direct-message a node) · **Meshtastic Position** (show up as pin on other apps) · **Live GPS Fix** (baud auto-detect, background NMEA)

### Network Attacks (18)
Port Scan · Ping · DNS · Connect · **Responder** (LLMNR/NBT-NS → NTLM) · SSDP/UPnP Scanner · LAN Recon (ARP + port + banner + vendor) · **UART Shell** (serial terminal, auto-baud) · **Reverse TCP Tunnel** · **Telnet Honeypot** · **WiFi Dead Drop** (anonymous message board) · **Printer Detection + Raw Print** · **SSDP Poisoner** · **DHCP Starvation** · **Rogue DHCP** (STA + AP) · **Network Hijacking** (chained MitM) · **WPAD Abuse** (credential capture) · **Autodiscover Abuse** (Exchange NTLM hash capture)

### IR (2)
TV-B-Gone · Samsung Remote

### BadUSB (1)
USB-HID payload runner with DuckyScript-lite

### MIMIR Drop-Box Client (1)
USB-C control client for BPI-M4 Zero pentest drop-box. Scan, 5 attack modes, pocket-mode MAC randomization. Hand-rolled JSON protocol, no heap allocation.

### Triton — Autonomous Gotchi
Cyberpunk helmet face with visor + trident crown. Hunts handshakes autonomously. RL channel picker persisted to SD. 4 modes: HUNT, STEALTH, SURGICAL, STORM. Bordered speech bubble, RL sparkline, TX indicator, HS flash.

### C5 Remote Nodes
ESP32-C5 companion for **5 GHz WiFi** + **802.15.4 Zigbee/Thread**. Dual-band scan, remote deauth, Zigbee sniff — all over ESP-NOW mesh. WS2812 NeoPixel status LED.

### Mesh
PigSync ESP-NOW presence beacon — foundation for multi-device coordination.

### Tools (9)
Flashlight · Stopwatch · Dice/Coin/8-Ball · Morse · MAC Randomizer · Calculator · Screen Test · SD Format · **Theme Picker** (6 palettes)

### Themes
| Theme | Aesthetic |
|---|---|
| POSEIDON | Cyan/magenta on black |
| PHANTOM | Deep purple/violet |
| MATRIX | Green phosphor on black |
| AMBER | Warm retro terminal |
| E-INK | Black on white (paper) |
| TRON | Neon cyan + electric blue glow |

## Hardware

| Component | Spec |
|---|---|
| MCU | ESP32-S3 @ 240 MHz |
| Display | 1.14" ST7789v2 240x135 |
| Keyboard | TCA8418 I2C matrix (Adv) |
| Radio | WiFi 4 + BLE 5.0 |
| IR | transmit-only LED |
| USB | native USB-C (HID + CDC) |
| Storage | microSD |

### Supported Hats (one at a time)

| Hat | Chips | Features |
|---|---|---|
| M5Stack CAP-LoRa1262 | SX1262 + ATGM336H | LoRa 868/915 MHz + GPS |
| PINGEQUA Hydra RF Cap 424 | CC1101 + nRF24L01+ | Sub-GHz 300-928 MHz + 2.4 GHz |
| ESP32-C5 (custom node) | ESP32-C5 | 5 GHz WiFi + 802.15.4 |

### Future Integration: MIMIR Drop-Box

**MIMIR** is a companion pentest drop-box running on a **Banana Pi BPI-M4 Zero** (Allwinner H618, 4GB RAM). POSEIDON connects via USB-C as the pocket-mode control client — no wireless link, pure opsec. MIMIR runs autonomous handshake hunting, cracking, and post-exploitation. The Cardputer is the interactive UI.

| MIMIR Component | Detail |
|---|---|
| SBC | BPI-M4 Zero (quad A53, 4GB LPDDR4) |
| Attack Radio | Alfa AWUS036ACM (MT7612U, dual-band) |
| UPS | Geekworm X306 (18650, MAX17040 gauge) |
| Display | Waveshare 4" Spectra 6 e-ink (640x400) |
| Sidecar | DAVEY JONES (ESP32-C6 + SX1262 LoRa) |
| Control | POSEIDON Cardputer via USB-CDC |

## Controls

| Key | Action |
|---|---|
| letter | jump to menu item |
| `;` / `.` | scroll up / down |
| `ENTER` | select / confirm |
| `=` | info panel |
| `` ` `` | back / ESC |
| `+` / `-` | tune frequency (in RF features) |
| `A` | auto-scan (in scan features) |
| `S` | save capture |
| `R` | replay / reset |

## Comparison

|  | POSEIDON | Flipper Zero | Evil-M5 | Marauder | Bruce |
|---|---|---|---|---|---|
| Keyboard | native QWERTY | D-pad | native QWERTY | none | varies |
| Features | 90+ | 50+ | 87 | 30+ | 40+ |
| Sub-GHz | CC1101 | CC1101 | CC1101 | none | CC1101 |
| 2.4 GHz RF | nRF24 | none | none | none | nRF24 |
| LoRa | SX1262 | none | none | none | SX1262 |
| 5 GHz WiFi | C5 node | none | none | none | none |
| Zigbee | C5 node | none | none | none | none |
| MouseJack | full suite | none | none | none | partial |
| BLE Spam (nRF24) | CRC24+whiten | none | none | none | none |
| Protocol Decoder | Princeton/CAME/NICE/Linear | 40+ | none | none | partial |
| Signal Library | 3,190+ .sub | community | none | none | community |
| DHCP Attacks | starve+rogue+hijack | none | starve+rogue | none | none |
| WPAD/Autodiscover | NTLM capture | none | yes | none | none |
| CIW Zeroclick | 157 payloads | none | yes | none | none |
| Gotchi/Pet | Triton (RL brain) | Dolphin | none | none | none |
| Themes | 6 palettes | 1 | 1 | 1 | 1 |
| Drop-box Client | MIMIR USB-C | none | none | none | none |

## Massive Shoutouts

- **[@7h30th3r0n3](https://github.com/7h30th3r0n3)** → [Evil-M5Project](https://github.com/7h30th3r0n3/Evil-M5Project) + [RaspyJack](https://github.com/7h30th3r0n3/Raspyjack) — **SaltyJack submenu** (DHCP Starve, Rogue DHCP, Responder, WPAD, NTLMv2 crack), plus WPAD, Autodiscover, CIW, honeypot, dead drop, SSDP poisoner all ported from his work. Hands down the single biggest code-inspiration for POSEIDON's LAN/creds side. Go star both repos.
- **[@JesseCHale](https://github.com/JesseCHale)** → [HaleHound-CYD](https://github.com/JesseCHale/HaleHound-CYD) — CC1101 init sequence reference
- **[@insecurityofthings](https://github.com/insecurityofthings)** → [uC_mousejack](https://github.com/insecurityofthings/uC_mousejack) — MouseJack ESB sniffer + HID injection protocol
- **[@0ct0sec](https://github.com/0ct0sec)** → [M5PORKCHOP](https://github.com/0ct0sec/M5PORKCHOP) — PigSync mesh, wardrive, spectrum concepts
- **[@justcallmekoko](https://github.com/justcallmekoko)** → [ESP32Marauder](https://github.com/justcallmekoko/ESP32Marauder) — WiFi promisc attack patterns
- **[@bmorcelli](https://github.com/bmorcelli) / [@pr3y](https://github.com/pr3y)** → [Bruce](https://github.com/pr3y/Bruce) — CC1101/nRF24 feature reference
- **[@ECTO-1A](https://github.com/ECTO-1A)** → [AppleJuice](https://github.com/ECTO-1A/AppleJuice) — CVE-2023-42941 research
- **[@SpiderLabs](https://github.com/SpiderLabs)** → [Responder](https://github.com/SpiderLabs/Responder) — LLMNR/NBT-NS protocol
- **[@UberGuidoZ](https://github.com/UberGuidoZ)** → [Flipper](https://github.com/UberGuidoZ/Flipper) — Sub-GHz signal library (3,190+ .sub files)
- **[@h2zero](https://github.com/h2zero)** → [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino) — BLE stack
- **[@jgromes](https://github.com/jgromes)** → [RadioLib](https://github.com/jgromes/RadioLib) — SX1262 LoRa driver
- **[@M5Stack](https://github.com/m5stack)** → [M5Cardputer](https://github.com/m5stack/M5Cardputer) + [M5Unified](https://github.com/m5stack/M5Unified) — hardware + drivers
- **[@PINGEQUA](https://www.pingequa.com/)** — Hydra RF Cap 424 hardware

**If you see anything in this code that came from your project and isn't credited — please open an issue.**

## Roadmap

### v0.3 — Meshtastic Node + Platform Fork

POSEIDON becomes a full Meshtastic participant (not just a listener) — can send, receive, page specific nodes. Plus the esp_wifi platform fork so spoofed-addr2 frames fully land on-air.

- [ ] Hand-rolled minimal protobuf for MeshPacket / Data / User / Position
- [ ] AES-CTR via mbedtls with default LongFast channel PSK
- [ ] Send broadcast text messages to the mesh
- [ ] Send direct messages (page specific node by ID)
- [ ] Live node roster with long/short names, SNR, hops, last-seen
- [ ] Optional position reporting — POSEIDON appears as a pin on Meshtastic apps when GPS has fix
- [ ] ESP32 Arduino core fork with patched `libnet80211.a` / `libpp.a` so spoofed-addr2 mgmt frames bypass the TX-FIFO sanity check (Marauder / Ghost ESP parity)
- [ ] CAD-based channel busy-check before LoRa TX (be a polite mesh citizen)

### v0.4 — ESP32-C5 Full Integration
The C5 companion node already does basic 5 GHz scan + deauth over ESP-NOW. v0.3 makes it a first-class citizen.

- [ ] C5 auto-flash from Cardputer SD (OTA over ESP-NOW)
- [ ] 5 GHz client hunting + targeted deauth (per-STA, not just per-AP)
- [ ] 5 GHz PMKID + handshake capture (relay EAPOL frames over ESP-NOW)
- [ ] 802.15.4 Zigbee full packet capture + Wireshark-compatible PCAP export
- [ ] Thread network discovery + device enumeration
- [ ] Zigbee replay attacks (stored frames on SD)
- [ ] Multi-C5 coordination — deploy multiple nodes, control all from one Cardputer
- [ ] C5 NeoPixel status: color-coded by activity (scan/attack/idle/capture)
- [ ] C5 power management — deep sleep between commands, wake on ESP-NOW

### v0.5 — MIMIR Drop-Box (BPI-M4 Zero)
The MIMIR client module exists. v0.4 makes the server side real.

- [ ] MIMIR daemon: `hcxdumptool` wrapper for real scan events (replacing placeholders)
- [ ] On-device WPA2 dictionary cracker (dual-core PBKDF2-SHA1, wordlist from SD)
- [ ] Bjorn orchestrator port — handshake → crack → auto-exploit pivot
- [ ] Pwnagotchi plugin compat shim (pisugarx/gps/wigle/wpa-sec)
- [ ] FENRIR RL policy head — autonomous exploit strategy selection
- [ ] Armbian H618 image recipe (one-flash deploy)
- [ ] MIMIR ↔ POSEIDON file transfer (pull captured .pcap/.22000 to Cardputer SD)
- [ ] Live MIMIR dashboard on Cardputer e-ink–style screen (for long-running ops)
- [ ] GPS-tagged attack logging with WiGLE integration
- [ ] Pocket-mode auto-start on cable connect

### v0.6 — nRF52840 Integration
The nRF52840 is the real BLE chip — full BLE 5.0 with long range, coded PHY, and direction finding. Adding it as a USB-connected sniffer module.

- [ ] nRF52840 dongle as BLE sniffer (USB-CDC bridge from Cardputer)
- [ ] Full BLE advertisement capture + decode (not just nRF24 fake-BLE)
- [ ] BLE connection hijacking (MITM via nRF52 + NimBLE coordination)
- [ ] BLE direction finding (AoA/AoD with multi-antenna nRF52)
- [ ] BLE long-range attacks (Coded PHY S=8, 4x range)
- [ ] Zigbee via nRF52840's 802.15.4 radio (alternative to C5)
- [ ] Thread border router attack surface enumeration
- [ ] nRF52840 firmware flasher from Cardputer SD
- [ ] Combined attack: nRF24 MouseJack + nRF52 BLE MITM simultaneously

### v0.7 — On-Device Intelligence
- [ ] On-device WPA2 cracker (handshake → PBKDF2-SHA1, dual-core ESP32-S3)
- [ ] SSH shell via `libssh_esp32` (interactive remote terminal)
- [ ] LDAP domain dump (hand-rolled BER/DER LDAP client)
- [ ] Web crawler (HTTP spider + link extraction for internal web apps)
- [ ] SIP attack suite (scan/enumerate/spoof/flood/ring-all)
- [ ] CCTV toolkit (camera discovery + default credential spray)
- [ ] Skimmer detector (BLE OUI blocklist for payment card skimmers)
- [ ] Wall of Flipper (detect + counter-spam nearby Flipper Zeros)
- [ ] Pwnagotchi beacon spam (fake peer broadcasts)
- [ ] Open WiFi dashboard (association + internet connectivity test)
- [ ] UPnP NAT exploitation (AddPortMapping for firewall punching)

### Ongoing
- [ ] More .sub signal library contributions (community PRs welcome)
- [ ] Theme community — user-submitted color palettes
- [ ] DuckyScript full parser (not just -lite)
- [ ] Flipper .sub protocol-encoded format support (not just RAW)
- [ ] KeeLoq rolling code analysis (manufacturer key database)
- [ ] SD card USB mass storage mode (export captures without pulling card)
- [ ] ESP-NOW encrypted mesh (AES-256 between POSEIDON nodes)
- [ ] CI/CD: PlatformIO GitHub Actions build + auto-release .bin

## Legal

This is for **authorized security testing, research, and education only**. You are responsible for complying with all applicable laws. Do not use against networks or devices without explicit authorization.

MIT License. Take it, fork it, improve it.

---

<div align="center">

```
   ≋≋≋≋≋     ≋≋≋≋≋     ≋≋≋≋≋     ≋≋≋≋≋     ≋≋≋≋≋     ≋≋≋≋≋
 ≋≋     ≋≋ ≋≋     ≋≋ ≋≋     ≋≋ ≋≋     ≋≋ ≋≋     ≋≋ ≋≋     ≋≋
≋         ≋         ≋         ≋         ≋         ≋         ≋
```

*commander of the deep*

</div>
