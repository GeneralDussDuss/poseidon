#include <unity.h>
#include <string.h>
#include "cbor.h"
#include "cbor_util.h"
#include "ctap2.h"
#include "cose.h"
#include "authdata.h"

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

// getInfo returns status 0 and a versions array containing FIDO_2_0.
static void test_getinfo_has_fido2(void) {
    uint8_t aaguid[16] = {0};
    ctap2_cfg_t cfg; memset(&cfg, 0, sizeof cfg); cfg.aaguid = aaguid;
    uint8_t req[1] = {0x04}; uint8_t out[256];
    uint16_t n = ctap2_handle(&cfg, req, 1, out, sizeof out);
    TEST_ASSERT_EQUAL_UINT8(0x00, out[0]);
    CborParser p; CborValue map;
    TEST_ASSERT_EQUAL_INT(0, cbor_get_map(out + 1, n - 1, &p, &map));
    CborValue arr;
    TEST_ASSERT_EQUAL_INT(0, cbor_map_enter(&map, 1, &arr));
    TEST_ASSERT_TRUE(cbor_value_is_array(&arr));
    CborValue it; cbor_value_enter_container(&arr, &it);
    bool found = false;
    while (!cbor_value_at_end(&it)) {
        if (cbor_value_is_text_string(&it)) {
            char s[16]; size_t sl = sizeof s;
            cbor_value_copy_text_string(&it, s, &sl, &it);   // copies and advances
            if (strcmp(s, "FIDO_2_0") == 0) found = true;
        } else {
            cbor_value_advance(&it);
        }
    }
    TEST_ASSERT_TRUE(found);
}

static void test_cose_es256(void) {
    uint8_t pub[65]; pub[0] = 0x04;
    for (int i = 1; i < 65; i++) pub[i] = (uint8_t)i;
    uint8_t out[80];
    size_t n = cose_es256_from_pubkey(pub, out, sizeof out);
    uint8_t head[] = {0xA5,0x01,0x02,0x03,0x26,0x20,0x01,0x21,0x58,0x20};
    TEST_ASSERT_EQUAL_MEMORY(head, out, sizeof head);
    TEST_ASSERT_EQUAL_UINT(10 + 32 + 3 + 32, n);
    TEST_ASSERT_EQUAL_MEMORY(pub + 1, out + 10, 32);        // X
    TEST_ASSERT_EQUAL_UINT8(0x22, out[10 + 32]);            // key -3
    TEST_ASSERT_EQUAL_UINT8(0x58, out[10 + 32 + 1]);        // bstr(32)
    TEST_ASSERT_EQUAL_MEMORY(pub + 33, out + 10 + 32 + 3, 32); // Y
}

static void test_authdata_flags_counter(void) {
    uint8_t rp[32]; memset(rp, 0xAB, 32);
    uint8_t att[5] = {1,2,3,4,5};
    uint8_t out[64];
    size_t n = authdata_build(rp, AD_FLAG_UP | AD_FLAG_AT, 0x01020304, att, 5, out, sizeof out);
    TEST_ASSERT_EQUAL_UINT(32 + 1 + 4 + 5, n);
    TEST_ASSERT_EQUAL_MEMORY(rp, out, 32);
    TEST_ASSERT_EQUAL_UINT8(0x41, out[32]);                 // UP|AT
    TEST_ASSERT_EQUAL_UINT8(0x01, out[33]);
    TEST_ASSERT_EQUAL_UINT8(0x02, out[34]);
    TEST_ASSERT_EQUAL_UINT8(0x03, out[35]);
    TEST_ASSERT_EQUAL_UINT8(0x04, out[36]);
    TEST_ASSERT_EQUAL_MEMORY(att, out + 37, 5);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_cbor_encode_small_map);
    RUN_TEST(test_write_and_parse);
    RUN_TEST(test_getinfo_has_fido2);
    RUN_TEST(test_cose_es256);
    RUN_TEST(test_authdata_flags_counter);
    return UNITY_END();
}


