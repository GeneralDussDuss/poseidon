#include <unity.h>
#include "../../src/ui/plist.cpp"

void setUp(void) {}
void tearDown(void) {}

static void test_init_starts_at_top(void) {
    plist_t m;
    plist_init(&m, 20, 7);
    TEST_ASSERT_EQUAL(0, m.sel);
    TEST_ASSERT_EQUAL(0, m.top);
    TEST_ASSERT_EQUAL(7, plist_visible_count(&m));
}

static void test_short_list_shows_only_what_exists(void) {
    plist_t m;
    plist_init(&m, 3, 7);
    TEST_ASSERT_EQUAL(3, plist_visible_count(&m));
}

/* Moving within the visible window must NOT scroll: that is what lets the
 * caller repaint only two rows instead of the whole panel. */
static void test_move_inside_window_does_not_scroll(void) {
    plist_t m;
    plist_init(&m, 20, 7);
    TEST_ASSERT_FALSE(plist_move(&m, 1));
    TEST_ASSERT_EQUAL(1, m.sel);
    TEST_ASSERT_EQUAL(0, m.top);
}

static void test_move_past_bottom_scrolls_by_one(void) {
    plist_t m;
    plist_init(&m, 20, 7);
    for (int i = 0; i < 6; i++) { plist_move(&m, 1); }   /* sel == 6, last visible */
    TEST_ASSERT_EQUAL(6, m.sel);
    TEST_ASSERT_EQUAL(0, m.top);
    TEST_ASSERT_TRUE(plist_move(&m, 1));                 /* now it must scroll */
    TEST_ASSERT_EQUAL(7, m.sel);
    TEST_ASSERT_EQUAL(1, m.top);
}

static void test_wrap_forward_from_end(void) {
    plist_t m;
    plist_init(&m, 10, 7);
    for (int i = 0; i < 9; i++) { plist_move(&m, 1); }
    TEST_ASSERT_EQUAL(9, m.sel);
    TEST_ASSERT_TRUE(plist_move(&m, 1));
    TEST_ASSERT_EQUAL(0, m.sel);
    TEST_ASSERT_EQUAL(0, m.top);
}

static void test_wrap_backward_from_start(void) {
    plist_t m;
    plist_init(&m, 10, 7);
    TEST_ASSERT_TRUE(plist_move(&m, -1));
    TEST_ASSERT_EQUAL(9, m.sel);
    TEST_ASSERT_EQUAL(3, m.top);   /* count - rows == 10 - 7 */
}

static void test_empty_list_is_inert(void) {
    plist_t m;
    plist_init(&m, 0, 7);
    TEST_ASSERT_EQUAL(0, plist_visible_count(&m));
    TEST_ASSERT_FALSE(plist_move(&m, 1));
    TEST_ASSERT_EQUAL(0, m.sel);
}

static void test_list_shorter_than_window_never_scrolls(void) {
    plist_t m;
    plist_init(&m, 3, 7);
    TEST_ASSERT_FALSE(plist_move(&m, 1));
    TEST_ASSERT_FALSE(plist_move(&m, 1));
    TEST_ASSERT_EQUAL(2, m.sel);
    TEST_ASSERT_EQUAL(0, m.top);
    TEST_ASSERT_TRUE(plist_move(&m, 1));   /* wraps to 0 */
    TEST_ASSERT_EQUAL(0, m.sel);
    TEST_ASSERT_EQUAL(0, m.top);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_init_starts_at_top);
    RUN_TEST(test_short_list_shows_only_what_exists);
    RUN_TEST(test_move_inside_window_does_not_scroll);
    RUN_TEST(test_move_past_bottom_scrolls_by_one);
    RUN_TEST(test_wrap_forward_from_end);
    RUN_TEST(test_wrap_backward_from_start);
    RUN_TEST(test_empty_list_is_inert);
    RUN_TEST(test_list_shorter_than_window_never_scrolls);
    return UNITY_END();
}
