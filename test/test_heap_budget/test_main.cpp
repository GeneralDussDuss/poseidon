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

static int g_reclaim_calls = 0;
static void reclaimer_frees_10k(void)   { g_fake_free += 10000; g_reclaim_calls++; }
static void reclaimer_frees_10k_2(void) { g_fake_free += 10000; g_reclaim_calls++; }

void test_registry_dedups_and_runs_all(void) {
    g_fake_free = 20000; g_reclaim_calls = 0;
    heap_reclaim_register(reclaimer_frees_10k);
    heap_reclaim_register(reclaimer_frees_10k);    // duplicate -> registered once (idempotent)
    heap_reclaim_register(reclaimer_frees_10k_2);  // distinct -> also runs
    size_t recovered = heap_reclaim_all();
    TEST_ASSERT_EQUAL_INT(2, g_reclaim_calls);     // each distinct fn runs exactly once
    TEST_ASSERT_EQUAL_UINT32(20000, recovered);
}

void test_reclaim_all_empty_is_zero(void) {
    g_fake_free = 30000;
    TEST_ASSERT_EQUAL_UINT32(0, heap_reclaim_all());
}

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

void test_min_ever_zero_before_any_query(void) {
    hb_test_reset();
    // No heap_free_internal() call yet -> sentinel unresolved -> reports 0.
    TEST_ASSERT_EQUAL_UINT32(0, heap_min_ever_internal());
}

void test_null_query_does_not_pin_watermark(void) {
    hb_test_reset();
    hb_set_query(nullptr, nullptr);
    heap_free_internal();                 // returns 0 but must NOT pin min_ever to 0
    hb_set_query(fake_free, fake_largest);
    g_fake_free = 50000;
    heap_free_internal();
    TEST_ASSERT_EQUAL_UINT32(50000, heap_min_ever_internal());
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_free_and_largest_passthrough);
    RUN_TEST(test_min_ever_tracks_lowest_free);
    RUN_TEST(test_registry_dedups_and_runs_all);
    RUN_TEST(test_reclaim_all_empty_is_zero);
    RUN_TEST(test_preflight_true_when_largest_fits_after_reclaim);
    RUN_TEST(test_preflight_false_when_largest_too_small);
    RUN_TEST(test_min_ever_zero_before_any_query);
    RUN_TEST(test_null_query_does_not_pin_watermark);
    return UNITY_END();
}
