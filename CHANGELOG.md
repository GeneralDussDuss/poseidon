# Changelog

All notable changes to POSEIDON are documented here.
Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
versioning follows [SemVer](https://semver.org/spec/v2.0.0.html).

## [0.4.1] - 2026-04-20

### Added — WhisperPair probe (CVE-2025-36911)

New BLE feature under `w` in the BLE menu. Scans for Google Fast Pair
advertisements (service UUID `0xFE2C`), classifies each as DISCOVERABLE
(pairable, spec-compliant) or NON-DISCOVERABLE (in-use). On a selected
target, connects GATT → discovers the Key-Based Pairing characteristic
(`FE2C1234-8366-4814-8EB0-01DE32100BEA`) → writes a 2-stage probe.

**Stage 1 (any target):** real secp256r1 ephemeral keypair (via mbedTLS
hardware accel), sends 80-byte envelope = 16B ciphertext + 64B ephemeral
pubkey. Vulnerable firmware responds with a notify on the KBP char even
though the accessory isn't in pairing mode — a compliant provider must
silently drop the write. Result: `VULNERABLE` / `LIKELY PATCHED` /
`NO_FE2C_SVC` / `CONNECT_FAIL`.

**Stage 2 (when we have the accessory's anti-spoofing pubkey):** real
ECDH → `K = SHA-256(shared_secret.x)[0:16]` → AES-128-ECB decrypt the
response → lift the BR/EDR address from bytes 1..6. Pubkeys loaded at
runtime from `/poseidon/fastpair_keys.bin` (67 bytes per record: 3B
model ID BE + 64B raw X||Y pubkey). Lets the probe extract the
Classic-BT MAC normally hidden behind RPA until pairing mode.

Verdict + BR/EDR MAC (when extracted) logged to
`/poseidon/whisperpair.csv`.

**Module is probe-only.** The ESP32-S3 has no BR/EDR radio, so the full
attack (SSP bond + A2DP/HFP hijack + Find Hub registration) cannot
complete on-device. The CVE demonstration + extracted BR/EDR hand off
to external Classic-BT hardware. Built-in 15-device model-ID lookup
covers popular accessories (Pixel Buds, Sony XM4/XM5, Jabra, JBL,
Nothing, OnePlus, etc.); unknown IDs display as hex.

Credit: CVE-2025-36911 disclosed by [COSIC @ KU Leuven](https://www.esat.kuleuven.be/cosic/news/whisperpair-hijacking-bluetooth-accessories-using-google-fast-pair/)
(Preneel, Singelée, Antonijević, Duttagupta, Wyns) in January 2026,
$15K Google bounty. Reference PoCs:
[aalex954/whisperpair-poc-tool](https://github.com/aalex954/whisperpair-poc-tool),
[SpectrixDev/DIY_WhisperPair](https://github.com/SpectrixDev/DIY_WhisperPair).

**Authorized testing only** — probe devices you own. Disclosed CVE,
patches rolling out; this is a demonstration + self-check tool.

## [0.4.0] - 2026-04-19

### Fixed — deauth frames actually TX on-air

The ESP-IDF WiFi blob (`libnet80211.a`) contains a function
`ieee80211_raw_frame_sanity_check(int32_t, int32_t, int32_t)` that is
called from `esp_wifi_80211_tx` before every raw-frame send. It rejects
MGMT subtypes `0xC` (deauth) and `0xA` (disassoc) by returning non-zero,
causing TX to return `ESP_ERR_INVALID_ARG (258)` and logging
`wifi:unsupport frame type: 0c0/0a0`. That filter is the actual blocker —
not the interface, not the mode, not the silent-AP pattern.

Every previous fix attempt was at the wrong layer. Fun fact: the
"patched libs" from `bmorcelli/esp32-arduino-lib-builder` do **not**
actually patch this function. We verified the symbol is still strong
and the filter string is still in every Bruce-lib zip.

**The real fix** (credit: [GANESH-ICMC/esp32-deauther](https://github.com/GANESH-ICMC/esp32-deauther)):
link-time symbol override. Define
`ieee80211_raw_frame_sanity_check` in our own code returning 0, and add
`-Wl,--allow-multiple-definition` so the linker prefers our `.o` over
the library's `.a`. See `src/features/wifi_sanity_override.cpp` and the
linker flag in `platformio.ini`.

Verified on-device: autotest fired 800 deauth/disassoc frames at a
non-PMF WPA2 AP, result `ok=800 fail=0 rate=99%`. Target client
disconnected within seconds.

Fixes all five deauth paths: targeted (`wifi_deauth`), per-client
(`wifi_clients`), all-clients channel-hop (`wifi_clients_all`),
broadcast-all (`wifi_deauth_extras`), and Triton autonomous. No code
changes to those features — they were all correct, just blocked at the
blob.

### Fixed — NimBLE 2.x migration (13 files)

NimBLE-Arduino 1.4.1 crashes at BLE init on Core 3.3.8. Bumped to
`^2.3.9` (matching Bruce) and migrated every BLE feature file:

- `NimBLEAdvertisedDeviceCallbacks` → `NimBLEScanCallbacks`
- `onResult(*)` → `onResult(const *) override`
- `setAdvertisedDeviceCallbacks` → `setScanCallbacks`
- `scan->start(dur_sec, callback, is_continue)` → `scan->start(dur_ms, is_continue)` — unit change sec→ms, callback arg gone (UI polls `isScanning()`)
- `NimBLEAddress::getNative()` → `getBase()->val`
- `setAdvertisementType(MODE)` → `setConnectableMode(MODE)`
- `addData(std::string(...))` → `addData(uint8_t*, size_t)`
- Server/characteristic callbacks now take `NimBLEConnInfo&`
- `NimBLEHIDDevice::inputReport/manufacturer/hidInfo/...` → `getInputReport/setManufacturer/setHidInfo/...`
- `getServices()/getCharacteristics()` return vector by reference, not pointer
- `NimBLEDevice::getInitialized` → `isInitialized`

All 13 BLE feature files compile clean + BLE scan tested on-device
(enumerates devices over 6s, no reboot).

### Added — full platform migration

Platform moved to `pioarduino/platform-espressif32@55.03.38` (Arduino
Core 3.3.8 / ESP-IDF 5.5.4) with
`bruce_esp32-arduino-libs-20260123`. Migration was required for recent
NimBLE/RadioLib/lwIP versions — deauth was solved separately by the
symbol override above.

Toolchain fallout (fixed):
- `c5_cmd.cpp`, `mesh.cpp` — ESP-NOW recv callback signature changed in
  IDF 5.x from `(mac, data, len)` to `(recv_info, data, len)`. Wrapped
  in `#if ESP_IDF_VERSION_MAJOR >= 5` for dual-platform builds.
- `meshtastic_node.cpp` — `esp_read_mac` / `ESP_MAC_WIFI_STA` moved to
  `esp_mac.h`. Gated on the same macro.
- RMT legacy driver (`driver/rmt.h`) conflicts with IDF 5.5 `driver_ng`
  at boot — stubbed out in `subghz_record/replay/broadcast` pending a
  proper migration to the new `rmt_tx.h/rmt_rx.h` API. Those three
  features are no-ops in v0.4; scan/spectrum/brute force still work.

Rollback path: `cp platformio.ini.stable platformio.ini` + `git checkout
v0.3.0` — but you don't need it, v0.4 just works.

Build note: must run from WSL2 / native Linux, not Git Bash on Windows.
pioarduino's `idf_tools.py` hard-fails on MSys environment markers. See
`docs/v0.4-platform-migration.md`.

### Added — SaltyJack LAN attack suite (phase 1)

### Added — SaltyJack LAN attack suite (phase 1)

New top-level submenu (`j` from root) that ports LAN-side pentest attacks from
[@7h30th3r0n3](https://github.com/7h30th3r0n3)'s [Evil-M5Project](https://github.com/7h30th3r0n3/Evil-M5Project)
and [RaspyJack](https://github.com/7h30th3r0n3/Raspyjack). Named in homage.

- **SaltyJack ▸ About** — homage landing page, credits, arsenal overview
- **SaltyJack ▸ DHCP Starve** — floods the associated network's DHCP server
  with random-MAC Discover/Request cycles until the pool is exhausted. Live
  counters for discover / offer / request / ACK / NAK. Auto-detects pool
  exhaustion (NAK >= 20). Requires POSEIDON to be associated to the target
  WiFi first.

Phase-2 attacks coming in subsequent commits: Rogue DHCP (STA + AP modes),
Responder (LLMNR + NBT-NS + SMB1 NTLMv2), WPAD proxy NTLM harvest, on-device
NTLMv2 wordlist cracker. All direct ports of Evil-Cardputer source with
proper attribution in every file.

Hardware path: works now on WiFi STA mode (device joins target). Once the
user's W5500 SPI Ethernet module arrives, same attacks work over wired too
via lwIP's transport-agnostic `netif`.


## [0.3.0] - 2026-04-17

### Added — Meshtastic node (full participant)

POSEIDON is now a full Meshtastic leaf node on the default LongFast public channel,
not just a listener. Four new menu entries under LoRa:

- **Mesh Chat** (`c`) — live feed of received text messages, type to broadcast
- **Mesh Nodes** (`n`) — scrollable roster of seen nodes with short name, id, SNR,
  RSSI, hops, last-seen, GPS pin indicator. ENTER opens page screen for that node
- **Mesh Page** (`p`) — direct-message a specific node (paging)
- **Mesh Pos** (`g`) — toggle periodic Position broadcast; POSEIDON appears as a
  pin on other Meshtastic apps when GPS has a fix

Wire-level compatibility (byte-exact vs firmware v2.7.23):
- LoRa PHY: 906.875 MHz, SF11, BW250, CR4/5, preamble 16, sync 0x2B, CRC on
- 16-byte packet header (to/from/id/flags/channel/next_hop/relay_node) LE
- AES-CTR-128 with default LongFast PSK, counter block in bytes 12-15 of nonce
- Hand-rolled protobuf codec for Data, User, Position (no nanopb dep)
- Node ID derived from WiFi MAC matching `NodeDB::pickNewNodeNum`
- Packet ID = 10-bit counter | 22-bit random, non-zero guarantee

Leaf-only design:
- We receive and send but do not forward other nodes' packets
- No PKI (AES-CCM + Curve25519), MQTT, ACKs, telemetry, multi-channel

### Fixed — LoRa spectrum rewrite + freeze + ESC (2026-04-17)

The LoRa analyzer was freezing on a blank screen when a frequency was
selected, and ESC wasn't backing users out of bar/waterfall modes.

Root cause of the freeze: `run_bars` and `run_waterfall` were retuning
the SX1262 210+ times per frame (one per pixel column) via a full
standby → setFrequency → startReceive → RSSI dance. Any single retune
hanging on a BUSY wait froze the whole feature. Plus, the SX1262's RSSI
is wideband at the tuned frequency — you can't actually get per-bin
spectrum from a single reading, so the sweep was misleading anyway.

**Rewrite:**
- Tune once at band center, stay in continuous RX (no sweep per frame)
- Sample ambient RSSI per frame — non-blocking, ~1 ms
- X-axis reinterpreted as time instead of frequency (honest about what
  the signal actually shows)
- New packet-capture pipeline: `poll_packet()` pulls real LoRa frames
  as they arrive and buffers them in a 6-deep history ring (RSSI, SNR,
  size, age) — every view now shows detected packets as an overlay
- All three modes retained with clearer semantics:
  - **Bar Meter** — RSSI bars over time with peak hold + packet overlay
  - **Waterfall** — scrolling RSSI heatmap with packet overlay
  - **Oscilloscope** — single-frequency waveform, trimmable with +/-
- TAB cycles band and retunes once instead of sweep-per-frame
- Tight input polling (every 4 ms) in bars and waterfall so ESC/backtick
  catches quick taps — the slow-render + delay(15) pattern was dropping
  key presses below ~10 Hz

### Fixed — LoRa PI4IOE antenna switch + boot loop (2026-04-17)

LoRa feature was panic'ing the whole device with a `Guru Meditation
Error: LoadProhibited` on Core 1. Panic traced to `M5.getIOExpander(0)`
returning a reference to an unregistered slot, which then LoadProhibited
as soon as a method was called on it. Nothing in POSEIDON ever registers
an IOExpander with M5Unified.

The esp_restart() fallback in `lora_radio()` then put the device into
a hard boot loop cycling every few hundred ms — `rst:0x3 RTC_SW_SYS_RST`
forever, device unusable.

**Fix:**
- Replaced `M5.getIOExpander(0)` with direct I2C writes to the
  PI4IOE5V6408 at address 0x43 via `M5.In_I2C` — register 0x01 for
  direction, 0x05 for output, with a presence probe so missing hats
  log a warning instead of hanging
- `lora_radio()` now returns a dummy `SX1262` instance (disconnected
  pins, all RadioLib calls no-op or return error) instead of calling
  `esp_restart()` on null. Any code path that reaches for the radio
  before `lora_begin()` now fails gracefully with a log warning

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

### Fixed — LoRa crash on frequency select (2026-04-17)

Testers reported POSEIDON crashing the moment a frequency was picked in a
LoRa feature. Two bugs compounded:

- `lora_hw.cpp` was passing `cfg.bw_khz / 1000.0f` to RadioLib's `setBandwidth`,
  which expects **kHz** directly. The 125 kHz preset became `0.125` — not a
  valid SX1262 bandwidth — and the call silently errored out. The radio was
  then left in a half-configured state that crashed on the next retune.
- `lora_spectrum.cpp` `lora_read_rssi` retuned the SX1262 while it was still
  in RX mode from the previous sweep iteration. The BUSY line never deasserted
  and RadioLib eventually aborted mid-sweep.

**Fix:**
- Pass `cfg.bw_khz` unchanged (no divide-by-1000)
- Check return values on every post-`begin()` setter so future invalid config
  fails loud at init instead of crashing later
- Call `radio.standby()` before every `setFrequency` in the spectrum sweep
  and check the `startReceive` return value too

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
