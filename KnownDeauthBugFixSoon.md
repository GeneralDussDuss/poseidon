# ⚠️ Known Deauth Issue — Fix Coming in v0.4 ⚠️

**TL;DR:** Deauth frames currently don't land on-air in v0.3. Every other feature works. Fix is queued for v0.4.

---

## What's broken

WiFi deauth on POSEIDON v0.3 — targeted, broadcast-all, per-client, and Triton autonomous modes all hit the same issue. The Cardputer shows frames being "sent" in the UI but `drops:` climbs rapidly while `sent:` stays at 0. Target devices don't disconnect.

## Why

Stock `espressif32@6.7.0` bundles **ESP-IDF 5.3's WiFi binary blob**, which filters management frame subtypes `0xC` (deauth) and `0xA` (disassoc) regardless of setup. Confirmed by direct testing — MAC spoofing, `WIFI_IF_AP` routing, Marauder-pattern silent softAP, all still return `ESP_ERR_INVALID_ARG (0x102)` for deauth frames specifically. Beacon spam (subtype `0x8`) and other MGMT frames work fine — only deauth/disassoc are filtered.

## The fix

Migration to `pioarduino/platform-espressif32@55.03.36` (ESP-IDF 5.5) with [bmorcelli's patched WiFi libs](https://github.com/bmorcelli/esp32-arduino-lib-builder/releases) — the same stack Bruce uses. Patched `libnet80211.a` has the subtype filter NOP'd.

Previous migration attempt was blocked by Windows + Git Bash + MSys toolchain install issues. Next attempt will be from WSL2 / Linux where those issues don't exist.

Full migration plan lives at [`docs/v0.4-platform-migration.md`](docs/v0.4-platform-migration.md).

## What still works in v0.3

Everything else. The code-level deauth correctness fixes (addr1, disassoc pair, seq increment, client sniffer, PMF warning) are all in place — they'll start working the moment the blob is patched.

- WiFi scan, beacon spam, PMKID capture, evil portal, karma, wardrive
- BLE scan, spam, HID, tracker detect, GATT, Find-My, Sour Apple
- Sub-GHz CC1101 (scan, record, replay, jammer, signal library)
- nRF24 MouseJack, BLE-via-nRF24 spam, spectrum
- LoRa scan / Meshtastic (full node participation)
- Network attacks (portscan, ping, DNS, responder, DHCP attacks, etc.)
- SaltyJack LAN suite
- All BadUSB, IR, tools, themes

## Workarounds

1. **ESP32-C5 companion node** with Marauder-compatible firmware — POSEIDON offloads deauth over ESP-NOW. Works today regardless of v0.3 limitation
2. Wait for v0.4 platform migration

## Don't file this as a bug

It's a known blob-level behavior on stock ESP-IDF 5.3, not a POSEIDON code bug. If you want to help accelerate the fix, the migration plan in `docs/v0.4-platform-migration.md` is ready to execute from WSL or native Linux.

---

*Last updated: 2026-04-18 during v0.3 testing*
