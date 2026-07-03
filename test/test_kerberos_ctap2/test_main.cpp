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

// ---- mock crypto (deterministic, non-cryptographic) for CTAP2 command tests ----
static int mk_rand(uint8_t*d,size_t n,void*){for(size_t i=0;i<n;i++)d[i]=(uint8_t)(0x11+i);return 0;}
static int mk_sha256(const uint8_t*m,size_t n,uint8_t o[32],void*){for(int i=0;i<32;i++){uint8_t v=(uint8_t)i;for(size_t j=i;j<n;j+=32)v^=m[j];o[i]=v;}return 0;}
static int mk_keygen(uint8_t priv[32],uint8_t pub[65],void*){for(int i=0;i<32;i++)priv[i]=(uint8_t)(0x30+i);pub[0]=0x04;for(int i=1;i<65;i++)pub[i]=(uint8_t)(0x50+i);return 0;}
static int mk_sign(const uint8_t*,const uint8_t*,size_t,uint8_t*sig,size_t*sl,void*){static const uint8_t der[]={0x30,0x06,0x02,0x01,0x01,0x02,0x01,0x01};memcpy(sig,der,sizeof der);*sl=sizeof der;return 0;}
static uint8_t aad_fp2(const uint8_t*a,size_t n){uint8_t f=0;for(size_t i=0;i<n;i++)f^=a[i];return f;}
static int mk_seal(const uint8_t k[32],const uint8_t iv[12],const uint8_t*aad,size_t al,const uint8_t*in,size_t len,uint8_t*out,uint8_t tag[16],void*){for(size_t i=0;i<len;i++)out[i]=in[i]^k[i%32]^iv[i%12];memset(tag,0,16);tag[0]=aad_fp2(aad,al);return 0;}
static int mk_open(const uint8_t k[32],const uint8_t iv[12],const uint8_t*aad,size_t al,const uint8_t*in,size_t len,const uint8_t tag[16],uint8_t*out,void*){if(tag[0]!=aad_fp2(aad,al))return -1;for(size_t i=0;i<len;i++)out[i]=in[i]^k[i%32]^iv[i%12];return 0;}
static kerb_crypto_t MOCK={mk_rand,mk_sha256,mk_keygen,mk_sign,mk_seal,mk_open,nullptr};

static bool g_present2 = true;
static bool up2(void*){ return g_present2; }
static ctap2_cfg_t mk_cfg(void) {
    static uint8_t devkey[32]; for(int i=0;i<32;i++) devkey[i]=(uint8_t)(0xC0+i);
    static uint8_t aaguid[16]; memset(aaguid,0xAA,16);
    ctap2_cfg_t c; memset(&c,0,sizeof c);
    c.cy=&MOCK; c.devkey=devkey; c.aaguid=aaguid; c.user_present=up2; c.store=nullptr;
    return c;
}

// Build a makeCredential request (command byte + params CBOR) into out.
static uint16_t build_makecred_req(uint8_t *out, size_t cap) {
    CborEncoder enc, map;
    cbor_encoder_init(&enc, out+1, cap-1, 0);
    cbor_encoder_create_map(&enc, &map, 4);
    cbor_encode_int(&map, 1); uint8_t cdh[32]; memset(cdh,0xCC,32); cbor_encode_byte_string(&map, cdh, 32);
    cbor_encode_int(&map, 2); CborEncoder rp; cbor_encoder_create_map(&map,&rp,1);
    cbor_encode_text_stringz(&rp,"id"); cbor_encode_text_stringz(&rp,"example.com"); cbor_encoder_close_container(&map,&rp);
    cbor_encode_int(&map, 3); CborEncoder u; cbor_encoder_create_map(&map,&u,2);
    cbor_encode_text_stringz(&u,"id"); uint8_t uid[4]={1,2,3,4}; cbor_encode_byte_string(&u,uid,4);
    cbor_encode_text_stringz(&u,"name"); cbor_encode_text_stringz(&u,"a"); cbor_encoder_close_container(&map,&u);
    cbor_encode_int(&map, 4); CborEncoder arr; cbor_encoder_create_array(&map,&arr,1);
    CborEncoder e; cbor_encoder_create_map(&arr,&e,2);
    cbor_encode_text_stringz(&e,"alg"); cbor_encode_int(&e,-7);
    cbor_encode_text_stringz(&e,"type"); cbor_encode_text_stringz(&e,"public-key");
    cbor_encoder_close_container(&arr,&e); cbor_encoder_close_container(&map,&arr);
    cbor_encoder_close_container(&enc,&map);
    size_t n = cbor_encoder_get_buffer_size(&enc, out+1);
    out[0] = 0x01;                                   // makeCredential command
    return (uint16_t)(1+n);
}

static void test_makecred_packed(void) {
    g_present2 = true;
    ctap2_cfg_t cfg = mk_cfg();
    uint8_t req[256]; uint16_t rl = build_makecred_req(req, sizeof req);
    uint8_t out[512]; uint16_t n = ctap2_handle(&cfg, req, rl, out, sizeof out);
    TEST_ASSERT_EQUAL_UINT8(0x00, out[0]);
    CborParser p; CborValue map; TEST_ASSERT_EQUAL_INT(0, cbor_get_map(out+1, n-1, &p, &map));
    char fmt[16]; size_t fl = sizeof fmt;
    TEST_ASSERT_EQUAL_INT(0, cbor_map_text(&map, 1, fmt, &fl));
    TEST_ASSERT_EQUAL_STRING("packed", fmt);
    uint8_t ad[320]; size_t adl = sizeof ad;
    TEST_ASSERT_EQUAL_INT(0, cbor_map_bytes(&map, 2, ad, &adl));
    TEST_ASSERT_TRUE(ad[32] & 0x40);                 // AT flag
    TEST_ASSERT_TRUE(ad[32] & 0x01);                 // UP flag
}

static void test_makecred_denied(void) {
    g_present2 = false;
    ctap2_cfg_t cfg = mk_cfg();
    uint8_t req[256]; uint16_t rl = build_makecred_req(req, sizeof req);
    uint8_t out[512]; uint16_t n = ctap2_handle(&cfg, req, rl, out, sizeof out);
    (void)n;
    TEST_ASSERT_EQUAL_UINT8(0x27, out[0]);           // CTAP2_ERR_OPERATION_DENIED
}

// Make a credential, pull its credId out of authData, then assert with it.
static void test_getassert_nonresident(void) {
    g_present2 = true;
    ctap2_cfg_t cfg = mk_cfg();
    static uint32_t counter = 5; cfg.counter = &counter;

    uint8_t req[256]; uint16_t rl = build_makecred_req(req, sizeof req);
    uint8_t mkout[512]; uint16_t mn = ctap2_handle(&cfg, req, rl, mkout, sizeof mkout);
    TEST_ASSERT_EQUAL_UINT8(0x00, mkout[0]);
    CborParser p; CborValue map; cbor_get_map(mkout + 1, mn - 1, &p, &map);
    uint8_t ad[320]; size_t adl = sizeof ad; cbor_map_bytes(&map, 2, ad, &adl);
    // authData: rpIdHash(32)+flags(1)+count(4)+aaguid(16)+credIdLen(2)+credId+cose
    uint16_t cidLen = (uint16_t)((ad[37 + 16] << 8) | ad[37 + 16 + 1]);
    uint8_t *cid = ad + 37 + 16 + 2;

    // getAssertion {1: rpId, 2: cdh, 3: [{id: cid, type}]}
    uint8_t areq[256]; CborEncoder enc, amap;
    cbor_encoder_init(&enc, areq + 1, sizeof(areq) - 1, 0);
    cbor_encoder_create_map(&enc, &amap, 3);
    cbor_encode_int(&amap, 1); cbor_encode_text_stringz(&amap, "example.com");
    cbor_encode_int(&amap, 2); uint8_t cdh[32]; memset(cdh, 0xDD, 32); cbor_encode_byte_string(&amap, cdh, 32);
    cbor_encode_int(&amap, 3); CborEncoder arr; cbor_encoder_create_array(&amap, &arr, 1);
    CborEncoder e; cbor_encoder_create_map(&arr, &e, 2);
    cbor_encode_text_stringz(&e, "id");   cbor_encode_byte_string(&e, cid, cidLen);
    cbor_encode_text_stringz(&e, "type"); cbor_encode_text_stringz(&e, "public-key");
    cbor_encoder_close_container(&arr, &e); cbor_encoder_close_container(&amap, &arr);
    cbor_encoder_close_container(&enc, &amap);
    size_t an = cbor_encoder_get_buffer_size(&enc, areq + 1); areq[0] = 0x02;

    uint8_t out[512]; uint16_t n = ctap2_handle(&cfg, areq, (uint16_t)(an + 1), out, sizeof out);
    TEST_ASSERT_EQUAL_UINT8(0x00, out[0]);
    CborParser p2; CborValue m2; cbor_get_map(out + 1, n - 1, &p2, &m2);
    uint8_t ad2[64]; size_t ad2l = sizeof ad2; cbor_map_bytes(&m2, 2, ad2, &ad2l);
    TEST_ASSERT_TRUE(ad2[32] & 0x01);                // UP set
    TEST_ASSERT_FALSE(ad2[32] & 0x40);               // AT not set on assertion
    TEST_ASSERT_EQUAL_UINT8(6, ad2[36]);             // counter 5 -> 6
}

// An allowList credential id that isn't ours -> no credentials.
static void test_getassert_no_cred(void) {
    g_present2 = true;
    ctap2_cfg_t cfg = mk_cfg();
    uint8_t bogus[60]; memset(bogus, 0x55, 60);
    uint8_t areq[128]; CborEncoder enc, amap;
    cbor_encoder_init(&enc, areq + 1, sizeof(areq) - 1, 0);
    cbor_encoder_create_map(&enc, &amap, 3);
    cbor_encode_int(&amap, 1); cbor_encode_text_stringz(&amap, "example.com");
    cbor_encode_int(&amap, 2); uint8_t cdh[32]; memset(cdh, 0xDD, 32); cbor_encode_byte_string(&amap, cdh, 32);
    cbor_encode_int(&amap, 3); CborEncoder arr; cbor_encoder_create_array(&amap, &arr, 1);
    CborEncoder e; cbor_encoder_create_map(&arr, &e, 2);
    cbor_encode_text_stringz(&e, "id");   cbor_encode_byte_string(&e, bogus, 60);
    cbor_encode_text_stringz(&e, "type"); cbor_encode_text_stringz(&e, "public-key");
    cbor_encoder_close_container(&arr, &e); cbor_encoder_close_container(&amap, &arr);
    cbor_encoder_close_container(&enc, &amap);
    size_t an = cbor_encoder_get_buffer_size(&enc, areq + 1); areq[0] = 0x02;
    uint8_t out[128]; ctap2_handle(&cfg, areq, (uint16_t)(an + 1), out, sizeof out);
    TEST_ASSERT_EQUAL_UINT8(0x2E, out[0]);           // CTAP2_ERR_NO_CREDENTIALS
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_cbor_encode_small_map);
    RUN_TEST(test_write_and_parse);
    RUN_TEST(test_getinfo_has_fido2);
    RUN_TEST(test_cose_es256);
    RUN_TEST(test_authdata_flags_counter);
    RUN_TEST(test_makecred_packed);
    RUN_TEST(test_makecred_denied);
    RUN_TEST(test_getassert_nonresident);
    RUN_TEST(test_getassert_no_cred);
    return UNITY_END();
}


