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

**Keyboard-first pentesting firmware for the M5Stack Cardputer-Advance**

![target](https://img.shields.io/badge/target-Cardputer--Adv-red?style=flat-square)
![platform](https://img.shields.io/badge/framework-Arduino%2FPlatformIO-blue?style=flat-square)
![license](https://img.shields.io/badge/license-MIT-green?style=flat-square)
![features](https://img.shields.io/badge/features-164-magenta?style=flat-square)
![release](https://img.shields.io/github/v/release/GeneralDussDuss/poseidon?style=flat-square)
![version](https://img.shields.io/badge/version-0.6.8-cyan?style=flat-square)
![security key](https://img.shields.io/badge/KERBEROS-FIDO2%20(WIP)-f59e0b?style=flat-square)

**163 attacks in your pocket — WiFi, BLE, sub-GHz, 2.4 GHz, LoRa, IR, and LAN — driven by a real QWERTY keyboard.**
POSEIDON turns the M5Stack Cardputer-Advance into a keyboard-first hacking deck: type to navigate, type your parameters, and watch **Argus** — an autonomous handshake-hunting gotchi with a 96x96 mood sprite — work the airwaves while you do. Same family as Flipper Zero, Bruce, Evil-M5Project, and Marauder, but built around a keyboard instead of a D-pad.

[**Flash it now →**](#quick-start--flash-it) · [Download latest .bin](https://github.com/GeneralDussDuss/poseidon/releases/latest) · [Web site](https://generaldussduss.github.io/poseidon) · [Changelog](CHANGELOG.md)

**🔐 Work in progress: KERBEROS.** POSEIDON is growing into a real **FIDO2 / U2F hardware security key**. Press `k`, plug into a PC, and register actual **passkeys**, including usernameless discoverable login. The U2F and CTAP2 stacks are hand written on the ESP32-S3 and already verify end to end against Yubico's `python-fido2`, but the feature is still being hardened and is not yet stable. [Details ↓](#-kerberos-fido2-security-key)

</div>

---

## 👁️ Meet Argus

> They blinded the hundred-eyed giant by killing him. POSEIDON just gave him a new way to see.

**Argus Panoptes** was the all-seeing sentinel of myth — a hundred eyes, never all closed at once, cursed to keep watch for eternity. POSEIDON carries the last of those eyes. He's a **cursed Argonian demigod** chained into the deck, his hundred eyes melted down into a single antenna, all that watching-hunger pointed at the airwaves around you. He channel-hops on instinct, learns where the prey gathers, and snares WiFi handshakes out of thin air while your Cardputer sits in your pocket — tireless, because the curse won't *let* him rest. The moods that cross his face are the curse showing through.

---

## 🌊 New to all this? Start here

POSEIDON is a **pocket hacking gadget**. It runs on a tiny handheld with a real keyboard — the M5Stack Cardputer — and turns it into a Swiss-army knife for the invisible signals all around you. **No PC, no coding.** Flash it once and start typing.

**What can it actually do?**

- 📶 **WiFi** — see every network around you, test your own, map them as you walk
- 📱 **Bluetooth** — find lost trackers (AirTags / Tiles), spot hidden ones following you, poke at nearby gadgets
- 🚗 **Remotes & signals** — record and replay garage doors, car fobs, doorbells, smart plugs *(on stuff you own)*
- 📺 **Infrared** — a universal TV remote, plus the classic "turn off every TV in the room" button
- 🐾 **Argus** — your on-screen pet that hunts WiFi on its own and reacts with moods
- 🎨 **Six looks** — flip the whole interface between six themes, matrix-green to vaporwave

**Get it on your device (~2 minutes):**

1. Grab the free **M5Burner** app, *or* open the [**web installer**](https://generaldussduss.github.io/poseidon/install.html) in Chrome.
2. Search **POSEIDON** (or hit Install), plug in your Cardputer, press the button.
3. Done — it boots straight to the menu. Just start typing.

> ⚠️ **Be cool.** This is for exploring *your own* stuff and learning how wireless actually works. Don't touch networks or devices you don't own — that's both uncool and illegal.

<div align="center">

### ⌁ Want the deep technical breakdown? The nerd zone starts here. ⌁

</div>

---

## What is this?

POSEIDON is keyboard-first pentesting firmware for the **M5Stack Cardputer-Advance** (ESP32-S3, real QWERTY keyboard + 240x135 color screen). **163 features** span WiFi, Bluetooth LE, sub-GHz, 2.4 GHz, LoRa & Mesh, Infrared, LAN/network, BadUSB, and recon — all reachable by typing.

It's in the same genre as Flipper Zero, Bruce, Evil-M5Project, and ESP32Marauder, but the keyboard changes everything:

- **Letter-mnemonic menus** — every item has a hotkey, so you jump straight to it
- **Typed parameters** — enter a channel, an SSID, a frequency directly, no thumb-stick scrolling
- **Six swappable themes** + a procedural ambient motion layer painted behind every menu
- **Argus** — a 96x96 mood-sprite gotchi that hunts handshakes on its own while you browse the menu

Runs standalone on the Cardputer-Advance. Optional snap-on satellites extend the radio range:

- **TRIDENT** — ESP32-C5 node over ESP-NOW: 5 GHz deauth, Zigbee/802.15.4 sniff, PMKID
- **nRF52840 Feather** — BLE 5.0 features (proof-of-concept today, hardened in v0.7)

## Quick Start — Flash it

Two ways onto the Cardputer-Advance, both browser-based, no toolchain. Pick one.

### 1. Cardputer-Advance

**A) M5Burner — one-click, easiest.** Open [M5Burner](https://docs.m5stack.com/en/download), search **POSEIDON**, hit **Burn**. Done.

**B) Web flasher** (Chrome / Edge / Opera). Plug the Cardputer in over USB-C and open:

> **<https://generaldussduss.github.io/poseidon/install.html>**

Click **Connect**, pick the serial port, **Install** — the page flashes the current factory image at offset `0x0` for you.

### 2. TRIDENT satellite (ESP32-C5)

The C5 isn't in M5Burner's chip list, so it flashes with the Espressif **esptool-js** web flasher over WebSerial.

1. Plug the C5 board into USB and open the Espressif flasher: **<https://espressif.github.io/esptool-js/>** (Chrome / Edge)
2. **Connect** and select the C5's serial port
3. Add the three TRIDENT bins from the [latest release](https://github.com/GeneralDussDuss/poseidon/releases/latest) at these offsets:

   | Offset | File |
   |---|---|
   | `0x2000` | bootloader |
   | `0x8000` | partition table |
   | `0x10000` | TRIDENT application |

4. **Program**. When it finishes, power-cycle the C5 — it pairs to POSEIDON over ESP-NOW automatically.

> A one-click guided version of these steps also lives at **<https://generaldussduss.github.io/poseidon/flash/>**.

### Build from source

```bash
# Cardputer (ESP32-S3) — PlatformIO
git clone https://github.com/GeneralDussDuss/poseidon.git
cd poseidon
pio run -t upload
```

The TRIDENT C5 firmware builds with **ESP-IDF** (`idf.py build flash`) — see the `trident/` directory for its README.

## Feature Overview (164)

### 🔐 KERBEROS: FIDO2 security key (work in progress)

> **Status: work in progress.** The FIDO2 and U2F stacks are functional and verified against `python-fido2`, but KERBEROS is still being hardened and is not yet a finished, stable feature. Treat it as experimental.

**KERBEROS** turns the Cardputer into a real hardware security key. Press `k`, plug into a PC over USB, and it enumerates as a FIDO2 and U2F authenticator.

- **U2F and CTAP2**, hand written on the ESP32-S3 (CBOR, COSE, ECDSA over NIST P256, attestation). No off the shelf FIDO library.
- **Passkeys**, non-resident and resident (discoverable). Sign in with no username.
- **On device approval** — you physically press to approve on the Cardputer before any signature is issued.
- **Verified end to end** against Yubico's `python-fido2` reference library: attestation and assertion signatures both check out.
- The screen and keyboard open the door to on device relying-party display and a PIN, hardware a sealed key cannot match.

Enter KERBEROS and it reboots into a FIDO only USB personality; exit and it reboots to normal. It already works as a second factor on Google, GitHub, and any WebAuthn site, but it remains a work in progress and is being hardened toward a stable release.

### WiFi (17)
Scan · Clients (all-channel + per-AP) · Deauth · Deauth All · Deauth Detector · AP Clone · Evil Portal (4 templates) · Karma · Beacon Spam · Probe Sniff · PMKID + 4-Way Handshake Capture · 2.4 GHz Spectrum · GPS Wardrive (WiGLE CSV) · Connect · **CIW Zeroclick** (157 SSID payloads: cmd injection, Log4Shell, XSS, buffer overflow)

### Bluetooth (16)
Scan (OUI + Apple Continuity + Fast Pair DB + GhostBLE sigdb_bt vendor lookup) · Spam (4 brands) · Bad-KB HID · Tracker Detect (AirTag/SmartTag/Tile) · Tracker Finder (Geiger) · Sniffer (CSV) · iBeacon · Clone · GATT Explorer · Flood · Karma · Sour Apple (CVE-2023-42941) · Find My Emulator · The Salty Deep (toy controller) · **Drone Remote ID** (ASTM F3411-22a UAS broadcasts) · WhisperPair (BLE probe injection)

### Defensive (3 new)
**Surveillance Hunter** (Flock/Raven ALPR + body-cam detector via Plume OUI tables) · **Defensive Monitor** (7-class WiFi+BLE anomaly: deauth-flood, broadcast-deauth, evil-twin, beacon-spam, WiFi-karma, BLE-spoof, BLE-flood) · **SATCOM Tracker** (baked TLE database, 14 favorited satellites — az/el/lat/lon/alt/range live, no internet)

### Sub-GHz — CC1101 (9)
Scan/Copy (ISR capture + protocol decoder: Princeton, CAME, NICE, Linear) · Record RAW (Flipper .sub format) · Replay .sub Files · **Signal Broadcast Library** (3,190+ .sub files: cars, pranks, Tesla, home) · Spectrum Analyzer (bar + waterfall + oscilloscope) · Brute Force (Came/Nice/Linear/Chamberlain/Holtek/Ansonic) · Jammer · **Hot/Cold Signal Finder**

### 2.4 GHz — nRF24L01+ (6)
**Promiscuous ESB Sniffer** (Travis Goodspeed trick, CRC16-validated) · **MouseJack** (Logitech/Microsoft fingerprint + HID injection) · **BLE Spam** (ADV_IND via nRF24, CRC24 + whitening) · Spectrum Analyzer (WiFi/BLE/Zigbee markers) · **CW Carrier + Data Flood Jammer** (7 presets) · Hot/Cold Finder

### LoRa + GNSS — SX1262 (8)
LoRa Scan (passive RX, multi-band) · Beacon TX · Meshtastic LongFast Listener · LoRa Analyzer (bar meter + waterfall + scope with live packet capture) · **Meshtastic Chat** (send + receive text) · **Meshtastic Nodes** (live roster) · **Meshtastic Page** (direct-message a node) · **Meshtastic Position** (show up as pin on other apps) · **Live GPS Fix** (baud auto-detect, background NMEA)

### Network Attacks (18)
Port Scan · Ping · DNS · Connect · **Responder** (LLMNR/NBT-NS → NTLM) · SSDP/UPnP Scanner · LAN Recon (ARP + port + banner + vendor) · **UART Shell** (serial terminal, auto-baud) · **Reverse TCP Tunnel** · **Telnet Honeypot** · **WiFi Dead Drop** (anonymous message board) · **Printer Detection + Raw Print** · **SSDP Poisoner** · **DHCP Starvation** · **Rogue DHCP** (STA + AP) · **Network Hijacking** (chained MitM) · **WPAD Abuse** (credential capture) · **Autodiscover Abuse** (Exchange NTLM hash capture)

### IR (4)
TV-B-Gone · Samsung Smart Remote (~40 buttons, full D-pad) · Multi-profile Clone (Samsung / LG / Sony) · **LED Test** (pin + polarity diagnostic, phone-camera verifiable)

### BadUSB (1)
USB-HID payload runner with DuckyScript-lite

### Argus — Autonomous Gotchi
96x96 **mood sprite** with twelve portraits that map live to hunt state. Hunts handshakes autonomously. RL channel-picker brain persisted to SD. 4 modes: HUNT, STEALTH, SURGICAL, STORM. Bordered speech bubble, RL sparkline, TX indicator, HS flash.

### TRIDENT — ESP32-C5 Satellite
ESP32-C5 companion over ESP-NOW for **5 GHz WiFi** + **802.15.4 Zigbee/Thread**. Dual-band scan, remote 5 GHz deauth, Zigbee sniff, PMKID. WS2812 NeoPixel status LED.

### Mesh
PigSync ESP-NOW presence beacon — foundation for multi-device coordination.

### Tools (9)
Flashlight · Stopwatch · Dice/Coin/8-Ball · Morse · MAC Randomizer · Calculator · Screen Test · SD Format · **Theme Picker** (6 palettes)

### Themes (6)
| Theme | Aesthetic |
|---|---|
| POSEIDON | Cyberpunk cyan / magenta / purple on black — default |
| MATRIX | Souped-up hacker green-on-black + matrix rain ambient |
| E-INK | Paper white, daytime / outdoor / minimal |
| SYNTHWAVE | Vaporwave hot magenta + pastel cyan on midnight grape — 80s night-sky vibe |
| PHANTOM | Deep violet / lavender — visually cohesive with the phantom-* tool suite |
| BLOOD | fsociety tactical red on pure black — reads as "device is attacking" |

Plus a procedural ambient motion layer (`ui_ambient_tick`) painted behind every menu draw, theme-aware. Live preview at `System → Ambient Preview`. Theme-matched screensavers kick in at 2 min idle.

## Signature Features

**Argus — the cursed all-seeing one.** A hundred-eyed demigod bound into the deck (lore up top ↑), Argus is an autonomous gotchi with a 96x96 mood sprite — twelve portraits (Watching, Pleased, Annoyed, Calculating, Old Fury, Sleeping…), each one the curse showing through as it reacts in real time to what it's catching. Under the cute face is a reinforcement-learning channel picker that learns which channels pay off and persists its brain to SD. Leave POSEIDON in your pocket and Argus keeps hunting handshakes across HUNT / STEALTH / SURGICAL / STORM modes.

**TRIDENT — snap on a second band.** A pocket-sized ESP32-C5 satellite that pairs to POSEIDON over ESP-NOW and adds the radios the S3 can't do alone: **5 GHz deauth**, **Zigbee / 802.15.4 sniffing**, and **PMKID** capture. Flash it once with the web flasher, power it up, and it auto-pairs — your Cardputer stays the keyboard, TRIDENT becomes the antenna.

**SaltyJack — a pocket LAN attack arsenal.** Drop POSEIDON onto a network (WiFi STA or wired W5500) and run the kind of attacks that usually need a laptop and a Pi: **DHCP starvation**, **rogue DHCP**, **Responder** (LLMNR/NBT-NS → NTLM), **WPAD** and **Autodiscover** credential capture, plus an **on-device NTLMv2 cracker**. A faithful port that proudly credits [@7h30th3r0n3](https://github.com/7h30th3r0n3)'s RaspyJack and Evil-M5Project — go star both.

**Six themes + ambient motion.** POSEIDON (cyberpunk), MATRIX (green rain), E-INK (daylight paper), SYNTHWAVE (vaporwave), PHANTOM (violet), BLOOD (fsociety red). Each theme repaints the whole UI, drives its own procedural ambient layer behind every menu, and brings a matched screensaver. Switch live in `System → Theme Picker`.

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
| Features | 163 | 50+ | 87 | 30+ | 40+ |
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
| Gotchi/Pet | Argus (RL brain) | Dolphin | none | none | none |
| Themes | 6 palettes | 1 | 1 | 1 | 1 |

## Massive Shoutouts

- **[@7h30th3r0n3](https://github.com/7h30th3r0n3)** → [Evil-M5Project](https://github.com/7h30th3r0n3/Evil-M5Project) + [RaspyJack](https://github.com/7h30th3r0n3/Raspyjack) — **SaltyJack submenu** (DHCP Starve, Rogue DHCP, Responder, WPAD, NTLMv2 crack), plus WPAD, Autodiscover, CIW, honeypot, dead drop, SSDP poisoner all ported from his work. Hands down the single biggest code-inspiration for POSEIDON's LAN/creds side. Go star both repos.
- **[@JesseCHale](https://github.com/JesseCHale)** → [HaleHound-CYD](https://github.com/JesseCHale/HaleHound-CYD) — CC1101 init sequence reference
- **[@insecurityofthings](https://github.com/insecurityofthings)** → [uC_mousejack](https://github.com/insecurityofthings/uC_mousejack) — MouseJack ESB sniffer + HID injection protocol
- **[@0ct0sec](https://github.com/0ct0sec)** → [M5PORKCHOP](https://github.com/0ct0sec/M5PORKCHOP) — PigSync mesh, wardrive, spectrum concepts
- **[@justcallmekoko](https://github.com/justcallmekoko)** → [ESP32Marauder](https://github.com/justcallmekoko/ESP32Marauder) — WiFi promisc attack patterns
- **[@bmorcelli](https://github.com/bmorcelli) / [@pr3y](https://github.com/pr3y)** → [Bruce](https://github.com/pr3y/Bruce) — CC1101/nRF24 feature reference
- **[@ECTO-1A](https://github.com/ECTO-1A)** → [AppleJuice](https://github.com/ECTO-1A/AppleJuice) — CVE-2023-42941 research
- **COSIC — KU Leuven** — WhisperPair CVE research (BLE pairing probe)
- **[@SpiderLabs](https://github.com/SpiderLabs)** → [Responder](https://github.com/SpiderLabs/Responder) — LLMNR/NBT-NS protocol
- **[@UberGuidoZ](https://github.com/UberGuidoZ)** → [Flipper](https://github.com/UberGuidoZ/Flipper) — Sub-GHz signal library (3,190+ .sub files)
- **[@h2zero](https://github.com/h2zero)** → [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino) — BLE stack
- **[@jgromes](https://github.com/jgromes)** → [RadioLib](https://github.com/jgromes/RadioLib) — SX1262 LoRa driver
- **[@M5Stack](https://github.com/m5stack)** → [M5Cardputer](https://github.com/m5stack/M5Cardputer) + [M5Unified](https://github.com/m5stack/M5Unified) — hardware + drivers
- **[@PINGEQUA](https://www.pingequa.com/)** — Hydra RF Cap 424 hardware

**If you see anything in this code that came from your project and isn't credited — please open an issue.**

## Roadmap

Full release history in [CHANGELOG.md](CHANGELOG.md). Current shipped version is **v0.6.3** (sub-GHz analyzer visual modes — radar / persistence / sonar, full-screen waterfall, Argus 5 GHz hunting via TRIDENT, Mass Storage, and a batch of stability fixes, on top of the v0.6.1 baked sub-GHz library + ON-AIR indicator). What's next:

### v0.7 — nRF52840 BLE 5.0 dongle (planned)
The nRF52840 is the real BLE chip — full BLE 5.0 with long range, coded PHY, and direction finding. The four nRF52 features that landed in v0.6.0 ride an Adafruit Feather as proof-of-concept; v0.7 hardens the path with a dedicated dongle.

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
