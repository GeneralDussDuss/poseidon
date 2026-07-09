# Heap Stability Program Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the WiFi portal and every RF feature start reliably under heap pressure by adding a cooperative heap-reclaim engine, blanket per-feature leak instrumentation, and a static lazy-alloc diet.

**Architecture:** A new `heap_budget` unit provides a reclaim registry, heap instrumentation with a lifetime low-water mark, and an `rf_preflight` gate. The mascot and lazy-allocated feature buffers register as reclaimable. The two menu dispatch seams instrument all 150 features at once. The engine's logic is host-testable via an injectable heap-query seam.

**Tech Stack:** C++17, ESP-IDF 5.5 heap_caps API, Arduino-ESP32 3.3.8 (pioarduino), PlatformIO Unity for host tests.

## Global Constraints

- Target: M5Stack Cardputer-Adv, ESP32-S3, no usable PSRAM. Internal DRAM only (`MALLOC_CAP_INTERNAL`).
- The heap-budget code performs NO dynamic allocation itself (fixed static arrays only).
- Reclaim callbacks must be idempotent and null-safe.
- Do not touch TinyUSB/framework buffers (`ncm_epbuf`, `mscd_epbuf`, `_transfer_buf`).
- Build: `pio run -e cardputer`. The toolchain bin dir must be on PATH: prepend `%USERPROFILE%\.platformio\packages\xtensa-esp-elf\bin` before invoking pio (the platform does not inject it in this environment).
- Host tests: `pio test -e native-test`. ESP-only includes in `heap_budget.cpp` MUST be guarded with `#if !defined(PIO_UNIT_TESTING)`.
- Copy/UI text: avoid hyphens and dashes (project style).
- Flash: `pio run -e cardputer -t upload --upload-port COM16` (device currently absent; hardware tasks wait for a COM port).

---

## File Structure

- `src/heap_budget.h` — public API (registry, instrumentation, preflight).
- `src/heap_budget.cpp` — engine. Pure logic + an injectable query seam. ESP heap calls behind `PIO_UNIT_TESTING` guard.
- `test/test_heap_budget/test_main.cpp` — Unity host test, `#include "../../src/heap_budget.cpp"` with a mock query.
- `src/menu.cpp` — 2 dispatch seams (1358, 1395) gain reclaim + delta logging.
- `src/argus.cpp` — `heap_argus_release()` + registration.
- `src/features/wifi_portal.cpp` — route through `rf_preflight`; keep the buffer shrink.
- RF features (beacon spam, deauth, karma, pmkid, evil-twin, apclone) — `rf_preflight` at entry.
- `src/features/wifi_wardrive.cpp`, capture/cache owners — lazy-alloc conversions.
- Settings menu — heap census screen.

---

## Task 1: heap_budget instrumentation core (host-testable)

**Files:**
- Create: `src/heap_budget.h`
- Create: `src/heap_budget.cpp`
- Test: `test/test_heap_budget/test_main.cpp`

**Interfaces:**
- Produces:
  - `void hb_set_query(size_t (*free_fn)(void), size_t (*largest_fn)(void));` (test seam)
  - `size_t heap_free_internal(void);`
  - `size_t heap_largest_internal(void);`
  - `size_t heap_min_ever_internal(void);`
  - `void heap_report(const char *tag);`

- [ ] **Step 1: Write the failing test**

`test/test_heap_budget/test_main.cpp`:
```cpp
#include <unity.h>
#include <stdint.h>
#include <stddef.h>
#include "../../src/heap_budget.cpp"

static size_t g_fake_free    = 100000;
static size_t g_fake_largest = 40000;
static size_t fake_free(void)    { return g_fake_free; }
static size_t fake_largest(void) { return g_fake_largest; }

void setUp(void)    { hb_test_reset(); hb_set_query(fake_free, fake_largest); }
void tearDown(void) {}

void test_free_and_largest_passthrough(void) {
    g_fake_free = 55000; g_fake_largest = 22000;
    TEST_ASSERT_EQUAL_UINT32(55000, heap_free_internal());
    TEST_ASSERT_EQUAL_UINT32(22000, heap_largest_internal());
}

void test_min_ever_tracks_lowest_free(void) {
    g_fake_free = 80000; heap_free_internal();
    g_fake_free = 12000; heap_free_internal();
    g_fake_free = 90000; heap_free_internal();
    TEST_ASSERT_EQUAL_UINT32(12000, heap_min_ever_internal());
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_free_and_largest_passthrough);
    RUN_TEST(test_min_ever_tracks_lowest_free);
    return UNITY_END();
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `pio test -e native-test -f test_heap_budget`
Expected: FAIL to compile ("heap_budget.cpp: No such file").

- [ ] **Step 3: Write minimal implementation**

`src/heap_budget.h`:
```cpp
#pragma once
#include <stddef.h>

// Test seam: inject heap-query functions on host. On device the defaults
// (heap_caps_*) are installed by hb_install_esp_query() at boot.
void   hb_set_query(size_t (*free_fn)(void), size_t (*largest_fn)(void));
void   hb_test_reset(void);   // reset min-ever + registry (tests only)

size_t heap_free_internal(void);
size_t heap_largest_internal(void);
size_t heap_min_ever_internal(void);
void   heap_report(const char *tag);
```

`src/heap_budget.cpp`:
```cpp
#include "heap_budget.h"

#if !defined(PIO_UNIT_TESTING)
#include <Arduino.h>
#include <esp_heap_caps.h>
static size_t esp_free(void)    { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL); }
static size_t esp_largest(void) { return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL); }
#endif

static size_t (*s_free_fn)(void)    = 0;
static size_t (*s_largest_fn)(void) = 0;
static size_t s_min_ever = (size_t)-1;

void hb_set_query(size_t (*free_fn)(void), size_t (*largest_fn)(void)) {
    s_free_fn = free_fn; s_largest_fn = largest_fn;
}

size_t heap_free_internal(void) {
    size_t f = s_free_fn ? s_free_fn() : 0;
    if (f < s_min_ever) s_min_ever = f;
    return f;
}
size_t heap_largest_internal(void) { return s_largest_fn ? s_largest_fn() : 0; }
size_t heap_min_ever_internal(void) { return s_min_ever == (size_t)-1 ? 0 : s_min_ever; }

void hb_test_reset(void) {
    s_min_ever = (size_t)-1;
    // registry reset added in Task 2
}

void heap_report(const char *tag) {
#if !defined(PIO_UNIT_TESTING)
    Serial.printf("[heap] %s free=%u largest=%u min_ever=%u\n",
                  tag, (unsigned)heap_free_internal(),
                  (unsigned)heap_largest_internal(),
                  (unsigned)heap_min_ever_internal());
#else
    (void)tag;
#endif
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `pio test -e native-test -f test_heap_budget`
Expected: PASS (2 tests).

- [ ] **Step 5: Commit**

```bash
git add src/heap_budget.h src/heap_budget.cpp test/test_heap_budget/test_main.cpp
git commit -m "feat(heap): budget core with injectable query + min-ever watermark"
```

---

## Task 2: reclaim registry

**Files:**
- Modify: `src/heap_budget.h`
- Modify: `src/heap_budget.cpp`
- Test: `test/test_heap_budget/test_main.cpp`

**Interfaces:**
- Consumes: Task 1 query seam.
- Produces:
  - `typedef void (*heap_reclaim_fn)(void);`
  - `void heap_reclaim_register(heap_reclaim_fn fn);`
  - `size_t heap_reclaim_all(void);` (returns bytes freed = free_after - free_before, clamped to 0)

- [ ] **Step 1: Write the failing test**

Append to `test/test_heap_budget/test_main.cpp` (and add both to `main`):
```cpp
static int g_reclaim_calls = 0;
static void reclaimer_frees_10k(void) { g_fake_free += 10000; g_reclaim_calls++; }

void test_registry_runs_all_and_reports_recovered(void) {
    g_fake_free = 20000; g_reclaim_calls = 0;
    heap_reclaim_register(reclaimer_frees_10k);
    heap_reclaim_register(reclaimer_frees_10k);
    size_t recovered = heap_reclaim_all();
    TEST_ASSERT_EQUAL_INT(2, g_reclaim_calls);
    TEST_ASSERT_EQUAL_UINT32(20000, recovered);
}

void test_reclaim_all_empty_is_zero(void) {
    g_fake_free = 30000;
    TEST_ASSERT_EQUAL_UINT32(0, heap_reclaim_all());
}
```
Add `RUN_TEST(test_registry_runs_all_and_reports_recovered);` and `RUN_TEST(test_reclaim_all_empty_is_zero);` to `main`. Note: `hb_test_reset()` in `setUp` must clear the registry so suites do not bleed.

- [ ] **Step 2: Run test to verify it fails**

Run: `pio test -e native-test -f test_heap_budget`
Expected: FAIL ("heap_reclaim_register not declared").

- [ ] **Step 3: Write minimal implementation**

Add to `src/heap_budget.h`:
```cpp
typedef void (*heap_reclaim_fn)(void);
void   heap_reclaim_register(heap_reclaim_fn fn);
size_t heap_reclaim_all(void);
```

Add to `src/heap_budget.cpp` (above `hb_test_reset`):
```cpp
#define HB_MAX_RECLAIMERS 16
static heap_reclaim_fn s_reclaimers[HB_MAX_RECLAIMERS];
static int s_reclaimer_n = 0;

void heap_reclaim_register(heap_reclaim_fn fn) {
    if (!fn) return;
    for (int i = 0; i < s_reclaimer_n; ++i) if (s_reclaimers[i] == fn) return; // idempotent
    if (s_reclaimer_n < HB_MAX_RECLAIMERS) s_reclaimers[s_reclaimer_n++] = fn;
}

size_t heap_reclaim_all(void) {
    size_t before = s_free_fn ? s_free_fn() : 0;
    for (int i = 0; i < s_reclaimer_n; ++i) s_reclaimers[i]();
    size_t after = s_free_fn ? s_free_fn() : 0;
    return after > before ? after - before : 0;
}
```
Update `hb_test_reset()` body to also `s_reclaimer_n = 0;`.

- [ ] **Step 4: Run test to verify it passes**

Run: `pio test -e native-test -f test_heap_budget`
Expected: PASS (4 tests).

- [ ] **Step 5: Commit**

```bash
git add src/heap_budget.h src/heap_budget.cpp test/test_heap_budget/test_main.cpp
git commit -m "feat(heap): cooperative reclaim registry"
```

---

## Task 3: rf_preflight gate

**Files:**
- Modify: `src/heap_budget.h`
- Modify: `src/heap_budget.cpp`
- Test: `test/test_heap_budget/test_main.cpp`

**Interfaces:**
- Consumes: Tasks 1 and 2.
- Produces: `bool rf_preflight(const char *tag, size_t need_bytes);`
  - Runs `heap_reclaim_all()`, then returns `heap_largest_internal() >= need_bytes`.
  - On device (not host) shows a toast when returning false; the pure logic returns the bool for host testing.

- [ ] **Step 1: Write the failing test**

Append (and register in `main`):
```cpp
void test_preflight_true_when_largest_fits_after_reclaim(void) {
    g_fake_free = 8000; g_fake_largest = 8000;
    heap_reclaim_register(reclaimer_frees_10k); // bumps free; largest stays 8000 here
    g_fake_largest = 15000;                     // simulate reclaim coalescing a big block
    TEST_ASSERT_TRUE(rf_preflight("test", 12000));
}
void test_preflight_false_when_largest_too_small(void) {
    g_fake_largest = 6000;
    TEST_ASSERT_FALSE(rf_preflight("test", 12000));
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `pio test -e native-test -f test_heap_budget`
Expected: FAIL ("rf_preflight not declared").

- [ ] **Step 3: Write minimal implementation**

Add to `src/heap_budget.h`:
```cpp
bool rf_preflight(const char *tag, size_t need_bytes);
```

Add to the ESP-only include block at the top of `src/heap_budget.cpp` (inside the existing `#if !defined(PIO_UNIT_TESTING)`):
```cpp
#include "ui.h"      // ui_toast(const char*, uint16_t color, uint32_t ms)
#include "theme.h"   // T_BAD == theme().bad (runtime themed color)
```
Add to `src/heap_budget.cpp`:
```cpp
bool rf_preflight(const char *tag, size_t need_bytes) {
    heap_reclaim_all();
    heap_report(tag);
    size_t largest = heap_largest_internal();
    if (largest < need_bytes) {
#if !defined(PIO_UNIT_TESTING)
        ui_toast("Low memory: reboot then retry", T_BAD, 2200);
#endif
        return false;
    }
    return true;
}
```
Note: `ui_toast` is `void ui_toast(const char *msg, uint16_t color, uint32_t ms)` (`src/ui.h:34`); `T_BAD` is `#define T_BAD (theme().bad)` (`src/theme.h:64`). The host test never compiles this branch (guarded), so no UI stub is needed.

- [ ] **Step 4: Run test to verify it passes**

Run: `pio test -e native-test -f test_heap_budget`
Expected: PASS (6 tests).

- [ ] **Step 5: Commit**

```bash
git add src/heap_budget.h src/heap_budget.cpp test/test_heap_budget/test_main.cpp
git commit -m "feat(heap): rf_preflight reclaim-and-veto gate"
```

---

## Task 4: install ESP query + boot census

**Files:**
- Modify: `src/heap_budget.h`
- Modify: `src/heap_budget.cpp`
- Modify: `src/main.cpp` (or the file with `setup()`; grep for `void setup(`)

**Interfaces:**
- Consumes: Task 1.
- Produces: `void hb_install_esp_query(void);` and `void heap_census(void);`

- [ ] **Step 1: Add device query install + census (no host test; device-only glue)**

Add to `src/heap_budget.h`:
```cpp
void hb_install_esp_query(void);
void heap_census(void);
```
Add to `src/heap_budget.cpp` (inside `#if !defined(PIO_UNIT_TESTING)` region for the body):
```cpp
void hb_install_esp_query(void) { hb_set_query(esp_free, esp_largest); }
void heap_census(void) {
#if !defined(PIO_UNIT_TESTING)
    Serial.printf("[heap] census internal free=%u largest=%u  8bit free=%u  dma free=%u\n",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA));
#endif
}
```
For host builds add a no-op `void hb_install_esp_query(void){}` and `void heap_census(void){}` under `#else`.

- [ ] **Step 2: Wire into setup()**

In `setup()`, after `Serial.begin`, add:
```cpp
    hb_install_esp_query();
    heap_census();
```
Add `#include "heap_budget.h"` at the top of that file.

- [ ] **Step 3: Build to verify it compiles**

Run: `pio run -e cardputer` (with toolchain on PATH per Global Constraints)
Expected: SUCCESS.

- [ ] **Step 4: Host regression**

Run: `pio test -e native-test -f test_heap_budget`
Expected: PASS (still 6).

- [ ] **Step 5: Commit**

```bash
git add src/heap_budget.h src/heap_budget.cpp src/main.cpp
git commit -m "feat(heap): install esp heap query + boot census"
```

---

## Task 5: ARGUS cooperative reclaim

**Files:**
- Modify: `src/argus.cpp` (holder at line 45; alloc at line 148)

**Interfaces:**
- Consumes: Task 2 (`heap_reclaim_register`).
- Produces: `void heap_argus_release(void);` (file-local, registered once).

- [ ] **Step 1: Add release + registration**

Add `#include "heap_budget.h"` near the top of `src/argus.cpp`.
Add, near the `s_ram_sprite` definition:
```cpp
// Reclaimable cache: the 18 KB DMA mascot sprite refills from flash on the
// next draw, so it yields to any RF feature that needs the internal heap.
static void heap_argus_release(void) {
    if (s_ram_sprite) { heap_caps_free(s_ram_sprite); s_ram_sprite = nullptr; }
    s_ram_sprite_tried = false;   // allow a fresh alloc attempt next draw
    s_ram_mood = (argus_mood_t)-1; // force a memcpy refresh when it comes back
}
```
Note: confirm the real name/type of the mood cache variable (`s_ram_mood`) at `argus.cpp:157-161` and match it; the invalidation must force the `cur != s_ram_mood` refresh branch.
`s_ram_sprite_tried` is currently a function-local `static` at `argus.cpp:144`. Promote it to a file-scope `static bool s_ram_sprite_tried = false;` (remove the local) so `heap_argus_release` can reset it. Update the draw function to use the file-scope one.

- [ ] **Step 2: Register once**

In the ARGUS init path (grep for the argus setup/first-draw function), add once:
```cpp
    static bool s_reg = false;
    if (!s_reg) { s_reg = true; heap_reclaim_register(heap_argus_release); }
```

- [ ] **Step 3: Build**

Run: `pio run -e cardputer`
Expected: SUCCESS.

- [ ] **Step 4: Commit**

```bash
git add src/argus.cpp
git commit -m "feat(heap): ARGUS sprite yields its 18KB DMA cache on reclaim"
```

---

## Task 6: blanket per-feature leak instrumentation

**Files:**
- Modify: `src/menu.cpp:1353-1368` and `src/menu.cpp:1388-1404`

**Interfaces:**
- Consumes: Tasks 1, 2.

- [ ] **Step 1: Instrument the ENTER seam (around line 1355)**

Add `#include "heap_budget.h"` at the top of `src/menu.cpp`.
Replace the block at 1355-1359:
```cpp
            if (sel->action) {
                Serial.printf("[FEAT_ENTER] %s\n", sel->label);
                g_current_feature_item = sel;
                sel->action();
                g_current_feature_item = nullptr;
```
with:
```cpp
            if (sel->action) {
                heap_reclaim_all();
                size_t hb_base = heap_free_internal();
                Serial.printf("[FEAT_ENTER] %s free=%u\n", sel->label, (unsigned)hb_base);
                g_current_feature_item = sel;
                sel->action();
                g_current_feature_item = nullptr;
                { size_t now = heap_free_internal();
                  long d = (long)now - (long)hb_base;
                  Serial.printf("[FEAT_EXIT] %s free=%u delta=%ld%s\n",
                      sel->label, (unsigned)now, d, d < -2048 ? " LEAK" : ""); }
```
Remove the now-duplicate `[FEAT_EXIT]` printf at line 1364.

- [ ] **Step 2: Instrument the hotkey seam (around line 1392)**

Apply the identical transform to the `ch->action()` block at 1392-1399, using `ch->label`. Remove its duplicate `[FEAT_EXIT]` printf at 1399.

- [ ] **Step 3: Build**

Run: `pio run -e cardputer`
Expected: SUCCESS.

- [ ] **Step 4: Commit**

```bash
git add src/menu.cpp
git commit -m "feat(heap): reclaim + per-feature heap delta at both dispatch seams"
```

---

## Task 7: route wifi_portal through rf_preflight

**Files:**
- Modify: `src/features/wifi_portal.cpp:418-521`

**Interfaces:**
- Consumes: Task 3.

- [ ] **Step 1: Add preflight at the top of run_portal AP bring-up**

Add `#include "heap_budget.h"` near the top of the file.
Immediately after the `[portal] AP-up entry free=` printf (line 418-419), add:
```cpp
    if (!rf_preflight("portal", 12288)) return;   // reclaim + veto; 12KB contiguous for hostap_attach
```
(12288 is the initial estimate; Task 13 calibrates it from real serial numbers.)

- [ ] **Step 2: Replace the ad-hoc pre-start log with heap_report**

Replace the `[portal] pre-start largest_free=` printf at 514-516 with:
```cpp
    heap_report("portal pre-start");
    size_t lblk = heap_largest_internal();
```
Keep the existing `if (lblk < 10240)` last-resort guard below it unchanged.

- [ ] **Step 3: Build**

Run: `pio run -e cardputer`
Expected: SUCCESS.

- [ ] **Step 4: Commit**

```bash
git add src/features/wifi_portal.cpp
git commit -m "feat(heap): portal AP bring-up gated by rf_preflight"
```

---

## Task 8: route remaining RF features through rf_preflight

**Files:**
- Modify each RF feature entry: `src/features/wifi_beacon_spam.cpp`, `src/features/wifi_deauth*.cpp`, `src/features/wifi_karma.cpp`, `src/features/wifi_pmkid.cpp`, `src/features/wifi_apclone.cpp`, evil-twin (grep `feat_evil_twin`).

**Interfaces:**
- Consumes: Task 3.

- [ ] **Step 1: Add preflight at each feature entry**

For each `feat_*` that calls `esp_wifi_init`/`esp_wifi_start`, add `#include "heap_budget.h"` and, at the top of the feature function (after SD/arg checks, before radio bring-up):
```cpp
    if (!rf_preflight("<feature-name>", 12288)) return;
```
Use the feature's own tag string. If a feature already toasts on its own low-memory path, keep only the `rf_preflight` guard and remove the redundant older check.

- [ ] **Step 2: Build**

Run: `pio run -e cardputer`
Expected: SUCCESS.

- [ ] **Step 3: Commit**

```bash
git add src/features/wifi_beacon_spam.cpp src/features/wifi_deauth*.cpp src/features/wifi_karma.cpp src/features/wifi_pmkid.cpp src/features/wifi_apclone.cpp
git commit -m "feat(heap): gate all RF features through rf_preflight"
```

---

## Task 9: lazy-alloc g_wdr_aps (20 KB) — alloc once, no free-on-exit

**Files:**
- Modify: `src/features/wifi_wardrive.cpp:30` (definition), `src/wifi_wardrive.h:30` (extern decl)
- Modify consumers: `src/features/triton.cpp:1119-1120`, `src/features/wifi_pmkid.cpp:563-566`

**IMPORTANT — shared session state:** `g_wdr_aps` is written by wardrive and read
LATER by triton and pmkid in the same session. It is NOT scratch. So it is
lazy-allocated once on first wardrive use and left resident; it is NOT freed on
wardrive exit (that would corrupt triton/pmkid reads). The win: sessions that
never wardrive keep the 20 KB free, and it starts free after every reboot.

**Interfaces:**
- Produces: `wdr_ap_t *g_wdr_aps` (was `wdr_ap_t g_wdr_aps[WARDRIVE_MAX_APS]`), plus `bool wdr_aps_ensure(void)` that lazily allocates it.

- [ ] **Step 1: Convert array to lazy pointer + ensure helper**

In `src/wifi_wardrive.h:30`, change:
```cpp
extern wdr_ap_t g_wdr_aps[WARDRIVE_MAX_APS];
```
to:
```cpp
extern wdr_ap_t *g_wdr_aps;   // lazy: allocated on first wardrive use, then resident
bool wdr_aps_ensure(void);    // returns true if g_wdr_aps is non-null (allocs if needed)
```
In `src/features/wifi_wardrive.cpp:30`, change the definition to:
```cpp
wdr_ap_t *g_wdr_aps = nullptr;
bool wdr_aps_ensure(void) {
    if (g_wdr_aps) return true;
    g_wdr_aps = (wdr_ap_t *)heap_caps_calloc(WARDRIVE_MAX_APS, sizeof(wdr_ap_t), MALLOC_CAP_INTERNAL);
    if (!g_wdr_aps) { ui_toast("Low memory for wardrive", T_BAD, 1500); return false; }
    return true;
}
```

- [ ] **Step 2: Guard the writer**

At the top of `feat_wifi_wardrive` (before it first writes `g_wdr_aps`), add:
```cpp
    if (!wdr_aps_ensure()) return;
```

- [ ] **Step 3: Null-guard the later readers**

In `triton.cpp` before the `g_wdr_aps[i]` loop (near 1119) and in `wifi_pmkid.cpp` before the loop (near 563), add:
```cpp
    if (!g_wdr_aps) { /* no wardrive data this session */ }
    else for (...) { ... existing loop ... }
```
Concretely: wrap the existing copy loop in `if (g_wdr_aps) { ... }`. These readers already gate on a populated count, but the null check is defense in depth since the buffer can now be null.

- [ ] **Step 4: Build + confirm the symbol left .bss**

Run: `pio run -e cardputer`
Then: `xtensa-esp32s3-elf-nm .pio/build/cardputer/firmware.elf | grep g_wdr_aps`
Expected: SUCCESS; `g_wdr_aps` is now a 4-byte pointer (`b`/`B`), not a 20 KB object.

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "perf(heap): lazy-alloc g_wdr_aps once, freeing 20KB until first wardrive"
```

---

## Task 10: lazy-alloc the next-largest app caches

**Files:**
- Modify owners of `s_capq` (8 KB), `feat_wifi_ciwv active` (6.9 KB), `feat_usb_guard hits` (5 KB), `s_hits` (4.7 KB). Grep each symbol for its definition and owning feature.

**Interfaces:**
- Same lazy pattern as Task 9, one buffer at a time.

**IMPORTANT — classify each buffer's lifetime first.** Before converting, grep
every reader of the symbol across `src/`. If it is read only inside its own
feature run, use free-on-exit (alloc on entry, free on every exit path). If any
OTHER feature reads it (shared session state, like `g_wdr_aps`), use the
Task 9 lazy-once pattern (alloc on first use, do NOT free on exit) and
null-guard the readers. Never free-on-exit a buffer another feature reads later.

- [ ] **Step 1: Convert each buffer, one commit each**

For each symbol, apply the pattern matching its lifetime class (above): static
array to lazy pointer, alloc with null-check toast. Verify the owning feature by
the symbol's mangled name (`feat_wifi_ciwv`, `feat_usb_guard` are function-local
statics; promote to file scope or gate with an init/teardown pair). A
function-local static read only within that function is the safe free-on-exit
case.

- [ ] **Step 2: Build + nm check after each**

Run: `pio run -e cardputer`
Then confirm the symbol dropped to a pointer via `nm ... | grep <symbol>`.
Expected: SUCCESS; each large `.bss` object becomes a 4-byte pointer.

- [ ] **Step 3: Commit each conversion**

```bash
git add -A
git commit -m "perf(heap): lazy-alloc <symbol>, freeing <N>KB when idle"
```

---

## Task 11: Settings heap census screen

**Files:**
- Modify: the settings menu builder (grep for the Settings submenu node array) and add a `feat_heap_census` entry.
- Create/modify: a small `feat_heap_census(void)` (place in `src/features/` or an existing settings feature file).

**Interfaces:**
- Consumes: Task 1/4 (`heap_free_internal`, `heap_largest_internal`, `heap_min_ever_internal`, `heap_reclaim_all`).

- [ ] **Step 1: Implement the screen**

```cpp
void feat_heap_census(void) {
    for (;;) {
        ui_clear();
        ui_title("Heap");
        char line[64];
        snprintf(line, sizeof(line), "free    %6u", (unsigned)heap_free_internal());
        ui_text(line);
        snprintf(line, sizeof(line), "largest %6u", (unsigned)heap_largest_internal());
        ui_text(line);
        snprintf(line, sizeof(line), "min ever%6u", (unsigned)heap_min_ever_internal());
        ui_text(line);
        ui_text("R reclaim   ESC back");
        int k = ui_wait_key();
        if (k == PK_ESC) return;
        if (k == 'r' || k == 'R') heap_reclaim_all();
    }
}
```
Match the real UI primitives used by other simple screens (grep a small existing `feat_*` for `ui_title`/`ui_text`/`ui_wait_key` exact names) and mirror them.

- [ ] **Step 2: Register the menu entry**

Add `{ .label = "Heap", .hotkey = 'h', .action = feat_heap_census }` (matching the real `menu_node_t` field layout) to the Settings children array, and `extern void feat_heap_census(void);` with the other externs in `src/menu.cpp`.

- [ ] **Step 3: Build**

Run: `pio run -e cardputer`
Expected: SUCCESS.

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "feat(heap): on-device heap census screen with manual reclaim"
```

---

## Task 12: hardware calibration and verification (requires device)

**Files:** none (measurement + threshold tuning only).

**Interfaces:** Consumes all prior tasks.

- [ ] **Step 1: Flash**

Run: `pio run -e cardputer -t upload --upload-port COM16` (toolchain on PATH).
Expected: upload succeeds. If the port is absent, replug the Cardputer or enter download mode (hold G0/BOOT, tap RESET).

- [ ] **Step 2: Baseline census**

Read serial at boot. Record `[heap] census ...`. Expected: internal free higher than before the diet (target: +40 KB or more vs the pre-diet 48 KB fresh-boot figure).

- [ ] **Step 3: Cycle features and watch for leaks**

Open Triton, BLE scan, ARGUS home, spectrum, wardrive; return to menu after each. On serial, every `[FEAT_EXIT]` delta should be near 0. Any line tagged `LEAK` names a feature to fix (add its buffer to the reclaim registry or fix its exit path). Iterate until no `LEAK`.

- [ ] **Step 4: Reproduce the original crash scenario**

After cycling features, WiFi -> Scan -> select AP -> C (clone). Read `[heap] portal pre-start free=.. largest=..`. Expected: clone AP starts (no crash, no reboot). If `rf_preflight` vetoes with the low-memory toast, note the `largest` value.

- [ ] **Step 5: Calibrate the preflight threshold**

Using the real `largest` value at a successful `esp_wifi_start`, set `need_bytes` in the `rf_preflight("portal", …)` call (Task 7) to just below the smallest observed successful value, with ~2 KB margin. Rebuild, reflash, re-verify the clone starts from a cycled session. Apply the same tuned value to the other RF features (Task 8).

- [ ] **Step 6: Commit the calibrated thresholds**

```bash
git add src/features/
git commit -m "fix(heap): calibrate rf_preflight thresholds from on-device numbers"
```

---

## Task 13: finish the branch

- [ ] **Step 1: Full host test + build gate**

Run: `pio test -e native-test -f test_heap_budget` (expect all pass) and `pio run -e cardputer` (expect SUCCESS).

- [ ] **Step 2: Use superpowers:finishing-a-development-branch** to decide merge/PR/cleanup for the `heap-stability` branch.
