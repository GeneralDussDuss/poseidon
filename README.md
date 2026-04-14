# POSEIDON

Keyboard-first pentesting firmware for the **M5Stack Cardputer**. Commander of the deep.

Sibling project to [DAVEY JONES](https://github.com/GeneralDussDuss/davey-jones) — same nautical theme, different hardware, radically different UX.

## Why another Cardputer firmware?

Existing Cardputer firmwares (looking at you, Evil-Cardputer) cram 80+ features into a flat menu with hardcoded `;`, `.`, `,`, `/` navigation. They run on a device with a **full QWERTY keyboard** but force you to search-to-find or memorize index numbers. That's a Flipper UX on a keyboard device.

POSEIDON is designed around the keyboard:

- **Letter mnemonics at every level.** Press `W` for WiFi, then `S` for scan. Two keystrokes to a running attack.
- **Hierarchical menus**, not one flat 80-item list. Categories are obvious and discoverable.
- **Typed parameters.** `/open` to filter to open networks. Type a BSSID instead of picking from a list. The keyboard is there — use it.
- **Context hotkeys.** On a WiFi scan result: `D`=deauth, `C`=clone, `P`=portal, `I`=info. No menu diving.
- **Lazy radio domains** (from Davey Jones). Only one radio active at a time, heap stays uncluttered.
- **FreeRTOS-backed.** Scans run on a background task. UI never blocks.

## Hardware

**M5Stack Cardputer (K126)**

| Component | Spec |
|-----------|------|
| MCU | ESP32-S3 @ 240 MHz |
| RAM | 8 MB PSRAM |
| Flash | 8 MB |
| Display | 1.14" ST7789v2 240×135 |
| Keyboard | 56-key QWERTY (MX1.25) |
| Radio | WiFi 4 + BLE 5 |
| Other | IR LED, microSD, I2S speaker, PDM mic, Grove I2C |

## Menu tree (current)

```
POSEIDON
├── [W] WiFi
│   ├── [S] Scan          ← reference implementation (done)
│   ├── [D] Deauth
│   ├── [P] Portal
│   └── [B] Beacon spam
├── [B] Bluetooth
│   ├── [S] Scan
│   ├── [P] Spam
│   └── [H] Bad-KB
├── [I] IR
│   ├── [T] TV-B-Gone
│   └── [R] Remote
└── [S] System
    ├── [F] Files
    ├── [S] Settings
    └── [A] About
```

## Controls

| Key | Action |
|-----|--------|
| `letter` | Jump to menu item / invoke action hotkey |
| `ENTER` | Select / confirm |
| `FN + `<code>\`</code> | ESC (no dedicated ESC on 56-key layout) |
| `FN + ;` / `FN + .` | Up / Down |
| `FN + ,` / `FN + /` | Left / Right |
| `BKSP` | Delete char in text entry |
| `/` | Open filter/search prompt (in lists) |

In the WiFi scan list:

| Key | Action |
|-----|--------|
| `O` | Toggle open-only filter |
| `/` | Type SSID substring filter |
| `R` | Rescan |
| `ENTER` | Show AP details |

## Building

1. Install [PlatformIO](https://platformio.org/install/cli)
2. Plug in your Cardputer
3. ```bash
   pio run -t upload -t monitor
   ```

## Status

**v0.1.0 — foundation laid.**

Done:
- Project skeleton, PlatformIO config
- Keyboard input abstraction with FN-modified arrow keys
- Hierarchical menu with letter mnemonics
- Lazy radio domain switching (WiFi ↔ BLE ↔ idle)
- Status bar + footer hint system
- **WiFi Scan** — fully implemented, shows the pattern for every future feature

Stubs (next):
- WiFi deauth (with typed BSSID entry)
- Evil portal
- Beacon spam
- BLE scan / spam / Bad-KB
- IR TV-B-Gone / remote
- File browser, settings

## Architecture

```
src/
├── main.cpp          Boot, splash, menu dispatch
├── app.h             Shared constants, colors, screen geometry
├── input.h / .cpp    Keyboard polling, modal line editor
├── ui.h / .cpp       Status bar, footer, body drawing primitives
├── menu.h / .cpp     Hierarchical menu tree + runtime
├── radio.h / .cpp    Lazy domain manager (WiFi / BLE)
└── features/
    ├── wifi_scan.cpp Reference feature (live scan + filter + details)
    └── stubs.cpp     Placeholders for unimplemented features
```

### The feature pattern

Every feature should follow `features/wifi_scan.cpp`:

1. `radio_switch()` to the right domain first.
2. Set footer hints so hotkeys are discoverable.
3. Long-running work on a FreeRTOS task; UI thread stays responsive.
4. Main loop is `input_poll()` + redraw, non-blocking.
5. ESC returns to the caller.

Never block the UI. Never make the user navigate three submenus to reach an action.

## License

MIT
