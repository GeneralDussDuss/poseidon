# Proposal: CC1101 + nRF24L01+ Cardputer HAT integration

**Status:** draft, awaiting hardware in hand (hat arrives ~2026-04-15)
**Owner:** @GeneralDussDuss

---

## What's coming

A Cardputer HAT carrying:

- **CC1101** — sub-GHz transceiver, 300–928 MHz, OOK/ASK/FSK/MSK
- **nRF24L01+** — 2.4 GHz raw packet transceiver (below the WiFi stack)

Both are SPI peripherals on the Cardputer's expansion header. This kills the need for a
remote node entirely — POSEIDON talks directly to both chips over SPI.

## Why this is the killer feature

| Chip     | Unlocks                                                             |
|----------|---------------------------------------------------------------------|
| CC1101   | Garage doors, car remotes (fixed-code replay), weather stations, wireless sensors, 433/868 MHz jammer detect, PT2240/EV1527 decode, LoRa-adjacent |
| nRF24    | Microsoft wireless keyboard/mouse injection (Mousejack, CVE-2016-0378), Logitech Unifying dongle hijack, Bastille-style keyboard sniffing, 2.4 GHz raw packet inject **below** the WiFi/BLE radios |

Neither exists on any other Cardputer firmware. Both are the kind of attack that makes
people actually watch demo videos.

## Wiring (confirm against hat docs when hardware arrives)

Cardputer exposes these on the expansion header (Grove I²C + rear pads):

| Cardputer GPIO | Likely role              |
|----------------|--------------------------|
| 13             | G13 / SDA  (can repurpose as SPI) |
| 17             | G17 / SCL  (can repurpose)        |
| 41 / 42        | additional header lines, HAT-specific |

SPI bus: reuse **HSPI** / `SPI2_HOST`. Rough pin map (TBD on hat arrival):

```
SCK   → G13 (or dedicated HAT pin)
MOSI  → G17
MISO  → G??
CC1101 CS   → G??
nRF24  CS   → G??
CC1101 GDO0 → G?? (interrupt / sync pulse)
nRF24  IRQ  → G?? (interrupt)
nRF24  CE   → G??
```

The SD card already uses SPI (SCK 40, MISO 39, MOSI 14, CS 12). If HAT needs different
pins we'll have two independent SPI buses; SPI2 and SPI3 are both available on S3.

## Lazy radio integration

Sub-GHz and nRF24 both get folded into `src/radio.cpp` as new domains:

```c
enum radio_domain_t {
    RADIO_OFF,
    RADIO_WIFI,
    RADIO_BLE,
    RADIO_SUBGHZ,   // CC1101 via SPI
    RADIO_NRF24,    // nRF24L01+ via SPI
};
```

WiFi/BLE tear down cleanly before either hat radio comes up, same pattern as the
existing switch. Hat radios are cheap to enable/disable (just CS lines) so switching
is effectively instantaneous compared to the WiFi stack init.

## New menu domains

Two top-level mnemonics:

```
R Radio (sub-GHz)
  s Scan       — RSSI sweep 300-928 MHz, live waterfall, favorite-bands
  c Capture    — record signal to SD (.sub format, Flipper-compatible)
  r Replay     — TX captured file
  d Decode     — OOK/ASK PT2240 / EV1527 / HT6P20D / garage remotes
  j Jam det    — passive jammer detector across favorite bands

N nRF / 2.4G raw
  s Scan       — nRF 125-ch RSSI sweep (Mousejack-style)
  m Mousejack  — Microsoft/Logitech keyboard inject
  s Sniff      — promiscuous capture (decoded to SD)
  h HID clone  — pair as a Logitech Unifying receiver target
```

## File formats

- **`.sub`** for captured sub-GHz (Flipper Zero format). Users get Flipper ecosystem
  compatibility for free — signals captured on a Flipper can be replayed on the
  Cardputer and vice versa.
- **`.csv`** for nRF sniffs (channel, MAC, payload hex, timestamp).
- **`.rc`** for decoded remote-control codes (protocol, key, repeats).

## Protocols we'd support

**CC1101 decoders (ship minimum viable):**
- PT2240 / EV1527 (fixed-code garage/gate remotes)
- HT6P20D
- Princeton PT2262
- Nice FLOR-S (rolling code, passive observe only — no replay)
- Weather stations: Ambient/Acurite (parse-only)

**nRF24L01+ targets:**
- Microsoft Wireless Keyboard 3000/800/850 (Mousejack)
- Logitech Unifying M series (unpatched firmware only)
- Amazon Basics wireless mice / keyboards
- HID over 2.4 GHz generic sniffing

## Library options

Pick existing libs first, only write from scratch if licensing or fit is wrong:

- **SmartRC-CC1101-Driver-Lib** (LSatan/SmartRC) — well-maintained CC1101 driver with OOK
  replay and frequency agility. Arduino-native. Zero-dep.
- **RadioLib** (jgromes) — big general-purpose radio library, covers CC1101 + nRF24 +
  LoRa. Probably overkill; evaluate size cost before pulling in.
- **RF24** (nRF24/RF24) — canonical Arduino nRF24 library.
- **nrf-research-firmware** — Bastille's original Mousejack tooling; port the attack
  logic, not the firmware (different chip).

## Non-goals

- No SDR. NESDR Smart HF lives in the phantom ecosystem for that job, proxied via the
  Banana Pi — POSEIDON isn't trying to be a software-defined radio.
- No rolling-code replay attacks against cars/garages we don't own. Decoder shows the
  code, doesn't help forge. Hard rule.
- No web UI for radio config — it's a keyboard-first tool.

## Open questions (raise when hardware is in hand)

- Final pinout on the hat — exact GPIOs for SCK/MOSI/MISO, CS, GDO0, IRQ, CE
- Antenna: does the hat have its own antenna, or does it terminate at SMA for external?
- Power: does the hat draw enough to bother the Cardputer battery? CC1101 TX is ~35 mA
  continuous, nRF24 TX is ~12 mA — not bad, but if user runs a sub-GHz replay loop
  while BLE scanning, we might hit brownout.
- Interrupt routing: S3 has limited interruptable pins, need to confirm hat pins map
  to interruptable GPIOs.
- Antenna switching if both chips share one antenna path.

## Proposed commit sequence (once hat arrives)

1. `task 4.1a: CC1101 SPI probe — detect chip + print part number on boot`
2. `task 4.1b: sub-GHz menu scaffold — scan stub with live RSSI display`
3. `task 4.1c: sub-GHz capture to SD as .sub`
4. `task 4.1d: sub-GHz replay from .sub`
5. `task 4.1e: PT2240/EV1527 decoder`
6. `task 4.1f: jammer detector`
7. `task 4.1g: nRF24 SPI probe + channel scan`
8. `task 4.1h: Mousejack HID injection`
9. `task 4.1i: nRF promiscuous sniff to CSV`

Each commit stands alone + is testable in isolation. Ship one per evening, iterate.

## Waiting

Hold implementation until hardware is in hand and pinout is confirmed. Ping back when
the hat arrives and I'll start task 4.1a immediately.
