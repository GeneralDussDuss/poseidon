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

**A keyboard-first pentesting firmware for the M5Stack Cardputer**

![target](https://img.shields.io/badge/target-M5Stack%20Cardputer-red?style=flat-square)
![platform](https://img.shields.io/badge/framework-Arduino%2FPlatformIO-blue?style=flat-square)
![license](https://img.shields.io/badge/license-MIT-green?style=flat-square)
![status](https://img.shields.io/badge/status-actively%20terrifying-magenta?style=flat-square)

</div>

---

## What is this?

POSEIDON is a pentesting firmware written from scratch for the M5Stack Cardputer (ESP32-S3 + 1.14" LCD + 56-key QWERTY + IR + microSD + Grove I²C + speaker). It's in the same family as Flipper Zero, Bruce, Evil-Cardputer, and ESP32Marauder — but designed around the one thing those all lack: **a real keyboard**.

Every menu has letter mnemonics (press `W` for WiFi, `S` for Scan, two keystrokes to a running attack). Every feature has a typed-parameter mode (type a BSSID directly, no list picking). Every list supports `/`-filter. No Flipper-style index-counting.

## Highlights

- **44 features** across WiFi, BLE, IR, USB-HID, Network, Mesh, and drop-box LAN recon
- **Hierarchical menus with letter mnemonics** — never more than 3 keystrokes to anything
- **In-menu `=` info panel** — thorough description of every tool, not just a footer hint
- **Slide transitions** between menus, scrollable lists with cursor-centered viewport
- **Animated splash** — Hokusai's *Great Wave* with magenta scanline sweep
- **Sick ass animations** everywhere: radar sweeps on scans, matrix rain on handshake capture, hex packet streams on beacon spam, glitch blocks on flood attacks, EQ bars on Karma, radial wave pulses on Responder
- **Lazy radio management** — only one radio stack (WiFi/BLE) active at a time; 100 KB RAM reclaimed when you leave a domain
- **Keyboard-first everywhere** — typed BSSIDs, typed DuckyScript, typed wireshark-style filters
- **Adaptive learning** — the Triton handshake-hunter pet has a reinforcement-learning layer that remembers which channels produce captures and biases its dwell time toward them, persisted across reboots

## Feature matrix

### WiFi (16)
Scan · Clients (all + per-AP) · Deauth · Deauth all · Deauth detector · AP Clone · Evil Portal (4 templates) · Karma · Beacon spam · Probe sniff · PMKID + 4-way Handshake capture · 2.4 GHz Spectrum · GPS Wardrive → WiGLE · Connect

### Bluetooth (14)
Scan (OUI + Apple Continuity + Fast Pair DB) · Spam (4 brands) · Bad-KB HID keyboard · Tracker detect (AirTag/SmartTag/Tile) · Tracker Finder (Geiger) · Sniffer → CSV · iBeacon · Clone (connectable GATT) · GATT Explorer (read/write) · Flood · Karma · Sour Apple (CVE-2023-42941) · Find My emulator · **The Salty Deep** (Lovense/WeVibe/Satisfyer controller)

### IR (2)
TV-B-Gone · Samsung remote

### BadUSB (1)
USB-HID payload runner with DuckyScript-lite + built-in library

### Network (6) — drop-box mode
Port scan · Ping · DNS · Connect · **Responder** (LLMNR/NBT-NS/mDNS poisoner → NTLM hash capture) · **UPnP / SSDP scanner** · **LAN Recon** (ARP + port + banner + vendor + CSV export)

### Mesh (1)
**PigSync** — ESP-NOW presence beacon (foundation for the C5 drop-node C2 concept when those boards arrive)

### Triton
The autonomous handshake gotchi. Runs promisc capture, hunt-mode deauth, and mood states. Learns its best channels via ε-greedy softmax and persists the brain to SD.

### Tools (8)
Flashlight · Stopwatch · Dice/Coin/8-Ball · Morse sender · MAC randomizer · Calculator · Screen test · SD format

### System (4)
File browser · Clock · Settings · About

## Usage

```bash
# Clone + build
git clone https://github.com/GeneralDussDuss/poseidon.git
cd poseidon
pio run -t upload -t monitor
```

Plug the Cardputer in, flash, boot. Splash plays, any key drops you into the main menu.

### Controls

| key | action |
|---|---|
| letter | jump to menu item by mnemonic |
| `;` / `.` | scroll up / down |
| `ENTER` | select / confirm |
| `=` (also `?`) | open info panel for highlighted item |
| `` ` `` (backtick) | back / cancel |
| `/` | open filter prompt in any list |
| `R` | rescan / reset (in context) |

## Screenshots

*(Coming soon — plug it in and see)*

## Architecture

```
src/
├── main.cpp             boot, splash, hand off to menu
├── app.h                screen geometry + palette
├── input.h / .cpp       keyboard polling, modal line editor
├── ui.h / .cpp          status/footer, splash, animation primitives
├── menu.h / .cpp        hierarchical menu + slide transitions + info panels
├── radio.h / .cpp       lazy domain switcher (WiFi ↔ BLE ↔ idle)
├── splash.cpp           Hokusai splash animation
├── ble_db.h / .cpp      ~200 OUIs + Apple Continuity + Fast Pair + BT SIG UUIDs
├── ble_types.h          shared BLE target struct
├── wifi_types.h         shared AP struct
├── gps.h / .cpp         NMEA parser for the LoRa-GNSS HAT
├── mesh.h / .cpp        ESP-NOW presence protocol
├── dhcp_cache.h / .cpp  MAC → hostname cache from DHCP Option 12
└── features/            one file per feature
    ├── wifi_*.cpp       (15 wifi features)
    ├── ble_*.cpp        (14 ble features)
    ├── ir_*.cpp         (2 ir features)
    ├── net_*.cpp        (6 network features)
    ├── triton.cpp       handshake gotchi
    ├── badusb.cpp       usb-hid payload runner
    ├── tools.cpp        flashlight / stopwatch / dice / etc.
    ├── system_tools.cpp saved wifi / clock / file browser / settings
    ├── mesh_status.cpp  pigsync peer table viewer
    ├── splash.cpp       animated splash with wave + title glow
    └── stubs.cpp        about screen
```

## Massive shoutouts

This firmware would not exist without the work of these people. Where code or protocol knowledge was ported, it's credited inline in the source.

- **[@7h30th3r0n3](https://github.com/7h30th3r0n3)** → [Evil-M5Project](https://github.com/7h30th3r0n3/Evil-M5Project)
  The canonical Cardputer pentest firmware. The radial wave + arc pulse animation in POSEIDON's Handshake Capture screen is ported directly from Evil-Cardputer's `showWaitingAnimationNTLM`. Many UX patterns and feature ideas borrowed.

- **[@0ct0sec](https://github.com/0ct0sec)** → [M5PORKCHOP](https://github.com/0ct0sec/M5PORKCHOP)
  Inspiration for the PigSync ESP-NOW mesh, WiGLE v1.6 wardrive CSV format, and the 2.4 GHz spectrum analyzer concept.

- **[@justcallmekoko](https://github.com/justcallmekoko)** → [ESP32Marauder](https://github.com/justcallmekoko/ESP32Marauder)
  Foundational work on ESP32 WiFi promisc attacks. Deauth frame formats, beacon spam patterns, and probe sniff techniques borrowed.

- **[@bmorcelli](https://github.com/bmorcelli) / [@pr3y](https://github.com/pr3y)** → [Bruce](https://github.com/pr3y/Bruce)
  Multi-device pentest firmware. Feature-set ideas for captive portal, RF replay, network attacks.

- **[@ECTO-1A](https://github.com/ECTO-1A)** → [AppleJuice](https://github.com/ECTO-1A/AppleJuice)
  Original research on CVE-2023-42941 (the iOS 17 Apple Continuity DoS). POSEIDON's Sour Apple feature is built directly on this work.

- **[@RapierXbox](https://github.com/RapierXbox)** → [ESP32-Sour-Apple](https://github.com/RapierXbox/ESP32-Sour-Apple)
  ESP32 port of AppleJuice. The exact Sour Apple payload bytes come from this repo.

- **[@SpiderLabs](https://github.com/SpiderLabs)** → [Responder](https://github.com/SpiderLabs/Responder)
  The canonical LLMNR/NBT-NS/mDNS poisoner. POSEIDON's Responder feature is a lite port of its core protocol handling.

- **[@h2zero](https://github.com/h2zero)** → [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino)
  The BLE stack this entire firmware relies on.

- **[@M5Stack](https://github.com/m5stack)** → [M5Cardputer](https://github.com/m5stack/M5Cardputer) + [M5Unified](https://github.com/m5stack/M5Unified)
  The hardware and the base drivers.

- **Agostino Carracci (1557 – 1602)** / **Katsushika Hokusai (1760 – 1849)**
  Long dead, but the two public-domain engravings I rolled through during splash-screen iteration are theirs. Current splash is Hokusai's *Great Wave off Kanagawa*, c. 1831.

**If you see anything in this code that came from your project and isn't credited — please open an issue or PR so I can fix the attribution.**

## Comparison

|  | POSEIDON | Flipper Zero | Evil-Cardputer | Marauder | Bruce |
|---|---|---|---|---|---|
| Hardware | Cardputer | custom | Cardputer | any ESP32 | multi-device |
| Keyboard input | ✅ native | ❌ | ✅ native | ❌ | ⚠️ |
| Typed parameters | ✅ | ❌ | ⚠️ | ❌ | ⚠️ |
| Letter mnemonics | ✅ everywhere | ❌ | ❌ | ❌ | ❌ |
| Per-item info panels | ✅ (`=` key) | ❌ | ❌ | ❌ | ❌ |
| Slide transitions | ✅ | ⚠️ basic | ❌ | ❌ | ❌ |
| WiFi scan + deauth + portal | ✅ | ❌ (sub-GHz focus) | ✅ | ✅ | ✅ |
| PMKID + full 4-way handshake | ✅ hashcat 22000 | ❌ | ✅ | ✅ | ✅ |
| BLE HID Bad-KB | ✅ w/ 11 disguises | ❌ | ⚠️ | ❌ | ⚠️ |
| Sour Apple (CVE-2023-42941) | ✅ | ✅ (with app) | ✅ | ❌ | ✅ |
| Find My / AirTag emulator | ✅ flocks of 32 | ⚠️ | ❌ | ❌ | ⚠️ |
| LAN auto-recon (drop-box) | ✅ | ❌ | ⚠️ | ❌ | ⚠️ |
| Responder (NTLM capture) | ✅ | ❌ | ❌ | ❌ | ❌ |
| UPnP/SSDP enumerator | ✅ | ❌ | ❌ | ❌ | ❌ |
| Triton / autonomous gotchi | ✅ w/ RL | ❌ | ⚠️ karma-auto | ❌ | ❌ |
| USB HID BadUSB | ✅ | ✅ | ✅ | ❌ | ⚠️ |
| Wireless toy controller | ✅ | ❌ | ❌ | ❌ | ❌ |
| Adaptive ML per-channel | ✅ Triton | ❌ | ❌ | ❌ | ❌ |

## Hardware

| Component | Spec |
|---|---|
| MCU | ESP32-S3 @ 240 MHz |
| RAM | 8 MB PSRAM |
| Flash | 8 MB |
| Display | 1.14" ST7789v2 240×135 |
| Keyboard | 56-key QWERTY (MX1.25) |
| Radio | WiFi 4 + BLE 5.0 |
| IR | transmit-only LED on GPIO 44 |
| USB | native USB-C (HID capable) |
| Storage | microSD |
| Speaker | I²S |
| Grove | I²C |

**Planned expansion:** ESP32-C5 for **5 GHz WiFi** + **802.15.4 (Zigbee/Thread)** as remote-radio nodes over ESP-NOW mesh. The C5 is the only ESP with 5 GHz — once wired in, POSEIDON becomes the only keyboard-driven pentesting device on the planet that can see 5 GHz networks.

## Building

### Prerequisites
- [PlatformIO](https://platformio.org/install/cli)
- An M5Stack Cardputer

### Build

```bash
pio run
```

### Flash

Hold the Cardputer's **G0** button, press **Reset**, release G0. Screen goes dark = download mode.

```bash
pio run -t upload
```

Windows sometimes needs `--upload-port COMn` explicit. Check device manager for the virtual serial.

### Monitor

```bash
pio run -t monitor
```

## Legal

This is for **authorized security testing, research, and education only**. You are responsible for complying with all applicable laws. Do not run any of this against networks or devices you don't own or have explicit written authorization to test.

That said — it's your Cardputer, your network, your pentest engagement. Go find things.

## License

MIT. Take it, fork it, improve it. Credit the people above who deserve credit.

---

<div align="center">

```
   ≋≋≋≋≋     ≋≋≋≋≋     ≋≋≋≋≋     ≋≋≋≋≋     ≋≋≋≋≋     ≋≋≋≋≋
 ≋≋     ≋≋ ≋≋     ≋≋ ≋≋     ≋≋ ≋≋     ≋≋ ≋≋     ≋≋ ≋≋     ≋≋
≋         ≋         ≋         ≋         ≋         ≋         ≋
```

*commander of the deep*

</div>
