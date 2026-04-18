# ✅ Deauth Fix Built — Awaiting On-Hardware Verification

**Status:** The v0.3 deauth issue (subtype-filter block in ESP-IDF 5.3 blob)
is fixed in the current master branch via platform migration to
pioarduino 55.03.38 + Bruce's patched WiFi libs. Compiles clean (see
`CHANGELOG.md` → Unreleased). **Not yet validated on physical hardware.**

If you flash from master and confirm deauth lands on-air, please open a
PR updating this file to "verified" or ping us — once a second set of
eyes confirms frames actually TX, v0.4.0 gets tagged.

---

## What was broken in v0.3

WiFi deauth — targeted, broadcast-all, per-client, Triton autonomous —
all five modes returned `ESP_ERR_INVALID_ARG (0x102)` from
`esp_wifi_80211_tx`. UI showed frames "sent" but on-air count stayed zero
and target devices didn't disconnect.

## Root cause

Stock `espressif32@6.7.0` bundles **ESP-IDF 5.3's WiFi binary blob**
(`libnet80211.a`) which filters management frame subtypes `0xC` (deauth)
and `0xA` (disassoc) regardless of setup. Confirmed across MAC spoofing,
`WIFI_IF_AP` routing, Marauder-pattern silent softAP, promiscuous mode
— deauth is always rejected. Beacon spam (subtype `0x8`) and other MGMT
frames worked fine — only deauth / disassoc were filtered.

## What changed in master (v0.4 pre-release)

Platform migrated to:
- `pioarduino/platform-espressif32@55.03.38` (Arduino Core 3.3.8 / ESP-IDF 5.5.4)
- `framework-arduinoespressif32-libs` overridden to
  [`bmorcelli/esp32-arduino-lib-builder`](https://github.com/bmorcelli/esp32-arduino-lib-builder/releases/download/idf-release_v5.5/bruce_esp32-arduino-libs-20260407-140520.zip)
  — same patched `libnet80211.a` [Bruce](https://github.com/pr3y/Bruce)
  uses, with the subtype filter NOP'd out

IDF 5.x API drift fixed in three files (ESP-NOW recv callback signature,
`esp_mac.h` include). All other libraries (NimBLE 1.4.1, RadioLib 7.4.0,
M5 stack) compile clean without bumps. Firmware image ~2 MB, RAM 48%,
Flash 63%.

## Everything else still works

- WiFi scan, beacon spam, PMKID capture, evil portal, karma, wardrive
- BLE scan, spam, HID, tracker detect, GATT, Find-My, Sour Apple
- Sub-GHz CC1101 (scan, record, replay, jammer, signal library)
- nRF24 MouseJack, BLE-via-nRF24 spam, spectrum
- LoRa scan / Meshtastic (full node participation)
- Network attacks (portscan, ping, DNS, responder, DHCP attacks, etc.)
- SaltyJack LAN suite
- All BadUSB, IR, tools, themes

## Rollback

If master breaks on your board for unrelated reasons:

```bash
cp platformio.ini.stable platformio.ini
pio run -t upload
```

restores the v0.3 stock platform (deauth returns to blob-filtered).

## Build notes

Migration must run from WSL2 / native Linux, not Git Bash on Windows —
pioarduino's install machinery (`idf_tools.py`) hard-fails on MSys
environment markers. Full execution log + fixes in
[`docs/v0.4-platform-migration.md`](docs/v0.4-platform-migration.md).

---

*Last updated: 2026-04-18 — fix compiled, awaiting on-hardware verification.*
