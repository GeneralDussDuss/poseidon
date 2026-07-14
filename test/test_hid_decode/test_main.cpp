#include <unity.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "../../src/hid_decode.cpp"

// Canonical USB HID boot-keyboard report map (HID 1.11, Appendix B.1).
// modifier byte at offset 0, one reserved byte at offset 1, six keycode
// bytes at offsets 2..7. No report ID.
static const uint8_t BOOT_KBD_MAP[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x06,        // Usage (Keyboard)
    0xA1, 0x01,        // Collection (Application)
    0x05, 0x07,        //   Usage Page (Keyboard/Keypad)
    0x19, 0xE0,        //   Usage Minimum (Left Control)
    0x29, 0xE7,        //   Usage Maximum (Right GUI)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x08,        //   Report Count (8)
    0x81, 0x02,        //   Input (Data,Var,Abs)  -> modifier byte
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x08,        //   Report Size (8)
    0x81, 0x01,        //   Input (Const)         -> reserved byte
    0x95, 0x05,        //   Report Count (5)
    0x75, 0x01,        //   Report Size (1)
    0x05, 0x08,        //   Usage Page (LEDs)
    0x19, 0x01,        //   Usage Minimum (1)
    0x29, 0x05,        //   Usage Maximum (5)
    0x91, 0x02,        //   Output (Data,Var,Abs) -> LED report
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x03,        //   Report Size (3)
    0x91, 0x01,        //   Output (Const)        -> LED padding
    0x95, 0x06,        //   Report Count (6)
    0x75, 0x08,        //   Report Size (8)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x65,        //   Logical Maximum (101)
    0x05, 0x07,        //   Usage Page (Keyboard/Keypad)
    0x19, 0x00,        //   Usage Minimum (0)
    0x29, 0x65,        //   Usage Maximum (101)
    0x81, 0x00,        //   Input (Data,Array)    -> six keycode bytes
    0xC0               // End Collection
};

static hid_layout_t g_layout;

void setUp(void)    { memset(&g_layout, 0, sizeof(g_layout)); }
void tearDown(void) {}

void test_parse_boot_keyboard_layout(void) {
    TEST_ASSERT_TRUE(hid_parse_report_map(BOOT_KBD_MAP, sizeof(BOOT_KBD_MAP), &g_layout));
    TEST_ASSERT_TRUE(g_layout.is_keyboard);
    TEST_ASSERT_EQUAL_INT(0, g_layout.report_id);       // no report ID prefix
    TEST_ASSERT_EQUAL_INT(0, g_layout.modifier_offset);  // modifier byte first
    TEST_ASSERT_EQUAL_INT(2, g_layout.key_offset);       // after reserved byte
    TEST_ASSERT_EQUAL_INT(6, g_layout.key_count);        // six keycode slots
}

void test_decode_single_letter(void) {
    hid_parse_report_map(BOOT_KBD_MAP, sizeof(BOOT_KBD_MAP), &g_layout);
    const uint8_t rpt[8] = { 0x00, 0x00, 0x04, 0, 0, 0, 0, 0 };  // 'a' usage
    char out[64];
    int n = hid_decode_keyboard(&g_layout, rpt, sizeof(rpt), out, sizeof(out));
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_STRING("A", out);
}

void test_decode_shift_modifier(void) {
    hid_parse_report_map(BOOT_KBD_MAP, sizeof(BOOT_KBD_MAP), &g_layout);
    const uint8_t rpt[8] = { 0x02, 0x00, 0x04, 0, 0, 0, 0, 0 };  // LShift + 'a'
    char out[64];
    int n = hid_decode_keyboard(&g_layout, rpt, sizeof(rpt), out, sizeof(out));
    TEST_ASSERT_EQUAL_INT(2, n);
    TEST_ASSERT_EQUAL_STRING("LShift+A", out);
}

void test_decode_named_key_enter(void) {
    hid_parse_report_map(BOOT_KBD_MAP, sizeof(BOOT_KBD_MAP), &g_layout);
    const uint8_t rpt[8] = { 0x00, 0x00, 0x28, 0, 0, 0, 0, 0 };  // Enter
    char out[64];
    int n = hid_decode_keyboard(&g_layout, rpt, sizeof(rpt), out, sizeof(out));
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_STRING("Enter", out);
}

void test_decode_empty_report(void) {
    hid_parse_report_map(BOOT_KBD_MAP, sizeof(BOOT_KBD_MAP), &g_layout);
    const uint8_t rpt[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    char out[64];
    int n = hid_decode_keyboard(&g_layout, rpt, sizeof(rpt), out, sizeof(out));
    TEST_ASSERT_EQUAL_INT(0, n);
    TEST_ASSERT_EQUAL_STRING("", out);
}

void test_decode_multiple_keys(void) {
    hid_parse_report_map(BOOT_KBD_MAP, sizeof(BOOT_KBD_MAP), &g_layout);
    const uint8_t rpt[8] = { 0x00, 0x00, 0x04, 0x05, 0, 0, 0, 0 };  // 'a' 'b'
    char out[64];
    int n = hid_decode_keyboard(&g_layout, rpt, sizeof(rpt), out, sizeof(out));
    TEST_ASSERT_EQUAL_INT(2, n);
    TEST_ASSERT_EQUAL_STRING("A+B", out);
}

void test_reject_non_keyboard_map(void) {
    // Usage (Mouse) instead of Keyboard -> not a keyboard layout.
    const uint8_t mouse_map[] = {
        0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0xC0
    };
    TEST_ASSERT_FALSE(hid_parse_report_map(mouse_map, sizeof(mouse_map), &g_layout));
    TEST_ASSERT_FALSE(g_layout.is_keyboard);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_parse_boot_keyboard_layout);
    RUN_TEST(test_decode_single_letter);
    RUN_TEST(test_decode_shift_modifier);
    RUN_TEST(test_decode_named_key_enter);
    RUN_TEST(test_decode_empty_report);
    RUN_TEST(test_decode_multiple_keys);
    RUN_TEST(test_reject_non_keyboard_map);
    return UNITY_END();
}
