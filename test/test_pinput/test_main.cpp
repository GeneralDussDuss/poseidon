#include <unity.h>
#include <stdint.h>
#include "../../src/ui/pinput.cpp"

void setUp(void) {}
void tearDown(void) {}

static void test_arrows_map_to_directions(void) {
    TEST_ASSERT_EQUAL(UI_EV_UP,    pinput_map(0x100).kind);
    TEST_ASSERT_EQUAL(UI_EV_DOWN,  pinput_map(0x101).kind);
    TEST_ASSERT_EQUAL(UI_EV_LEFT,  pinput_map(0x102).kind);
    TEST_ASSERT_EQUAL(UI_EV_RIGHT, pinput_map(0x103).kind);
}

static void test_enter_and_esc(void) {
    TEST_ASSERT_EQUAL(UI_EV_SELECT, pinput_map(0x0D).kind);
    TEST_ASSERT_EQUAL(UI_EV_BACK,   pinput_map(0x1B).kind);
}

static void test_backspace_is_its_own_event(void) {
    TEST_ASSERT_EQUAL(UI_EV_BKSP, pinput_map(0x08).kind);
}

/* Letter mnemonics are POSEIDON's core UX: printable keys must survive
 * as characters, not be swallowed into navigation events. */
static void test_printables_carry_the_character(void) {
    ui_event_t w = pinput_map('w');
    TEST_ASSERT_EQUAL(UI_EV_CHAR, w.kind);
    TEST_ASSERT_EQUAL('w', w.ch);

    ui_event_t space = pinput_map(0x20);
    TEST_ASSERT_EQUAL(UI_EV_CHAR, space.kind);
    TEST_ASSERT_EQUAL(' ', space.ch);

    ui_event_t tilde = pinput_map(0x7E);
    TEST_ASSERT_EQUAL(UI_EV_CHAR, tilde.kind);
    TEST_ASSERT_EQUAL('~', tilde.ch);
}

static void test_none_and_unknown_are_none(void) {
    TEST_ASSERT_EQUAL(UI_EV_NONE, pinput_map(0).kind);
    TEST_ASSERT_EQUAL(UI_EV_NONE, pinput_map(0x104).kind);  /* PK_FN */
    TEST_ASSERT_EQUAL(UI_EV_NONE, pinput_map(0x09).kind);   /* PK_TAB */
    TEST_ASSERT_EQUAL(UI_EV_NONE, pinput_map(0x1FF).kind);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_arrows_map_to_directions);
    RUN_TEST(test_enter_and_esc);
    RUN_TEST(test_backspace_is_its_own_event);
    RUN_TEST(test_printables_carry_the_character);
    RUN_TEST(test_none_and_unknown_are_none);
    return UNITY_END();
}
