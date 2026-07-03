#include <unity.h>
#include <string.h>
#include "cbor.h"
#include "cbor_util.h"

void setUp(void) {}
void tearDown(void) {}

// Encode {1: 2} and confirm the canonical bytes A1 01 02.
static void test_cbor_encode_small_map(void) {
    uint8_t buf[16]; CborEncoder enc, map;
    cbor_encoder_init(&enc, buf, sizeof buf, 0);
    cbor_encoder_create_map(&enc, &map, 1);
    cbor_encode_int(&map, 1);
    cbor_encode_int(&map, 2);
    cbor_encoder_close_container(&enc, &map);
    size_t n = cbor_encoder_get_buffer_size(&enc, buf);
    TEST_ASSERT_EQUAL_UINT(3, n);
    uint8_t want[] = {0xA1, 0x01, 0x02};
    TEST_ASSERT_EQUAL_MEMORY(want, buf, 3);
}

// Write {1: h'AABB', 3: "abc"} via cbor_util, assert canonical bytes, parse back.
static void test_write_and_parse(void) {
    uint8_t buf[64]; cbor_writer w; cw_init(&w, buf, sizeof buf);
    cw_map(&w, 2);
    cw_key(&w, 1); uint8_t bs[2] = {0xAA, 0xBB}; cw_bytes(&w, bs, 2);
    cw_key(&w, 3); cw_text(&w, "abc");
    size_t n = cw_finish(&w);
    uint8_t want[] = {0xA2, 0x01, 0x42, 0xAA, 0xBB, 0x03, 0x63, 'a', 'b', 'c'};
    TEST_ASSERT_EQUAL_UINT(sizeof want, n);
    TEST_ASSERT_EQUAL_MEMORY(want, buf, n);

    CborParser p; CborValue map;
    TEST_ASSERT_EQUAL_INT(0, cbor_get_map(buf, n, &p, &map));
    uint8_t got[4]; size_t gl = sizeof got;
    TEST_ASSERT_EQUAL_INT(0, cbor_map_bytes(&map, 1, got, &gl));
    TEST_ASSERT_EQUAL_UINT(2, gl);
    TEST_ASSERT_EQUAL_MEMORY(bs, got, 2);
    char t[8]; size_t tl = sizeof t;
    TEST_ASSERT_EQUAL_INT(0, cbor_map_text(&map, 3, t, &tl));
    TEST_ASSERT_EQUAL_STRING("abc", t);
    // absent key -> negative
    uint64_t u;
    TEST_ASSERT_NOT_EQUAL(0, cbor_map_uint(&map, 9, &u));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_cbor_encode_small_map);
    RUN_TEST(test_write_and_parse);
    return UNITY_END();
}

