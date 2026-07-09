# POSEIDON Heap Stability Program — Design

Date: 2026-07-09
Target: M5Stack Cardputer-Adv (ESP32-S3, no usable PSRAM, ~320 KB internal DRAM)
Status: approved design, ready for implementation plan

## Problem

Launching the WiFi Evil-Portal clone crashes with a `+0x2c` null deref inside
`esp_wifi_start` -> `ieee80211_hostap_attach`. On-device serial traces from
2026-07-08 proved the cause is **internal heap exhaustion**, not the portal
code:

| State at portal entry | Free internal heap |
|---|---|
| Fresh boot | ~48 KB |
| After cycling features (Triton, BLE scan, ARGUS, spectrum) | ~7.8 KB |

Two facts define the problem:

1. **Static baseline is tight.** ~215 KB of internal DRAM is static (`.data` +
   `.bss`) across ~1,986 symbols, leaving only ~100 KB for the entire heap.
   The large splash/sprite/OUI blobs are correctly flash-resident (`0x3c…`), so
   a broad static rewrite is low ROI. But a handful of large app-owned `.bss`
   buffers are always resident even when their feature is idle.
2. **Features do not return heap on exit.** The ~40 KB gap between a fresh boot
   and a cycled session is heap that features acquire and never give back. The
   confirmed persistent holder is ARGUS `s_ram_sprite` (18 KB DMA, allocated at
   `argus.cpp:148`, freed nowhere).

The portal needs roughly 40 KB of contiguous internal heap **once**, at AP
bring-up. So the correct model is cooperative reclamation plus leak elimination,
not permanently reserving more RAM.

## Goals

- Portal (and every RF feature) starts reliably even after heavy feature
  cycling, or fails clean with a toast instead of crashing.
- Every feature returns to its heap baseline on exit; regressions are visible
  on serial and named by feature.
- Recover meaningful permanent headroom from idle-resident static buffers.
- All of the above measurable on-device and testable on the dev host.

## Non-goals

- Rewriting the ~1,960 small static symbols (death by a thousand cuts, low ROI).
- Touching TinyUSB / framework-owned endpoint buffers (`ncm_epbuf`,
  `mscd_epbuf`, `_transfer_buf`).
- Re-enabling PSRAM (broken on this unit; ID reads back 0xffffff).

## Architecture

A single new unit, `src/heap_budget.{h,cpp}`, provides the engine. Everything
else is a small integration into existing seams. Each part is independently
useful and independently testable.

### Part A — heap_budget engine

```c
// Reclaim registry: subsystems yield recoverable caches (refillable from flash)
typedef void (*heap_reclaim_fn)(void);
void   heap_reclaim_register(heap_reclaim_fn fn);
size_t heap_reclaim_all(void);              // runs all registered fns; returns bytes recovered

// Instrumentation
void   heap_report(const char *tag);        // "free=.. largest=.. min_ever=.." one line
size_t heap_free_internal(void);            // MALLOC_CAP_INTERNAL free
size_t heap_largest_internal(void);         // MALLOC_CAP_INTERNAL largest contiguous block
size_t heap_min_ever_internal(void);        // lifetime low-water mark
void   heap_census(void);                   // full per-region dump (boot + Settings screen)

// RF pre-flight gate: reclaim, report, and veto if the largest block can't fit.
// Returns true if OK to proceed; false means caller must bail (toast already shown).
bool   rf_preflight(const char *tag, size_t need_bytes);
```

Design notes:
- The registry is a fixed-size static array of function pointers (no dynamic
  allocation in the heap-management code itself). Cap ~16 entries.
- `heap_min_ever_internal()` is updated on every `heap_report` and every
  `rf_preflight` call, plus an optional periodic sampler in the main loop, so
  transient starvation between feature runs is captured.
- `rf_preflight` is the single choke point for RF features. `need_bytes` is the
  contiguous requirement, not total free, because `hostap_attach` needs one
  block. Default portal value derived from the observed failure (~12 KB) with
  margin; tuned once real post-shrink numbers are read from serial.

### Part B — cooperative reclaim participants

- `argus.cpp`: add `heap_argus_release()` that frees `s_ram_sprite`, sets it to
  `nullptr`, and resets `s_ram_sprite_tried = false`. Register it via
  `heap_reclaim_register` at first draw (or module init). On the next mascot
  draw it reallocates if heap allows, else falls back to the direct-from-flash
  `pushImage` path already wired at `argus.cpp:153`. The mascot degrades
  gracefully under pressure; nothing is permanently sacrificed.
- Any other reclaimable cache found during implementation registers the same
  way. Live state (mesh `s_nodes`/`s_msgs`, subghz `s_raw`) is NOT reclaimable
  and is out of scope for the registry; those already free on exit.

### Part C — blanket leak instrumentation (2 edits, 150 features)

Features are dispatched from exactly two seams, both already bracketed with
`[FEAT_ENTER]` / `[FEAT_EXIT]` serial logs keyed by `sel->label`:
`src/menu.cpp:1358` and `src/menu.cpp:1395`.

At each seam:
1. Before `action()`: call `heap_reclaim_all()`, then capture
   `size_t base = heap_free_internal();`
2. After `action()`: log `sel->label`, `base`, current free, and the net delta.
   A feature returning below `base` (beyond a small slack) is flagged `LEAK`.

This instruments all 150 features from one place and auto-names any leaker on
serial. No per-feature edits required.

### Part D — static footprint diet (lazy allocation)

Convert the largest **app-owned** always-resident `.bss` buffers to
alloc-on-feature-entry / free-on-exit. Candidates ranked by size (from ELF
symbol census, `0x3fc…` region):

| Symbol | Size | Owner | Action |
|---|---|---|---|
| `g_wdr_aps` | 20 KB | wardrive | lazy alloc on wardrive entry, free on exit |
| `s_capq` | 8 KB | WiFi capture | lazy alloc on capture entry, free on exit |
| `feat_wifi_ciwv active` | 6.9 KB | wifi feature | lazy alloc / free |
| `feat_usb_guard hits` | 5 KB | usb_guard | lazy alloc / free |
| `s_hits` | 4.7 KB | (tbd during impl) | lazy alloc / free |

Realistic permanent recovery: ~40 to 60 KB. Each conversion must handle alloc
failure gracefully (feature shows a toast and returns, never derefs null) and
free on every exit path. USB/TinyUSB buffers are excluded.

### Part E — RF bring-up hardening

- Keep the WiFi buffer-pool shrink already in `wifi_portal.cpp`
  (`dynamic_tx 16->6`, `cache_tx 4->2`, `dynamic_rx 16->8`); it saves ~29 KB at
  `esp_wifi_init`.
- Route `run_portal` and the other RF features (beacon spam, deauth, karma,
  pmkid, evil-twin, apclone) through `rf_preflight` at entry.
- Keep the 10 KB last-resort guard before `esp_wifi_start` as defense in depth.

### Part F — verification

- **Host unit test** in `env:native-test`: registry register/run/return-bytes
  behavior and the min-ever watermark math, mocking the heap primitives. Runs on
  the dev machine, no board attached.
- **On-device Settings screen**: "Heap census" showing free / largest / min-ever
  live, plus a manual "Reclaim now" action.
- **Hardware loop** (once a COM port is back): flash, cycle Triton, BLE scan,
  ARGUS, spectrum, wardrive; confirm each feature returns to baseline
  (`[FEAT_EXIT]` delta ~0) and portal entry free stays near 48 KB after cycling,
  and the clone AP starts instead of crashing.

## Data flow

```
boot ─> heap_census()                       (baseline logged)
menu dispatch ─> heap_reclaim_all()          (ARGUS yields 18 KB, etc.)
              ─> base = heap_free_internal()
              ─> feat_*()                     (lazy-allocs its own buffers)
              ─> log delta vs base            (LEAK if below baseline)
RF feature ─> rf_preflight(tag, need)         (reclaim + veto if too fragmented)
           ─> esp_wifi_init(shrunk pools)
           ─> 10 KB guard ─> esp_wifi_start
return home ─> ARGUS reallocates sprite or falls back to flash push
```

## Error handling

- Reclaim callbacks must be idempotent and null-safe (safe to call when the
  cache is already freed).
- `rf_preflight` failure path shows a toast and returns false; caller bails
  without touching the radio.
- Every lazy-alloc site checks for null and returns cleanly with a toast.
- The heap-budget code performs no dynamic allocation itself (fixed arrays).

## Risks

- Freeing the ARGUS sprite mid-session adds one flash reload on return home
  (visible as at most one slightly slower frame). Acceptable.
- Lazy-alloc conversions add alloc-failure paths to previously-infallible
  features; each must be verified on device.
- `rf_preflight` thresholds are estimates until real post-shrink serial numbers
  are read; treat the first hardware run as calibration.

## Files touched

- new: `src/heap_budget.h`, `src/heap_budget.cpp`
- new: `test/` native unit test for the registry/watermark
- `src/menu.cpp` — two dispatch seams (1358, 1395)
- `src/argus.cpp` — `heap_argus_release()` + registration
- `src/features/wifi_portal.cpp` — route through `rf_preflight`, keep shrink
- other RF features — `rf_preflight` at entry
- wardrive / capture / cache owners — lazy alloc conversions
- Settings menu — heap census screen
