# POSEIDON — Review & Action Items

**Repo:** https://github.com/GeneralDussDuss/poseidon
**Target:** M5Stack Cardputer (ESP32-S3), PlatformIO/Arduino, C/C++
**Reviewer:** Claude (web) — handing off to Claude Code for implementation

---

## Context for Claude Code

POSEIDON is a keyboard-first pentesting firmware for the M5Stack Cardputer. 44 features across WiFi/BLE/IR/USB-HID/Network/Mesh. Sibling project to DAVEY JONES. Part of a broader "phantom ecosystem" that also includes:

- **Banana Pi BPI-M4 Zero** — headless Linux brain
- **Nooelec NESDR Smart HF bundle** — wideband SDR receive
- **Evil Crow RF V2** — sub-GHz TX/RX (self-built from JLCPCB gerbers)
- **M5Stack Cardputer ADV** — handheld field tool (runs POSEIDON)

The firmware is in good shape. README is strong, attribution is exemplary, architecture is clean (one file per feature under `src/features/`), lazy radio domain switching is the right call for ESP32 RAM constraints. MIT licensed. 37 commits deep, no releases yet.

**Do not rewrite anything unprompted.** This is a task list, ordered by priority. Work through it top to bottom, one task at a time, commit after each. Ask the user before making architectural changes.

---

## Repo structure (for reference)

```
poseidon/
├── assets/raw/
├── c5_node/              # ESP32-C5 5GHz remote radio node (planned)
├── src/
│   ├── main.cpp
│   ├── app.h
│   ├── input.{h,cpp}
│   ├── ui.{h,cpp}
│   ├── menu.{h,cpp}
│   ├── radio.{h,cpp}     # lazy WiFi↔BLE↔idle switcher
│   ├── splash.cpp
│   ├── ble_db.{h,cpp}
│   ├── ble_types.h
│   ├── wifi_types.h
│   ├── gps.{h,cpp}
│   ├── mesh.{h,cpp}      # ESP-NOW presence
│   ├── dhcp_cache.{h,cpp}
│   └── features/
│       ├── wifi_*.cpp    (15 features)
│       ├── ble_*.cpp     (14 features)
│       ├── ir_*.cpp      (2 features)
│       ├── net_*.cpp     (6 features)
│       ├── triton.cpp    # RL handshake gotchi
│       ├── badusb.cpp
│       ├── tools.cpp
│       ├── system_tools.cpp
│       ├── mesh_status.cpp
│       ├── splash.cpp
│       └── stubs.cpp
├── tools/
├── .gitignore
├── README.md
└── platformio.ini
```

---

## Priority 1 — Ship-blockers for adoption

### Task 1.1 — Add firmware version string

**Why:** 37 commits, no version. Bug reports are unusable without knowing which build.

**Do:**
1. Add to `platformio.ini` build flags:
   ```ini
   build_flags =
     -DPOSEIDON_VERSION=\"0.1.0\"
     -DPOSEIDON_BUILD_DATE=\"__DATE__\"
   ```
2. Create `src/version.h` exposing `POSEIDON_VERSION` and a `const char* poseidon_version()` helper.
3. Show version on the About screen (`src/features/stubs.cpp` or wherever About lives).
4. Render version in the splash footer (`src/splash.cpp`) — small, bottom-right, dim magenta.
5. Print version + build date to serial on boot in `main.cpp`.

**Ask the user:** confirm starting version (`0.1.0` vs something else).

---

### Task 1.2 — GitHub Actions build CI

**Why:** Zero automated verification across 44 features and multiple radio stacks. Regressions will slip in.

**Do:**
1. Create `.github/workflows/build.yml`.
2. Install PlatformIO, cache `~/.platformio`, run `pio run`.
3. On tag push (`v*`), upload `.pio/build/*/firmware.bin`, `bootloader.bin`, `partitions.bin` as workflow artifacts.
4. Add a build status badge to the top of `README.md` next to the existing shields.

---

### Task 1.3 — Tag first release

**Why:** No precompiled binaries = users without PlatformIO locked out. M5Burner-compatible builds drive adoption.

**Do:**
1. After Task 1.1 and 1.2 land, `git tag v0.1.0 && git push --tags`.
2. CI builds artifacts automatically.
3. Create a GitHub Release with a short CHANGELOG excerpt (see Task 1.4), attach the `.bin` files.
4. Add an "Install (prebuilt)" section to README above the "Building" section with `esptool.py` flash commands.

**Stop after the tag push.** Ask the user to verify the release notes before publishing.

---

### Task 1.4 — CHANGELOG.md

**Why:** 37 commits, no history summary.

**Do:**
1. Create `CHANGELOG.md` following Keep a Changelog format.
2. Walk `git log --oneline master` and group commits into: Added / Changed / Fixed / Removed.
3. Put everything under `## [0.1.0] - YYYY-MM-DD` for the initial release.
4. Add an `## [Unreleased]` section at the top.

**Ask the user:** any commits they want to omit or re-label from the log.

---

## Priority 2 — Make it visible

### Task 2.1 — Screenshots & GIFs

### Task 2.2 — Link DAVEY JONES

## Priority 3 — Documentation gaps

### Task 3.1 — SD card schema doc
### Task 3.2 — DuckyScript-lite coverage doc
### Task 3.3 — Responder target environment note
### Task 3.4 — Regional RF compliance note
### Task 3.5 — Evil Portal template gallery

## Priority 4 — Ecosystem integration

### Task 4.1 — Sub-GHz remote radio node spec
### Task 4.2 — Kismet/PCAP output mode

## Priority 5 — Robustness

### Task 5.1 — Power management
### Task 5.2 — Watchdog / crash recovery audit
### Task 5.3 — Triton brain corruption handling

## Priority 6 — Nice to have

### Task 6.1 — Issue & PR templates
### Task 6.2 — SECURITY.md
### Task 6.3 — Host-side unit tests for parsers

---

## Working agreement

1. **One task per commit.** Commit message format: `task X.Y: <short description>`.
2. **Ask before architectural changes.** Priorities 4 and 5 have design docs first — do not skip to implementation.
3. **Do not touch attribution comments** in source files without asking. The user cares a lot about credit hygiene.
4. **Do not reformat existing code** as a side effect of making changes. Surgical edits only.
5. **Do not add new dependencies** without asking. ESP32 flash/RAM budget matters.
6. **Match the existing code style** — look at neighboring files before writing new code.
7. **Preserve the tone of README and docs.** Direct, confident, occasionally profane, credits generously.
8. **If unsure, stop and ask.**

---

## What NOT to do

- Don't rewrite `src/features/*.cpp` files for style.
- Don't add a plugin system or module registry.
- Don't add telemetry/analytics/phone-home.
- Don't add an OTA update system without explicit ask.
- Don't introduce a web UI.
- Don't suggest moving to Rust.
- Don't add a CONTRIBUTING.md with CLA/CoC boilerplate.
