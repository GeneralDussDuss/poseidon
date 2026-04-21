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
![version](https://img.shields.io/badge/version-0.5.0-cyan?style=flat-square)

[**Download Latest .bin**](https://github.com/GeneralDussDuss/poseidon/releases/latest) — flash with M5Burner, the browser (Chrome/Edge), or esptool at offset 0x0

**v0.5.0 shipped — TRIDENT.** The ESP32-C5 companion satellite is now a proper peripheral over ESP-NOW. POSEIDON's Cardputer-Adv is 2.4 GHz-only; TRIDENT adds **5 GHz deauth** (bypasses `ieee80211_raw_frame_sanity_check` via a direct binary patch to `libnet80211.a`), **802.15.4 Zigbee sniffing**, and **5 GHz PMKID capture** — features the S3 physically can't do. LoRa is now fully working end-to-end on the CAP-LoRa1262 hat (canonical SX1262 init, shared HSPI with SD, proper BUSY pin). Every scan / live screen is flicker-free: `ui_draw_status`, wifi_scan, ble_scan, wifi_clients, wifi_spectrum, and the c5 dashboards all cache state and only repaint on actual change. bmorcelli Launcher support ships as a second PIO env (`cardputer-launcher`) that links into the `ota_0` slot at 0x170000. Full breakdown in [CHANGELOG](CHANGELOG.md).

**TRIDENT web flasher** — ESP32-C5 isn't in M5Burner's chip list, so we put a one-click WebSerial flasher straight in the docs. Plug the C5 in, hit a button, done. Manual `esptool` + full ESP-IDF build-from-source instructions also live there.

</div>

---

## What is this?

POSEIDON is a pentesting firmware for the M5Stack Cardputer-Adv (ESP32-S3). 90+ features across WiFi, BLE, sub-GHz, 2.4 GHz, LoRa, IR, network attacks, and more. In the same family as Flipper Zero, Bruce, Evil-M5Project, and ESP32Marauder — but built around a **real keyboard** with letter mnemonics, typed parameters, and 6 swappable visual themes.

Supports four hardware hats (one at a time):
- **M5Stack CAP-LoRa1262** — LoRa (SX1262) + GNSS (GPS)
- **PINGEQUA Hydra RF Cap 424** — CC1101 sub-GHz + nRF24L01+ 2.4 GHz
- **ESP32-C5 companion node** — 5 GHz WiFi + Zigbee via ESP-NOW mesh
- **W5500 SPI → Ethernet** — wired RJ45 for full RaspyJack / SaltyJack parity (DHCP starve, rogue DHCP, Responder, WPAD, NTLM harvest) over a LAN cable instead of WiFi STA

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

### Planned: MIMIR Drop-Box (future)

**MIMIR** is a companion pentest drop-box targeting a **Banana Pi BPI-M4 Zero** (Allwinner H618, 4GB RAM). Design goal: POSEIDON connects via USB-C as the pocket-mode control client — no wireless link, pure opsec. MIMIR would run autonomous handshake hunting, cracking, and post-exploitation while the Cardputer stays the interactive UI. **Not implemented yet — aspirational roadmap item, no client code in POSEIDON today.**

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

Full release history in [CHANGELOG.md](CHANGELOG.md). Current shipped version is **v0.5.0** (TRIDENT + LoRa + flicker fixes + M5 Launcher support). What's next:

### v0.6 — nRF52840 Integration (planned)
The nRF52840 is the real BLE chip — full BLE 5.0 with long range, coded PHY, and direction finding. Adding it as a USB-connected sniffer module.

- [ ] nRF52840 dongle as BLE sniffer (USB-CDC bridge from Cardputer)
- [ ] Full BLE advertisement capture + decode (not just nRF24 fake-BLE)
- [ ] BLE connection hijacking (MITM via nRF52 + NimBLE coordination)
- [ ] BLE direction finding (AoA/AoD with multi-antenna nRF52)
- [ ] BLE long-range attacks (Coded PHY S=8, 4x range)
- [ ] Zigbee via nRF52840's 802.15.4 radio (alternative to TRIDENT)
- [ ] Thread border router attack surface enumeration
- [ ] nRF52840 firmware flasher from Cardputer SD
- [ ] Combined attack: nRF24 MouseJack + nRF52 BLE MITM simultaneously

### Aspirational — MIMIR Drop-Box (BPI-M4 Zero)
**Not started.** A future companion pentest drop-box on a Banana Pi BPI-M4 Zero running `hcxdumptool` + on-device cracker + Bjorn orchestrator. POSEIDON would act as the pocket-mode USB-C control client. No code exists today.

- [ ] MIMIR daemon: `hcxdumptool` wrapper
- [ ] On-device WPA2 dictionary cracker (dual-core PBKDF2-SHA1)
- [ ] Bjorn orchestrator port — handshake → crack → auto-exploit pivot
- [ ] FENRIR RL policy head — autonomous exploit strategy selection
- [ ] Armbian H618 image recipe (one-flash deploy)
- [ ] MIMIR ↔ POSEIDON file transfer
- [ ] GPS-tagged attack logging with WiGLE integration

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
