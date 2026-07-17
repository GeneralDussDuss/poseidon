/* Native Unity tests for the pure Argus-wardrive mood + milestone logic.
 * No Arduino / esp deps: wdr_mood.h is a standalone header, so these run
 * on the dev host (pio test -e native-test -f test_wdr_mood). */
#include <unity.h>
#include "../../src/features/wdr_mood.h"

void setUp(void) {}
void tearDown(void) {}

static wdr_mood_ctx base(void) {
    wdr_mood_ctx c;
    c.gps_valid = true;
    c.gps_ever_locked = true;
    c.gps_speed_kts = 5.0f;
    c.now_ms = 100000;
    c.entry_ms = 0;
    c.last_new_ms = 100000;   /* just found one */
    c.new_in_5s = 0;
    c.ap_count = 50;
    c.ap_cap = 256;
    return c;
}

static void test_table_full_is_stern(void) {
    wdr_mood_ctx c = base();
    c.ap_count = c.ap_cap;
    TEST_ASSERT_EQUAL(ARGUS_STERN, wdr_pick_mood(c));
}

static void test_no_fix_searching_is_curious(void) {
    wdr_mood_ctx c = base();
    c.gps_valid = false; c.gps_ever_locked = false;
    c.now_ms = 5000; c.entry_ms = 0;   /* only 5 s in */
    TEST_ASSERT_EQUAL(ARGUS_CURIOUS, wdr_pick_mood(c));
}

static void test_no_fix_slow_lock_is_reflective(void) {
    wdr_mood_ctx c = base();
    c.gps_valid = false; c.gps_ever_locked = false;
    c.now_ms = 25000; c.entry_ms = 0;  /* > 20 s no fix */
    TEST_ASSERT_EQUAL(ARGUS_REFLECTIVE, wdr_pick_mood(c));
}

static void test_locked_dense_is_calculating(void) {
    wdr_mood_ctx c = base();
    c.new_in_5s = 3;
    TEST_ASSERT_EQUAL(ARGUS_CALCULATING, wdr_pick_mood(c));
}

static void test_locked_steady_is_interested(void) {
    wdr_mood_ctx c = base();
    c.new_in_5s = 1;
    c.last_new_ms = 100000; c.now_ms = 104000;  /* 4 s since last new */
    TEST_ASSERT_EQUAL(ARGUS_INTERESTED, wdr_pick_mood(c));
}

static void test_locked_quiet_moving_is_watching(void) {
    wdr_mood_ctx c = base();
    c.new_in_5s = 0;
    c.last_new_ms = 100000; c.now_ms = 130000;  /* 30 s idle */
    c.gps_speed_kts = 5.0f;                      /* moving */
    TEST_ASSERT_EQUAL(ARGUS_WATCHING, wdr_pick_mood(c));
}

static void test_locked_quiet_parked_is_resigned(void) {
    wdr_mood_ctx c = base();
    c.new_in_5s = 0;
    c.last_new_ms = 100000; c.now_ms = 130000;  /* 30 s idle */
    c.gps_speed_kts = 0.0f;                      /* parked */
    TEST_ASSERT_EQUAL(ARGUS_RESIGNED, wdr_pick_mood(c));
}

static void test_locked_dry_spell_is_resigned(void) {
    wdr_mood_ctx c = base();
    c.new_in_5s = 0;
    c.last_new_ms = 100000; c.now_ms = 200000;  /* 100 s idle */
    TEST_ASSERT_EQUAL(ARGUS_RESIGNED, wdr_pick_mood(c));
}

static void test_locked_idle_is_sleeping(void) {
    wdr_mood_ctx c = base();
    c.new_in_5s = 0;
    c.last_new_ms = 100000; c.now_ms = 400000;  /* > 180 s idle */
    TEST_ASSERT_EQUAL(ARGUS_SLEEPING, wdr_pick_mood(c));
}

static void test_milestone_crossings(void) {
    TEST_ASSERT_TRUE(wdr_milestone_crossed(49, 50, 50));
    TEST_ASSERT_FALSE(wdr_milestone_crossed(50, 51, 50));
    TEST_ASSERT_TRUE(wdr_milestone_crossed(99, 100, 50));
    TEST_ASSERT_FALSE(wdr_milestone_crossed(0, 1, 50));
    TEST_ASSERT_FALSE(wdr_milestone_crossed(10, 10, 50));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_table_full_is_stern);
    RUN_TEST(test_no_fix_searching_is_curious);
    RUN_TEST(test_no_fix_slow_lock_is_reflective);
    RUN_TEST(test_locked_dense_is_calculating);
    RUN_TEST(test_locked_steady_is_interested);
    RUN_TEST(test_locked_quiet_moving_is_watching);
    RUN_TEST(test_locked_quiet_parked_is_resigned);
    RUN_TEST(test_locked_dry_spell_is_resigned);
    RUN_TEST(test_locked_idle_is_sleeping);
    RUN_TEST(test_milestone_crossings);
    return UNITY_END();
}
