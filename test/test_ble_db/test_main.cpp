/* Native Unity tests for the REAL ble_db.cpp — OUI/vendor lookup, the binary
 * search, and the Apple/FastPair/UUID tables. Pure logic + data returned; no
 * hardware. Include the .cpp directly so we can also reach the static tables to
 * prove the binary-search invariants (sorted + unique + every entry findable). */
#include <unity.h>
#include <string.h>
#include "../../src/ble_db.cpp"

void setUp(void) {}
void tearDown(void) {}

/* Known OUIs from the generated table map to the exact vendor label. */
static void test_oui_known_vendors(void) {
    TEST_ASSERT_EQUAL_STRING("Cisco",   ble_db_oui(0x00000C));
    TEST_ASSERT_EQUAL_STRING("AMD",     ble_db_oui(0x00001A));
    TEST_ASSERT_EQUAL_STRING("Toshiba", ble_db_oui(0x000039));
    TEST_ASSERT_EQUAL_STRING("Epson",   ble_db_oui(0x000048));
    TEST_ASSERT_EQUAL_STRING("Canon",   ble_db_oui(0x000085));
    TEST_ASSERT_EQUAL_STRING("Dell",    ble_db_oui(0x000097));
    TEST_ASSERT_EQUAL_STRING("Samsung", ble_db_oui(0x0000F0));
}

/* Lookup masks to 24 bits — a full 32-bit value with junk high byte still hits. */
static void test_oui_masking(void) {
    TEST_ASSERT_EQUAL_STRING("Samsung", ble_db_oui(0xAB0000F0));
    TEST_ASSERT_EQUAL_STRING("Cisco",   ble_db_oui(0xFF00000C));
}

/* A miss returns nullptr (all-ones is not an assigned OUI). */
static void test_oui_miss(void) {
    TEST_ASSERT_NULL(ble_db_oui(0xFFFFFF));
}

/* Binary-search correctness rests on the table being strictly ascending
 * (sorted + no duplicate OUIs), and every entry must be findable. This walks
 * all ~4352 rows — validates the actual data returned for the whole table. */
static void test_oui_table_integrity(void) {
    TEST_ASSERT_GREATER_THAN(1000, (int)OUI_N);
    for (size_t i = 1; i < OUI_N; i++)
        TEST_ASSERT_TRUE_MESSAGE(OUI[i - 1].oui < OUI[i].oui,
                                 "OUI table must be strictly ascending (sorted+unique)");
    for (size_t i = 0; i < OUI_N; i++) {
        const char *v = ble_db_oui(OUI[i].oui);
        TEST_ASSERT_NOT_NULL(v);
        TEST_ASSERT_EQUAL_STRING(OUI[i].vendor, v);
    }
}

static void test_apple_continuity(void) {
    TEST_ASSERT_EQUAL_STRING("iBeacon",     ble_db_apple(0x02, 0));
    TEST_ASSERT_EQUAL_STRING("AirTag",      ble_db_apple(0x12, 0));
    TEST_ASSERT_EQUAL_STRING("AirPods Pro", ble_db_apple(0x07, 0x02));
    TEST_ASSERT_EQUAL_STRING("Apple pairing", ble_db_apple(0x07, 0xFE)); /* unknown model */
    TEST_ASSERT_NULL(ble_db_apple(0xEE, 0));                             /* unknown subtype */
}

static void test_fastpair(void) {
    TEST_ASSERT_EQUAL_STRING("Pixel Buds A", ble_db_fastpair(0x00000F));
    TEST_ASSERT_NULL(ble_db_fastpair(0x999999));
}

static void test_uuids(void) {
    TEST_ASSERT_EQUAL_STRING("Battery",       ble_db_svc_uuid(0x180F));
    TEST_ASSERT_EQUAL_STRING("HID",           ble_db_svc_uuid(0x1812));
    TEST_ASSERT_EQUAL_STRING("Battery Level", ble_db_chr_uuid(0x2A19));
    TEST_ASSERT_NULL(ble_db_svc_uuid(0x0000));
}

/* ble_db_identify: OUI fallback for a public MAC. */
static void test_identify_oui(void) {
    uint8_t mac[6] = { 0x00, 0x00, 0xF0, 0x11, 0x22, 0x33 };  /* Samsung OUI */
    char out[32];
    TEST_ASSERT_TRUE(ble_db_identify(mac, nullptr, 0, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("Samsung", out);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_oui_known_vendors);
    RUN_TEST(test_oui_masking);
    RUN_TEST(test_oui_miss);
    RUN_TEST(test_oui_table_integrity);
    RUN_TEST(test_apple_continuity);
    RUN_TEST(test_fastpair);
    RUN_TEST(test_uuids);
    RUN_TEST(test_identify_oui);
    return UNITY_END();
}
