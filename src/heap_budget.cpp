#include "heap_budget.h"

#if !defined(PIO_UNIT_TESTING)
#include <Arduino.h>
#include <esp_heap_caps.h>
#include "ui.h"      // ui_toast(const char*, uint16_t color, uint32_t ms)
#include "theme.h"   // T_BAD == theme().bad (runtime themed color)
static size_t esp_free(void)    { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL); }
static size_t esp_largest(void) { return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL); }
#endif

static size_t (*s_free_fn)(void)    = 0;
static size_t (*s_largest_fn)(void) = 0;
static size_t s_min_ever = (size_t)-1;

#define HB_MAX_RECLAIMERS 16
static heap_reclaim_fn s_reclaimers[HB_MAX_RECLAIMERS];
static int s_reclaimer_n = 0;

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

void heap_reclaim_register(heap_reclaim_fn fn) {
    if (!fn) return;
    for (int i = 0; i < s_reclaimer_n; ++i) if (s_reclaimers[i] == fn) return;  // idempotent
    if (s_reclaimer_n < HB_MAX_RECLAIMERS) s_reclaimers[s_reclaimer_n++] = fn;
}

size_t heap_reclaim_all(void) {
    size_t before = s_free_fn ? s_free_fn() : 0;
    for (int i = 0; i < s_reclaimer_n; ++i) s_reclaimers[i]();
    size_t after = s_free_fn ? s_free_fn() : 0;
    return after > before ? after - before : 0;
}

void hb_test_reset(void) {
    s_min_ever = (size_t)-1;
    s_reclaimer_n = 0;
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
