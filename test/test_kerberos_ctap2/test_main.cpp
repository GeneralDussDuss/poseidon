#include <unity.h>
#include <string.h>
#include "cbor.h"

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

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_cbor_encode_small_map);
    return UNITY_END();
}
