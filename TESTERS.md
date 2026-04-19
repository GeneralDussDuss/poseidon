# For Testers

If you're helping stress-test POSEIDON, this doc tells you **what just changed**,
**what to hit hardest**, and **how to report what you find**.

**Currently testing:** v0.4.0 shipped — deauth lands on-air, full
SaltyJack LAN arsenal, NimBLE 2.x, pioarduino + IDF 5.5.4 platform. We
want regression breadth now: hit every feature, find the one thing that
slipped through.

---

## Priority 1 — Regression sweep

Every feature from v0.3 should still work, plus the six new SaltyJack
modules. Work through the menu leaf-by-leaf:

- **WiFi:** Scan, Clients (both variants), Deauth, Deauth All, Deauth
  Detector, AP Clone, Evil Portal, Karma, Beacon Spam, Probe Sniff,
  PMKID, 2.4 Spectrum, Wardrive (with GPS hat), CIW Zeroclick, Connect
- **BLE:** Scan, Spam, Bad-KB HID, Tracker Detect, Sniffer, iBeacon,
  Clone, GATT, Flood, Karma, Sour Apple, Find My Emulator, Toys
- **Sub-GHz:** Scan, Replay, Spectrum, Brute Force, Jammer, Broadcast
  Library, Finder — NOTE: Record/Replay/Broadcast TX are stubbed in
  v0.4 pending RMT driver_ng migration. They'll show a toast and exit
  cleanly. Scan/Spectrum/Finder/Brute still work.
- **nRF24:** Sniffer, MouseJack, BLE spam, Spectrum, Jammer, Finder
- **LoRa:** Scan, Beacon, Meshtastic Listener, Analyzer, Mesh Chat,
  Nodes, Page, Pos, GPS Fix
- **SaltyJack:** DHCP Starve, Rogue DHCP (STA+AP), Responder, WPAD,
  NTLMv2 cracker
- **Everything else:** Triton, TRIDENT PC bridge, MIMIR control, IR
  (TV-B-Gone + Samsung), BadUSB, theme picker, SFX settings, file
  browser, SD format, net attacks

If anything crashes, reboots the device, or doesn't work as it did in
v0.3.0, **file an issue with the menu path + serial log + how to
reproduce**.

## Priority 2 — Deauth stress test

Deauth frames now actually land (see [CHANGELOG](CHANGELOG.md) for
the link-time symbol override story). But we need real-world range
numbers:

1. Non-PMF WPA2 AP you own
2. WiFi → Scan → target → press `D`
3. `frames:` should climb rapidly, `drops:` stays at 0
4. Target phone/laptop disconnects in 2-5 sec
5. Report: router model, client model + OS, did it kick?

Also verify **Deauth All** (kicks every AP in range) and **PMKID** (hunt
mode now works too since it uses the same TX path — should capture
4-way handshakes on reconnect).

## Priority 3 — BLE across the whole stack

NimBLE 2.x migration touched 13 files. Scan is confirmed working. Run
through the rest:

- Bad-KB HID against a phone — pair, type a string, works?
- Tracker detect — leave running near an AirTag/Tile, does it pick up?
- Sour Apple against an iPhone — does the target see the popup storm?
- GATT explorer against your own device — enumerate services, read a
  characteristic
- Clone / Spam / Flood — all should at least start without crash

Report any BLE feature that still reboots the device.

**Known limits (unchanged):**
- WPA3-PSK, WPA2-Enterprise, WPA3-transition use PMF (Protected
  Management Frames). Deauth is cryptographically dropped — protocol
  defense, not a POSEIDON limit. UI warns.

## Priority 2 — Meshtastic node participation (new feature)

**What's new.** POSEIDON can now send and receive on the public LongFast
channel, show up in other people's Meshtastic apps, and page specific
nodes. Four new menu entries under LoRa: **Mesh Chat** (c), **Mesh Nodes** (n),
**Mesh Page** (p), **Mesh Pos** (g).

### Basic interop test (start here)

1. POSEIDON → LoRa → **Mesh Nodes**
2. Leave it running for ~2 minutes near an area with Meshtastic activity
   (mesh coverage varies wildly by city; check meshmap.net for your area)
3. **Does the roster populate?** If yes → RX works → protocol is correct
4. **Do your friend's / neighbor's / the local mesh's nodes show up by
   their real names?** If yes → NodeInfo decoding is working

### Send test

1. POSEIDON → LoRa → **Mesh Chat**
2. Press `T` to start typing, type a short message, `Enter` to broadcast
3. Check another Meshtastic device (phone app connected to a real node, or
   a second Meshtastic device) — does your message show up?
4. The message should appear *from* POSEIDON's node ID: `!xxxxxxxx` — verify
   it matches what POSEIDON shows as its own ID

### Paging test (direct message)

1. From **Mesh Nodes**, highlight one of your own devices and press Enter
2. Type a short DM, send
3. Your device should receive it addressed specifically to it, not as a
   broadcast

### Position reporting test

1. POSEIDON → LoRa → **Mesh Pos** → press `T` to toggle reporting on
2. Wait for GPS fix (needs CAP-LoRa1262 hat attached + sky view)
3. Press `B` to force a position send immediately
4. Other Meshtastic apps should show POSEIDON as a pin on their map

**What to report:**
- RX works but TX doesn't, or vice versa (points to encryption or header bug)
- Node roster shows garbage names / node IDs (protobuf decode issue)
- Any node that refuses to ACK a direct message (common for non-router nodes
  — note the destination's role)
- Position showing up at wrong lat/lon (sfixed32 sign bug)

**Known caveats:**
- **Leaf-only** — POSEIDON does NOT forward other nodes' packets. If there
  are nodes behind another node's relay, we won't reach them directly
- **No CAD before TX** — we transmit immediately without checking if the
  channel is busy. This collides with in-flight packets slightly more than
  a compliant node, especially in dense mesh areas. Will be added
- **NodeInfo broadcasts every 30 min regardless of the Mesh Pos toggle** —
  that toggle only gates Position, not NodeInfo. If you need to be fully
  silent, don't enter mesh features at all

## Priority 3 — LoRa stability (regression check)

**What was broken.** Before v0.3.0, picking a frequency in any LoRa feature
hard-restarted the device (boot loop from `M5.getIOExpander(0)` LoadProhibited).
The spectrum analyzer froze on a blank screen because it retuned the SX1262
210+ times per frame. Backtick didn't back out of bar/waterfall modes.

**How to test:**
1. Open every LoRa feature (Scan, Beacon, Mesh LF, Analyzer, all four mesh
   features) and confirm they all enter without crash / reboot
2. Analyzer: pick each of Bar Meter / Waterfall / Oscilloscope; each should
   render within a second and backtick should back out immediately
3. Analyzer: if any real LoRa traffic is nearby, the "RX N" overlay should
   show detected packets with RSSI/SNR/age
4. Switch bands with TAB — radio should retune without crashing

**What to report:**
- Any Guru Meditation panic (grab the serial log — `pio device monitor`)
- Any LoRa feature that still freezes or loops
- Frames not being detected on a band where you know traffic exists

## Priority 4 — Everything else (regression sanity)

Everything that worked in v0.2.0 should still work in v0.3.0. Quick smoke test:

- [ ] Sub-GHz scan / record / replay / spectrum / jammer
- [ ] BLE scan / spam / sniffer
- [ ] nRF24 MouseJack / BLE spam
- [ ] WiFi scan / PMKID capture / evil portal / wardrive
- [ ] BadUSB
- [ ] IR (TV-B-Gone / Samsung)
- [ ] GPS fix
- [ ] Theme picker (6 palettes)
- [ ] SD card features (save / load / format)
- [ ] TRIDENT screen mirror over USB-CDC

If anything worked in v0.2.0 and doesn't now → **file as a regression**, include
the feature name and what specifically broke.

## How to report

### Ideal bug report
- **Build**: v0.3.0 (confirm in `System → About` or the splash screen)
- **Hardware**: Cardputer-Adv + which hat attached
- **Feature**: exact menu path (`WiFi → Deauth → specific target`)
- **What happened**: screen behavior, serial log if available
- **What you expected**: e.g. "phone should have disconnected"
- **Reproducibility**: always / sometimes / once
- **Serial log** if possible: `pio device monitor --port COM? --baud 115200`

### Where to file
- **GitHub Issues**: https://github.com/GeneralDussDuss/poseidon/issues
- Use the label `v0.3.0-tester` so they're easy to triage

## What's NOT ready to test (don't file as bugs)

- **v0.4 platform fork / ESP-IDF WiFi injection patch** — stock blob
  still filters some spoofed-addr2 frames, so some deauth targets are
  stubborn even with the correct addr1 fix. Not a bug in POSEIDON
- **C5 remote nodes** — partial scaffolding only, full integration is v0.4
- **MIMIR server side** — the client module exists on POSEIDON but the
  BPI-M4 Zero server is still placeholder; v0.5 work

## Thanks

If you're testing, you're the reason v0.3.0 shipped correctly. The addr1
deauth bug was found because a tester said "deauths are weaker than
Marauder." Same for LoRa — a crash report is what sent us digging into
the bandwidth unit bug. Keep breaking things.
