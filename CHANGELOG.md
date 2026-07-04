# Changelog

All notable changes to POSEIDON are documented here.
Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
versioning follows [SemVer](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

(empty — new work since 0.6.4 lands here)

## [0.6.4] - 2026-07-03

### Added

- **KERBEROS — FIDO2 / U2F hardware security key.** Press `k` and the Cardputer
  becomes a real USB security key. The U2F (CTAP1) and CTAP2 protocol stacks are
  hand written on the ESP32-S3 — CBOR, COSE key encoding, ECDSA over NIST P256,
  attestation objects, authenticator data — with no off the shelf FIDO library.
  - **Passkeys**: non-resident (wrapped key handle) and resident / discoverable
    credentials stored in NVS. Usernameless sign in works.
  - **Verified end to end** against Yubico's `python-fido2` reference library:
    attestation and assertion signatures both check out, non-resident and
    resident.
  - Credential private keys are generated on device (mbedTLS) and never leave it.
  - On device presence approval before any signature.
  - Boot "key mode": entering KERBEROS reboots into a FIDO only USB personality
    so it is the sole HID device (the OS only recognises a lone FIDO interface);
    exiting reboots back to normal. Both modes keep the serial port for flashing.
  - Works today as a second factor on Google, GitHub, and any WebAuthn site.
  - Design, plan, and native + on device test suites live in the repo
    (`lib/kerberos_core`, `src/features/kerberos*`, `docs/specs`, `docs/plans`).

### Changed

- Vendored TinyCBOR v0.6.0 (`lib/tinycbor`) for host and device CBOR.

## [0.6.3] - 2026-06-13

### Fixed

- **Deauth now actually transmits.** `esp_wifi_80211_tx` was returning
  `ESP_ERR_NO_MEM` (257) under burst load — the lean 4/4 WiFi TX buffer pool
  filling up silently dropped ~half the frames (GitHub #7, the deauth /
  handshake-capture report). Added a NO_MEM retry so each frame resends until
  it lands. Covers Deauth, Deauth All, Clients, and Argus's hunt.
- **Spectrum analyzer flat-line fix.** Every mode swept frequency but never
  re-entered RX after `setMHZ`, so `getRssi()` read a stale flat value. Now
  re-strobes RX after each hop — real signal across all analyzer modes.
- **Argus overlay cleanup.** Removed the OLD_FURY lightning + ANNOYED glitch
  overlays that bled over his face and the status bar.

## [0.6.2] - 2026-06-11

### Added

- **Sub-GHz analyzer visual modes.** Four new modes on the CC1101 — peak-hold,
  radar PPI scope, digital-phosphor persistence, and a blip sonar — plus a
  full-screen waterfall that scales to available heap. Neon palette throughout.
- **Mass Storage mode.** Exposes the SD card over USB (TinyUSB MSC).
- **Gas Station + Drug Store** sub-GHz signal categories in the broadcast library.

### Changed

- **Argus** (the handshake-hunter gotchi) now hunts 5 GHz handshakes via the
  TRIDENT C5 satellite when one is online, and got a cursed-demigod character
  + lore pass.
- Full website + docs refresh: magenta cyberpunk re-skin, glitch accordions,
  two-lane README (newcomers + nerds), self-hosted Cardputer web-flash bin,
  six previously-uncredited devs added to the credits wall.

### Fixed

- **NimBLE heap-leak reboots** in Defensive Monitor + Drone Remote ID — a
  missing `setMaxResults(0)` let the scan accumulate advertisements until the
  device reset (~550 in). Callback-only now.
- **Defensive Monitor BLE-phase heap guard** — when the heap is too fragmented
  to re-init NimBLE, it stays WiFi-only for that cycle instead of crashing.
- Argus STORM-mode lightning overlay no longer paints over the status bar.
- GPS / LoRa audit fixes.

## [0.6.1] - 2026-05-29

### Added

- **Built-in sub-GHz signal library.** 40 baked OOK signals across the
  Cars / Pranks / Tesla / Home categories — no SD recordings required.
  Tesla Charge Port + Frunk, Princeton/CAME/NICE/Linear/Holtek/Multicode/
  Stanley/Liftmaster garage codes, doorbells (generic Chinese, Auchan/
  Lidl, talking-novelty, Honeywell + Heath/Zenith chimes), restaurant
  pagers (LRS + Genesys), keychain panic alarm, cricket chirp, TV-B-Gone
  RF beep, air horn, wireless outlet packs (5-pack A/B + Etekcity ON/OFF
  + master ALL ON/OFF), ceiling fan (LOW/MED/HIGH/OFF), smoke alarm
  test, window sensor alert, PIR motion tripped, glass break alert.
  Baked signals merge into their natural category alongside any user
  recordings dropped into `/poseidon/signals/<cat>/`; SD entries
  appear first, baked underneath prefixed with `*`.
- **ON-AIR TX indicator.** Big red badge with pulsing dot + frequency
  readout + play counter overlays the broadcast screen for the full
  duration of every CC1101 transmit. Red border slab around the body
  flips to green for 120 ms after TX completes so you can tell "frame
  stuck" from "TX actually fired."
- **Hydra RF Cap hardware prep** (lands before the physical hat
  arrives). Six pre-flight fixes audited against Bruce, bmorcelli's
  SmartRC fork, and PINGEQUA's official pin reference:
  - `nrf24_hw.cpp` now uses the HSPI bus (`sd_get_spi()`) instead of
    the global FSPI, which M5GFX owns for the display. Previously the
    TFT would tear / freeze on every nRF24 operation.
  - `cc1101_park_others()` calls `gps_end()` and reclaims pin 13 from
    the GPS UART driver before driving it as CC1101 CS.
  - `cc1101_begin()` now checks the `Init()` return value — half-init
    chips no longer slip through silently.
  - nRF24 sniffer ports Bruce's 6-pipe mousejack noise-address table
    (was a single 1-byte pipe that captured almost nothing).
  - `nrf_write_reg()` raw SPI moved to the HSPI bus too.
  - CC1101 RMT min pulse width 20 µs → 3 µs (Bruce's value, catches
    fast Manchester edges from 1200-baud transponders).

### Fixed

- nRF24 LoRa NSS pin parking — was floating during nRF24 sessions,
  could drive MISO if a stale LoRa init lingered.

### Notes

- Baked sub-GHz signals are **representative patterns from public
  references** — they TX cleanly through CC1101 but won't necessarily
  fire any specific real-world target. Use them as test signals and
  drop your recorded captures into `/poseidon/signals/<cat>/` on SD
  for verified payloads.
- Hydra cap regression sweep happens the moment the hardware lands.

## [0.6.0] - 2026-05-28

The biggest single drop since 0.5.0. Triton gets a real character
(96x96 Argus mood sprite), the WiFi stack moves to a Bruce-libs-safe
STA-mode raw-TX path that actually lands deauths on-air, BLE features
move from broken xTaskCreate to a cooperative tick pattern that
actually fires, the WiFi Portal gets a working raw-IDF AP recipe, the
IR LED gets its polarity corrected and the Samsung Smart Remote codes
get fact-checked against canonical Flipper-IRDB tables. Plus a dozen
new features and the C5 protocol bumps to v3 with a 5G-scan terminator
fix so the UI knows when a scan ended.

### Added

- **Argus mood sprite for Triton.** Twelve 96x96 RGB565 mood portraits
  (WATCHING / INTERESTED / PLEASED / ANNOYED / RESIGNED / CALCULATING
  / CYNICAL / OLD_FURY / SLEEPING / REFLECTIVE / CURIOUS / STERN)
  generated from a Gemini-rendered sprite sheet. Mood-mapped to
  Triton's hunting state — SLEEPING when idle, PLEASED on capture,
  OLD_FURY during FERAL mode, ANNOYED while dry, RESIGNED in despair.
  Sprite cached to internal SRAM (`heap_caps_malloc(MALLOC_CAP_INTERNAL)`)
  to avoid MMU cache stalls when `esp_wifi_80211_tx` pauses the flash
  bus mid-pushImage.
- **WiFi raw-TX path moved to STA mode** with linker-override of
  `ieee80211_raw_frame_sanity_check`. Bruce's pinned `libnet80211.a`
  filters subtype 0xC/0xA (deauth/disassoc) regardless of TX path,
  but a multi-def `-Wl,-zmuldefs` override returning 0 from our own
  stub gets every subtype through. Replaces the softAP-mode TX path
  that crashed in `ieee80211_hostap_attach +0x2c` null-deref on this
  lib stack.
- **BLE cooperative tick pattern** across Sour Apple, Spam, Karma,
  Flood, FindMy, Finder. Replaces `xTaskCreate(task, 4096, ...)` that
  silently failed `rc=-1` because NimBLE init eats heap down to
  ~2.5 KB on Bruce libs — not enough for a 4 KB task stack. New
  pattern: feature exposes `feature_tick(void)` called from its UI
  loop every iteration. Sour Apple now fires, BLE Spam shows up on
  every nearby phone, Find-My broadcasts pass iCloud relay.
- **WiFi Portal raw-IDF AP recipe.** Stash of softAP-mode (which
  crashes on Bruce libs), now uses
  `esp_bt_controller_mem_release(BTDM)` →
  `esp_netif_create_default_wifi_ap` → shrunk-buf `esp_wifi_init` →
  `esp_wifi_set_config(WIFI_IF_AP)` → `esp_wifi_start` → post-start
  `esp_wifi_set_channel`. Plus a pre-AP teardown so Portal works
  after Triton already inited WiFi STA mode. Sixteen phishing
  templates (Apple, Office365, LinkedIn, Amazon, Netflix, Instagram,
  Hotel, Starbucks, Airport, Router Admin, Zoom, SSO + four others).
- **IR LED active-HIGH polarity fixed.** Cardputer-Adv IR LED is wired
  anode to 3V3, cathode through resistor to GPIO 44. GPIO HIGH = LED
  ON. Earlier diagnostic mis-read picked "GPIO 44 inverted" which
  was matching the wrong premise — GPIO 44 is U0TXD on ESP32-S3 and
  floats HIGH at boot from boot-console pulses. Polarity now LOW =
  OFF; every IR feature parks LOW on exit + a 50 ms background
  watchdog forces LOW when no IR feature is active.
- **Samsung Smart Remote codes verified** against Flipper-IRDB BN59
  remotes + probonopd/irdb device 7,7. Fixed Power 0x40→0x02
  (canonical toggle), removed bogus Smart Hub 0x9E, added Exit 0x2D.
  All 38 remaining codes confirmed correct (volume, channel, D-pad,
  OK, color buttons, media transport, full number pad).
- **AP Signal Test** diagnostic — POSEIDON-SIGTEST AP on ch 1/6/11,
  raw-IDF recipe, verifies the AP path is actually broadcasting from
  the chip. The "is the AP path even working today" probe.
- **Evil Twin** — 5s deauth burst → 25s portal/SSID-clone, time-sliced.
- **BLE BlueDucky** — BLE HID keystroke injection (CVE-2023-45866)
  with Android DuckyScript payloads. *Known limit:* hangs on
  Cardputer-Adv without PSRAM — NimBLE eats too much heap for the
  GATT server allocation. Documented hardware ceiling.
- **SATCOM Tracker** — live satellite tracking (SGP4 + baked TLEs for
  ISS, Tiangong, Hubble, NOAA APT birds, ham SO-50/AO-7, BlueWalker 3
  etc.). Polar skyplot, 24h pass predictor, AOS chirp on next pass.
- **Drone Remote ID** — ASTM F3411 decoder for all six message types
  (Basic, Location, Auth, Self-ID, System, Operator). Decodes every
  DJI / Skydio / Autel craft broadcasting in your sky.
- **Surveillance Hunter** — Flock Safety ALPR + Raven/ShotSpotter
  passive RF detector. Embedded signature DB (Tier-1/Tier-2 OUI
  prefixes, SSID patterns including the CVE-2025-59409 dev SSID,
  XUNTONG manufacturer ID 0x09C8). GPS-tagged hits, .pcap export.
- **Defensive Monitor** — passive multi-class anomaly detector for
  your local airspace.
- **Screensaver Picker** — UI picker for the seven new screensavers
  (sonar / port scan / hex cascade / terminal crack / neural arc /
  glitch BSOD / tide waves), keyed off the active theme.
- **nRF52 family** — four features riding an Adafruit Feather nRF52840
  Bluefruit hat: scan, scout-strike, BLE MITM relay, WiFi+BLE combo.
- **Carousel menu style** — big-card single-focus layout with
  cyberpunk pictograph icons. Toggle at System -> Menu style.
- **Three theme-appropriate idle screensavers** — kick in at 2 min
  idle, pulled from a pool keyed off the active theme.
- **Theme system overhaul.** Six themes now ship (POSEIDON, MATRIX,
  E-INK, SYNTHWAVE, PHANTOM, BLOOD). Magenta splashes, beefed-up
  matrix rain, ambient procedural motion under every menu draw.
- **`ui_ambient_tick`** — theme-aware procedural ambient motion
  painted behind every menu draw. Live preview at
  System -> Ambient Preview.
- **BLE scan vendor labelling** wired in GhostBLE's curated 70-entry
  manufacturer-ID + 30-entry service-UUID tables. Generic "BLE"
  entries now resolve to Sony / Garmin / Govee / DJI / Tile / Anker /
  Wyze / Roku / Fitbit / Eddystone / Google Fast Pair / Drone Remote
  ID.
- **ESP32-C5 web flasher** at
  `https://generaldussduss.github.io/poseidon/flash/`. Single-page
  install via `esp-web-tools@10.2.1` on jsdelivr CDN. POSEIDON's S3
  flasher still pending — POSEIDON ships via M5Burner today.
- **C5 protocol v3 + 5G scan terminator.** Wire protocol bumped v2→v3
  (flag day — TRIDENT C5 firmware must be rebuilt to match). Adds 9
  new command opcodes (CLIENTS_HUNT, CLIENTS_AP, BEACON_SPAM,
  PROBE_SNIFF, DEAUTH_DETECT, KARMA, APCLONE, SPECTRUM, CIW) and 4
  new response opcodes. Critical fix: C5 now sends a zero-payload
  terminator `RESP_AP` so POSEIDON's UI knows when a scan finished
  instead of spinning forever waiting for results that already came.
- **Deauth autotest harness** + WiFi scan refresh logic improvements.

### Changed

- **Triton hop_task is now a cooperative tick.** Was 4 KB FreeRTOS
  task that silently `pdFAIL`'d on the internal-SRAM ceiling. New
  path iterates from the main UI loop every iteration — Bruce,
  Marauder, Evil-Cardputer, Porkchop all do this and POSEIDON was
  the outlier. Inter-burst delay bumped 5 → 25 ms so the
  dynamic-TX-buffer pool drains between calls.
- **Linker flag** swapped from `--allow-multiple-definition` to
  `-Wl,-zmuldefs`. The previous flag was order-dependent on .o file
  scan order — adding any new feature could silently flip which
  copy of `ieee80211_raw_frame_sanity_check` won the link. Bruce,
  Marauder, Porkchop all use `-zmuldefs` for the same reason.
- **`radio.cpp` simplified.** `radio_switch(RADIO_WIFI)` is now a
  no-op state flag — features bring up WiFi themselves with the
  per-mode recipe that fits their needs. NimBLE init centralized in
  `radio_switch(RADIO_BLE)` so BLE features don't double-init.
- **Full Samsung Smart Remote** keymap under `IR -> Remote` — ~40
  buttons, no modifier keys, every code verified against Flipper-IRDB.
- **WiFi Scan** raw-IDF path replaces Arduino's `WiFi.scanNetworks`
  which OOM'd at default 32-buffer init on this device's fragmented
  DMA RAM. New path finds 10+ APs reliably (was 1).

### Fixed

- **Triton exit crash.** `c5_begin()` called `WiFi.mode(WIFI_STA)`
  based on Arduino's tracked state (which was OFF because Triton
  inited WiFi via raw IDF). That double-created the default STA
  netif and asserted. Now uses `esp_wifi_get_mode` for true state
  check.
- **IR LED stuck on at boot** — first-instruction park in `setup()`
  plus background watchdog hammering pin LOW every 50 ms.
- **WiFi scan finding only 1 AP** — Arduino's default-buf scan path
  starved DMA RAM. Raw-IDF replacement reliably finds 13+.
- **Sour Apple / BLE Spam silent failures** — root caused to
  `xTaskCreate` rc=-1 (NimBLE heap exhaust). Fixed by cooperative
  tick refactor (see Added).
- **BLE static-random MAC byte** — was setting flag bits in `mac[0]`
  (incorrect); now `mac[5] |= 0xC0`. Sour Apple, FindMy, Karma,
  Flood, Spam all affected.
- **PSRAM allocation paths removed** — Cardputer-Adv unit ships
  with broken PSRAM (ID reads 0xffffff). NimBLE explicitly forced
  to internal-RAM allocation; no code path assumes PSRAM exists.

### Removed

- **OTA Update flow.** Was buggy and rarely used; the M5Burner +
  USB / web-flasher paths cover the same ground without the
  partition complexity. Source files moved to `*.disabled` for a
  future rebuild under a build flag with proper heap-budget gates.

### Security notes

POSEIDON is a pentesting tool. It is authorized for use on networks and
devices you own or have written permission to test. See README "Legal"
section.

(history below…)

- **WiFi Beacon Spam — raw-IDF recipe.** The Arduino path crashed in
  `ieee80211_hostap_attach` (+0x2c null deref) on Bruce's pinned
  `esp32-arduino-libs`. New path bypasses Arduino entirely:
  `esp_bt_controller_mem_release(BTDM)` →
  `esp_netif_init` + `esp_netif_create_default_wifi_sta` →
  `esp_wifi_init` with shrunk dynamic TX/RX bufs (16/16) → STA mode +
  promiscuous + raw 802.11 TX on `WIFI_IF_STA`. Sustained ~1200
  frames/sec, channels 1-11 rotate once per full SSID-list pass.
- **Beacon Spam scanner UI.** KITT-style sweeping header, live
  "now broadcasting" SSID in 2x font, fading 5-line recent feed,
  fps + total + live channel readout, hex stream at bottom.
- **IR LED hardware diagnostic** at `IR → LED Test`. Walks
  GPIO 44 / 9 / 41 × normal/inverted carrier polarities. Point a
  phone camera at the LED, note which step lights it up — nails
  down pin + polarity for any Cardputer board variant.
- **Full Samsung Smart Remote keymap** under `IR → Remote`. ~40
  buttons: power (toggle + discrete on/off), source, menu, tools,
  info, guide, smart-hub, mute, volume `u/d`, channel `c/v`, full
  D-pad on the `;./,//` cluster, OK on ENTER, four color buttons,
  media transport, number pad. No modifier keys required.
- **BLE scan vendor labelling** — wired in GhostBLE's curated 70-entry
  manufacturer-ID + 30-entry service-UUID tables (`sigdb_bt.h`).
  Devices that previously showed as generic "BLE" now resolve to
  Sony / Garmin / Govee / DJI / Tile / Anker / Wyze / Roku / Fitbit
  / Eddystone / Google Fast Pair / Drone Remote ID, etc.
- **ESP32-C5 web flasher** at
  `https://generaldussduss.github.io/poseidon/flash/`. Single-page
  install button via `esp-web-tools@10.2.1` on the jsdelivr CDN.
- **Carousel menu style** — big-card single-focus layout with
  cyberpunk pictograph icons. Toggle at `System → Menu style`.
- **Three theme-appropriate idle screensavers** — kick in at 2 min
  idle, pulled from a pool keyed off the active theme.
- **Theme system overhaul** — three themes, magenta splashes,
  beefed-up matrix rain. Pick at `System → Theme`.
- **`ui_ambient_tick`** — theme-aware procedural ambient motion
  painted behind every menu draw. Live preview at
  `System → Ambient Preview`.
- **Deauth autotest harness** + WiFi scan refresh logic improvements.

### Changed

- **IR LED carrier — bit-banged in CPU instead of LEDC.** The
  Cardputer-Adv's IR LED is active-LOW on GPIO 44 (anode to 3V3,
  cathode through resistor to pin). LEDC's `output_invert` flag
  inverts active PWM transitions but NOT the idle level — so
  `duty=0` between marks let the pin idle LOW and the LED stayed
  solidly on. Replaced LEDC with direct `digitalWrite` toggling at
  13 µs half-period (≈38.5 kHz, within IR receiver tolerance).
- **Samsung remote protocol corrected.** Was sending NEC byte
  layout (`addr, ~addr, cmd, ~cmd`); Samsung32 actually wants
  `addr, addr, cmd, ~cmd` with the same 4500/4500 µs header.
- **CC1101 driver** swapped from `lsatan/SmartRC-CC1101-Driver-Lib`
  to `bmorcelli/SmartRC-CC1101-Driver-Lib` fork — upstream
  bit-bangs MISO in `Reset()` which hangs on Arduino-ESP32 v3.x.
- **Sub-GHz spectrum + waveform features** refactored to share UI
  widgets, tuned demodulator.

### Fixed

- **IR Remote, Clone, and TV-B-Gone all silently no-op'd.** GPIO 44
  is UART0 RX by default and `pinMode(OUTPUT)` doesn't fully detach
  peripheral inputs from the GPIO matrix in Arduino-ESP32 v3. Added
  `gpio_reset_pin((gpio_num_t)44)` before LEDC/GPIO setup.
- **TV-B-Gone backlight blank.** Was using LEDC channel 0 / timer 0
  which is exactly what M5GFX uses for the display backlight —
  reconfiguring the timer to 38 kHz blanked the screen. Moved to
  channel 3 / timer 3.
- **Carousel text strobing** on idle frames.
- **WiFi/BLE deauth robustness** + BLE init lifecycle deferral
  so the controller boot race is gone.
- **Triton mid-session `sd_remount`** that deadlocked HSPI — removed.
- **Web flasher 404.** `docs/flash/` was untracked locally so
  `/flash/` returned 404 on Pages. Now committed with `!docs/flash/bin/**/*.bin`
  + `!docs/bin/**/*.bin` negation rules in `.gitignore` so the
  binaries actually ship.
- **Web flasher CDN.** `docs/flash/index.html` was on `unpkg.com`
  which served an edge-cached old 10.0-era bundle with no ESP32-C5
  support. Same `jsdelivr@10.2.1` fix as `docs/install.html` in
  commit 307f6b5 is now applied here too.

### Documentation

- ESP32-C5 web flasher live page deployed.
- Visual + immersion overhaul design spec + implementation plan
  under `docs/proposals/`.
- Ambient motion + memory optimization implementation plans.

## [0.5.0] - 2026-04-21

### Added — TRIDENT (ESP32-C5 companion satellite over ESP-NOW)

The Cardputer-Adv's ESP32-S3 is 2.4 GHz-only. TRIDENT is an ESP32-C5
plug-in node that pairs automatically with POSEIDON on channel 1 and
gives us the RF territory the S3 physically can't touch:

- **5 GHz deauth** (targeted + broadcast). `esp_wifi_80211_tx()` on
  IDF 5.5 is blocked by `ieee80211_raw_frame_sanity_check` inside
  `libnet80211.a` for management subtypes 0xA/0xC. Worked around via
  a direct binary patch: the function's body is rewritten to
  `li a0, 0; ret` (RISC-V), re-inserted into `libnet80211.a` with
  `objcopy --update-section` + `ar r`. Linker `--wrap` and
  `--allow-multiple-definition` both failed because the caller and
  definition live in the same object file — only a binary patch
  works.
- **802.15.4 Zigbee sniffer** — channel hop 11-26, pcap-compatible
  dump over ESP-NOW.
- **5 GHz PMKID capture** — ESP32-C5 dual-band scans every
  country-allowed channel, association-requests AP, captures M1,
  streams PMKID back to the Cardputer.
- **Full-band WiFi scan** — scanner reports 2.4 + 5 GHz APs with
  auth type (WPA3 / WPA3-EAP / WPA3-EXT shown in red as a
  PMF-mandatory hint, since deauth against those silently does
  nothing).

POSEI v2 protocol — lightweight binary framing over ESP-NOW:
HELLO / SCAN req+resp / DEAUTH / ZIGBEE-SNIFF / PMKID-CAP / STATUS.
Re-entry + duplicate-command guards on the C5 side (ESP-NOW
sometimes delivers the same frame twice; `s_scan_running` +
`s_last_scan_seq` flags now ignore the second copy instead of
racing two scan tasks). Channel-pin to ch 1 after every scan /
attack so the C5 never drifts off the discovery channel.

TRIDENT web flasher — ESP32-C5 isn't in M5Burner's supported chip
list yet, so the install page has a direct WebSerial flash button
(esp-web-tools 10.2.0+ pinned; 10.2.0 is the first release with C5
support via esptool-js 0.5.6). Reads `trident-factory.bin` from the
release assets. Manual esptool + full ESP-IDF build-from-source
paths also documented.

### Added — M5 Launcher integration

New `[env:cardputer-launcher]` PlatformIO env produces a POSEIDON
build linked at `ota_0` / `0x170000` using bmorcelli's canonical
8 MB partition table (`support_files/launcher_8Mb.csv`). Drop the
resulting `firmware.bin` onto SD or upload via bmorcelli's WebUI
and POSEIDON slots into the Launcher's app list with no further
config.

Settings → `[L] back to Launcher` surfaces a "unplug USB to return"
hint — bmorcelli's custom bootloader decides which partition to
run from the RTC reset-reason (POWERON → Launcher, SW → app), so
there's no software way to force a swap from inside an installed
app. Better to say so up-front than have the menu item do nothing.

### Fixed — LoRa end-to-end on CAP-LoRa1262

The SX1262 radio was broken in several compounding ways. After
re-porting the init against M5Stack's own reference example and
d4rkmen/plai's Cardputer-Adv Meshtastic port:

- Pass **BUSY (GPIO 6) to RadioLib's `Module()`** — was
  `RADIOLIB_NC`. Without it RadioLib had nothing to wait on between
  SPI commands, so `startReceive()` would stall silently.
- **Do not create a second `SPIClass`**. Our `SPIClass(FSPI)` was
  the display's SPI2 peripheral. `loraSpi.begin(40,39,14,-1)`
  hijacked the bus from M5GFX and every draw after that froze the
  CPU mid-SPI-transaction. Fixed by sharing the SD helper's HSPI
  (SPI3) — same physical pins 40/39/14, different CS, different
  peripheral, M5GFX untouched.
- **Antenna switch enable before `radio.begin()`** — not after.
  PI4IOE5V6408 P0 HIGH first, then chip init.
- **`setCurrentLimit(140)`** per M5's reference.
- **SD logging inside the scan loop removed**. After `lora_begin()`
  the pin matrix is owned by the radio — any SD write contends for
  pins 40/39/14 and freezes the CPU.
- **`sd_mount()` guard at top of `lora_begin`** so
  `sd_get_spi()` is guaranteed to have been `begin()`-d before we
  hand the `SPIClass&` to RadioLib.

Scan / beacon / Meshtastic / spectrum all now work on 433 / 868 /
915 / US LongFast. Auth display on the WiFi scan list uses the same
red-flag treatment as the C5 scanner (WPA3 / PMF-mandatory modes
highlighted).

### Fixed — flicker across every live / scan screen

Every live screen was redrawing the full body on a 250-400 ms tick
even when nothing changed. Eliminated:

- `ui_draw_status` caches `(radio, extra, heap_bucket)` — only
  repaints the gradient + text when something visibly changed.
  Heap is quantised to 4 KB buckets so minor churn doesn't force
  a redraw. The pulse dot animates independently in its own 5×5
  rect. `ui_force_clear_body` invalidates the cache.
- `feat_wifi_scan`, `feat_ble_scan`, `feat_wifi_clients` loops now
  gate `draw_list()` on `(count, cursor, scanning)` changing —
  not on a timer. Key handlers update state only; the state-change
  gate owns drawing.
- `feat_wifi_spectrum` rewritten to paint static chrome once and
  only redraw bars whose RSSI actually changed. The pulse ring
  erases the old position and draws the new one on channel change
  instead of clearing the whole body every 80 ms.
- `c5_scan::draw_status_header` caches the peer count and the
  status-dot colour; no-op when nothing changed.

### Added — release tooling

- `scripts/prepare_release.sh` packages three artifacts into
  `dist/<version>/` with exact filenames the webflasher manifests
  expect: `poseidon-factory.bin` (S3 factory image),
  `poseidon-launcher.bin` (app-only for bmorcelli's ota_0),
  `trident-factory.bin` (C5 merged image). The web flashers pull
  from `releases/latest/download/<name>` so a fresh tag
  automatically repoints the site.
- `scripts/wsl_sync_and_build.sh` copies back both PIO envs
  (`cardputer` + `cardputer-launcher`).
- `scripts/c5_tail.py` auto-reconnects on USB-CDC resets so the
  serial monitor survives a flash.

## [0.4.3] - 2026-04-20

### Fixed — Sub-GHz record / replay / broadcast actually transmit

All three sub-GHz features were stubbed on the v0.4 platform migration
because the legacy `driver/rmt.h` API linked against IDF 5.5's
`driver_ng` aborted the chip at boot. The UI + file parsers + waveform
preview were fully built but the actual RMT call was a `(void)raw;
(void)len;` no-op. Record toasted `"record unavailable v0.4"`. Replay
and broadcast silently transmitted nothing.

**Migrated to the new `rmt_tx.h` / `rmt_rx.h` API.** New helper module
`src/cc1101_rmt.{h,cpp}` owns channel + encoder lifecycle around the
CC1101 GDO0 pin. Two public calls:

- `cc1101_rmt_tx(pulses, n)` — transmit a pulse train. Converts the
  `int16_t` signed-microsecond format (pos=high, neg=low) to
  `rmt_symbol_word_t[]` pairs, uses the built-in copy encoder, blocks
  on `rmt_tx_wait_all_done`.
- `cc1101_rmt_rx(out, max, timeout_ms, gap_us)` — capture pulses into
  a buffer. 1 MHz resolution (1 µs tick, matches Flipper `.sub` file
  encoding 1:1). ISR-to-task notification on capture complete.

Wired into all three features. Record now actually samples the CC1101
demod output for 20 seconds (10ms silence = end-of-signal). Replay and
broadcast drive GDO0 via RMT with the precise timing the `.sub`
format requires — no more bit-bang approximation.

Boot verified clean on ESP32-S3: no RMT conflict. The three features
that appeared broken since v0.4.0 now actually work.

### Changed — release assets

Dropped the three ancillary bins (`-app.bin`, `-bootloader.bin`,
`-partitions.bin`). v0.4.3 and onward ship only `poseidon-v0.4.3-factory.bin`
which is the only one anyone actually needs. M5Burner's custom repo
points at factory.bin. esptool users want factory.bin. PlatformIO
users build from source. Cleaner release page, no user confusion.

## [0.4.2] - 2026-04-20

### Added — three-tier polish pass (15 items)

Tier 1 (QoL wins):
- WiFi scan: **S=save** dumps filtered results to `/poseidon/wifiscan-<ts>.csv` (ssid/bssid/channel/rssi/auth).
- BLE scan: **S=save** dumps filtered results to `/poseidon/blescan-<ts>.csv` (mac/type/name/rssi).
- BLE scan: **W=whisper** in the detail popup jumps straight into WhisperPair on the selected device; skips the rescan phase.
- MouseJack: **T=type** now prompts for a custom payload via `input_line()` instead of hardcoding `cmd /c echo pwned`. The only real TODO in the codebase — closed.
- Portal: new credentials fire a two-tone 1800Hz/2400Hz beep + `CRED CAPTURED <user>` WAVES overlay via a deferred flag (HTTP response stays snappy). Also serial-log for live tail.
- Deleted `src/deauth_autotest.cpp` and its `#ifdef POSEIDON_AUTO_DEAUTH_TEST` call site in `main.cpp` — served its purpose during v0.4 debug; the real fix is shipped in `wifi_sanity_override.cpp`.

Tier 2 (architectural):
- **Wardrive AP table now public** (`src/wifi_wardrive.h`). `g_wdr_aps[256]` + `g_wdr_ap_count` survive across feature exits — the session's richest BSSID→SSID→auth dictionary is now queryable by any WiFi-centric feature.
- **Triton seeds from wardrive on entry**: closes the "first 30s of hashcat lines have blank ESSID" gap. Serial logs the seed count.
- **wifi_pmkid seeds from wardrive too**: handshakes captured right after a wardrive session now carry the real ESSID field from frame zero.
- **Sub-GHz last-capture global** (`src/subghz_types.h`): `g_subghz_last_cap` (pulses + count + freq + ts) populated by `subghz_scan` after every successful decode. Infrastructure for future "play the last thing you caught" workflows that skip the SD round-trip.
- **nRF24 last-device global** (`src/nrf24_types.h`): `g_nrf24_last_device` set when MouseJack targets from the sniffer list. Readied for HUNT-bundle / TRIDENT handoff.
- **In-feature `?` help**: `menu.cpp` exports `g_current_feature_item` + `ui_show_current_help()`. Press `?` inside a feature to render its own long-form info paragraph without ESCing back to the menu. Wired into wifi_scan / ble_scan / ble_whisperpair / subghz_jam_detect as the starter set; other features can add the binding with one line.

Tier 3 (strategic):
- **TRIDENT `status` command**: desktop companion can now query `{"cmd":"status"}` to get fw version, active radio, free heap, wardrive AP count without screen-scraping.
- **TRIDENT `loot` command**: `{"cmd":"loot","which":"creds|ntlm|responder|whisperpair|wigle"}` streams SD logs as JSON-line events (`loot_begin` / `loot_row` / `loot_end`) — live credential relay to the desktop without pulling the SD card.
- **Mesh `!poseidon ping` / `!poseidon status`**: LoRa Meshtastic messages prefixed with `!poseidon ` are parsed in the RX path and respond with broadcast text. Turns the mesh into a C2 channel for field-deployed nodes.
- **New feature: `Radio → Sub-GHz → Jam Detect`**: RSSI-anomaly monitor. 10-second baseline warmup, then alerts on sustained `baseline + 15 dBm` spikes — the signature of a jammer or sustained TX in-band. Red overlay + two-tone siren + CSV log to `/poseidon/jamdetect.csv`. Peer to the WiFi deauth-detect feature.
- **New menu: `Tools → Hunt`**: bundles all four hot/cold RSSI locators (BLE tracker hunt + passive tracker scan + sub-GHz finder + 2.4 GHz finder) under one submenu. Operator picks "what am I hunting?" first instead of digging through per-radio menus.

### Fixed — theme persistence

- `theme_picker` was calling `theme_set()` inside a 6-iteration render loop every frame (~300 NVS writes/sec while browsing), which made the persisted theme a near-random final intermediate value. New `theme_preview(id)` is RAM-only; `theme_set()` is called once on ENTER commit.
- `theme_set()` no longer elides based on `s_current` — preview may have shifted it, so write-through on commit is mandatory.

### Fixed — WhisperPair scan UUID match

- Was constructing the 128-bit long form of `0xFE2C` to filter Fast Pair service data; NimBLE's internal equality across the 16/128-bit boundary was missing every hit. Now uses `NimBLEUUID((uint16_t)0xFE2C)` and also iterates all service-data entries as belt-and-braces. Scan duty cycle bumped (interval 100, window 99). Added `R=rescan` + empty-state adv-counter so the user can tell the scan is alive before a Fast Pair device happens to beacon.

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
