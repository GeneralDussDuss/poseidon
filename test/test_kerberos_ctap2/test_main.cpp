#include <unity.h>
#include <string.h>
#include "cbor.h"
#include "cbor_util.h"
#include "ctap2.h"
#include "cose.h"
#include "authdata.h"
#include "cred_store.h"

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
// clientPIN mock primitives. mk_keygen is fixed, so mock ECDH just needs to be
// deterministic (both platform + authenticator agree); real ECDH is mbedtls on device.
static int mk_ecdh(const uint8_t priv[32],const uint8_t pub[65],uint8_t out[32],void*){uint8_t b[97];memcpy(b,priv,32);memcpy(b+32,pub,65);return mk_sha256(b,97,out,nullptr);}
static int mk_hmac(const uint8_t*key,size_t kl,const uint8_t*msg,size_t ml,uint8_t out[32],void*){uint8_t b[600];size_t n=0;for(size_t i=0;i<32;i++)b[n++]=i<kl?key[i]:0;for(size_t i=0;i<ml&&n<sizeof b;i++)b[n++]=msg[i];return mk_sha256(b,n,out,nullptr);}
static int mk_aescbc(const uint8_t k[32],const uint8_t iv[16],int,const uint8_t*in,size_t len,uint8_t*out,void*){for(size_t i=0;i<len;i++)out[i]=in[i]^k[i%32]^iv[i%16];return 0;} // XOR: encrypt==decrypt
static kerb_crypto_t MOCK={mk_rand,mk_sha256,mk_keygen,mk_sign,mk_seal,mk_open,
                           mk_ecdh,mk_hmac,mk_aescbc,nullptr/*ctx*/};

// In-memory mock PIN store.
static uint8_t g_ph[16]; static uint8_t g_pr; static bool g_pset;
static int ps_load(pin_store*,uint8_t h[16],uint8_t*r){ if(!g_pset)return 0; memcpy(h,g_ph,16); *r=g_pr; return 1; }
static int ps_save(pin_store*,const uint8_t h[16],uint8_t r){ memcpy(g_ph,h,16); g_pr=r; g_pset=true; return 0; }
static int ps_saver(pin_store*,uint8_t r){ g_pr=r; return 0; }
static void ps_wipe(pin_store*){ g_pset=false; }
static pin_store PS = { ps_load, ps_save, ps_saver, ps_wipe };
static ctap2_pin_rt g_rt;

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

// makeCredential request with options.rk = true (keys 1,2,3,4,7).
static uint16_t build_makecred_rk(uint8_t *out, size_t cap) {
    CborEncoder enc, map;
    cbor_encoder_init(&enc, out+1, cap-1, 0);
    cbor_encoder_create_map(&enc, &map, 5);
    cbor_encode_int(&map, 1); uint8_t cdh[32]; memset(cdh,0xCC,32); cbor_encode_byte_string(&map, cdh, 32);
    cbor_encode_int(&map, 2); CborEncoder rp; cbor_encoder_create_map(&map,&rp,1);
    cbor_encode_text_stringz(&rp,"id"); cbor_encode_text_stringz(&rp,"example.com"); cbor_encoder_close_container(&map,&rp);
    cbor_encode_int(&map, 3); CborEncoder u; cbor_encoder_create_map(&map,&u,2);
    cbor_encode_text_stringz(&u,"id"); uint8_t uid[4]={9,8,7,6}; cbor_encode_byte_string(&u,uid,4);
    cbor_encode_text_stringz(&u,"name"); cbor_encode_text_stringz(&u,"bob"); cbor_encoder_close_container(&map,&u);
    cbor_encode_int(&map, 4); CborEncoder arr; cbor_encoder_create_array(&map,&arr,1);
    CborEncoder e; cbor_encoder_create_map(&arr,&e,2);
    cbor_encode_text_stringz(&e,"alg"); cbor_encode_int(&e,-7);
    cbor_encode_text_stringz(&e,"type"); cbor_encode_text_stringz(&e,"public-key");
    cbor_encoder_close_container(&arr,&e); cbor_encoder_close_container(&map,&arr);
    cbor_encode_int(&map, 7); CborEncoder opt; cbor_encoder_create_map(&map,&opt,1);
    cbor_encode_text_stringz(&opt,"rk"); cbor_encode_boolean(&opt,true); cbor_encoder_close_container(&map,&opt);
    cbor_encoder_close_container(&enc,&map);
    size_t n = cbor_encoder_get_buffer_size(&enc, out+1); out[0]=0x01;
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

static void test_cred_store_mem(void) {
    cred_store *s = cred_store_mem();
    cred_record a; memset(&a, 0, sizeof a);
    memset(a.rpIdHash, 0x11, 32); memset(a.id, 0xA1, 32); a.signCount = 1;
    cred_record b = a; memset(b.id, 0xB2, 32); b.signCount = 2;
    TEST_ASSERT_EQUAL_INT(0, s->add(s, &a));
    TEST_ASSERT_EQUAL_INT(0, s->add(s, &b));
    cred_record out; int total = 0;
    TEST_ASSERT_EQUAL_INT(0, s->find_by_rp(s, a.rpIdHash, &out, 0, &total));
    TEST_ASSERT_EQUAL_INT(2, total);
    TEST_ASSERT_EQUAL_INT(0, s->find_by_rp(s, a.rpIdHash, &out, 1, &total));  // b
    TEST_ASSERT_EQUAL_INT(0, s->update_counter(s, b.id, 99));
    TEST_ASSERT_EQUAL_INT(0, s->find_by_rp(s, a.rpIdHash, &out, 1, &total));
    TEST_ASSERT_EQUAL_UINT32(99, out.signCount);
    uint8_t other[32]; memset(other, 0x99, 32);
    TEST_ASSERT_NOT_EQUAL(0, s->find_by_rp(s, other, &out, 0, &total));
    TEST_ASSERT_EQUAL_INT(0, total);
}

// Resident make (rk=true) stores a record; discoverable getAssertion (empty
// allowList) finds it by rp and increments its own counter.
static void test_resident_make_and_discover(void) {
    g_present2 = true;
    ctap2_cfg_t cfg = mk_cfg();
    cfg.store = cred_store_mem();
    uint8_t req[256]; uint16_t rl = build_makecred_rk(req, sizeof req);
    uint8_t mkout[512]; uint16_t mn = ctap2_handle(&cfg, req, rl, mkout, sizeof mkout);
    TEST_ASSERT_EQUAL_UINT8(0x00, mkout[0]); (void)mn;
    uint8_t rpHash[32]; mk_sha256((const uint8_t *)"example.com", 11, rpHash, nullptr);
    cred_record r; int total = 0;
    TEST_ASSERT_EQUAL_INT(0, cfg.store->find_by_rp(cfg.store, rpHash, &r, 0, &total));
    TEST_ASSERT_EQUAL_INT(1, total);

    // getAssertion with no allowList (keys 1,2 only) -> discoverable
    uint8_t areq[128]; CborEncoder enc, amap;
    cbor_encoder_init(&enc, areq + 1, sizeof(areq) - 1, 0);
    cbor_encoder_create_map(&enc, &amap, 2);
    cbor_encode_int(&amap, 1); cbor_encode_text_stringz(&amap, "example.com");
    cbor_encode_int(&amap, 2); uint8_t cdh[32]; memset(cdh, 0xEE, 32); cbor_encode_byte_string(&amap, cdh, 32);
    cbor_encoder_close_container(&enc, &amap);
    size_t an = cbor_encoder_get_buffer_size(&enc, areq + 1); areq[0] = 0x02;
    uint8_t out[512]; uint16_t n = ctap2_handle(&cfg, areq, (uint16_t)(an + 1), out, sizeof out);
    TEST_ASSERT_EQUAL_UINT8(0x00, out[0]);
    CborParser p; CborValue m; cbor_get_map(out + 1, n - 1, &p, &m);
    uint8_t ad[64]; size_t adl = sizeof ad; cbor_map_bytes(&m, 2, ad, &adl);
    TEST_ASSERT_TRUE(ad[32] & 0x01);                 // UP
    TEST_ASSERT_EQUAL_UINT8(1, ad[36]);              // signCount 0 -> 1
    cfg.store->find_by_rp(cfg.store, rpHash, &r, 0, &total);
    TEST_ASSERT_EQUAL_UINT32(1, r.signCount);        // persisted in the store
}

// ---- clientPIN flow (pinUvAuthProtocol 1) ----

// Encode a COSE_Key {1:2,3:-25,-1:1,-2:X,-3:Y} into `parent`.
static void enc_cose_key(CborEncoder *parent, const uint8_t pub[65]) {
    CborEncoder m; cbor_encoder_create_map(parent, &m, 5);
    cbor_encode_int(&m,1); cbor_encode_int(&m,2);
    cbor_encode_int(&m,3); cbor_encode_int(&m,-25);
    cbor_encode_int(&m,-1); cbor_encode_int(&m,1);
    cbor_encode_int(&m,-2); cbor_encode_byte_string(&m,pub+1,32);
    cbor_encode_int(&m,-3); cbor_encode_byte_string(&m,pub+33,32);
    cbor_encoder_close_container(parent,&m);
}

// getKeyAgreement -> authenticator KA public key (0x04||X||Y).
static void get_auth_ka(ctap2_cfg_t *cfg, uint8_t authPub[65]) {
    uint8_t req[64]; req[0]=0x06;
    CborEncoder e,m; cbor_encoder_init(&e,req+1,sizeof req-1,0);
    cbor_encoder_create_map(&e,&m,2);
    cbor_encode_int(&m,1); cbor_encode_int(&m,1);   // pinUvAuthProtocol 1
    cbor_encode_int(&m,2); cbor_encode_int(&m,2);   // subCommand getKeyAgreement
    cbor_encoder_close_container(&e,&m);
    uint16_t rl=(uint16_t)(1+cbor_encoder_get_buffer_size(&e,req+1));
    uint8_t out[256]; uint16_t n=ctap2_handle(cfg,req,rl,out,sizeof out);
    TEST_ASSERT_EQUAL_UINT8(0x00,out[0]);
    CborParser p; CborValue map,ka;
    TEST_ASSERT_EQUAL_INT(0,cbor_get_map(out+1,n-1,&p,&map));
    TEST_ASSERT_EQUAL_INT(0,cbor_map_enter(&map,1,&ka));
    uint8_t x[32],y[32]; size_t xl=32,yl=32;
    TEST_ASSERT_EQUAL_INT(0,cbor_map_bytes(&ka,-2,x,&xl));
    TEST_ASSERT_EQUAL_INT(0,cbor_map_bytes(&ka,-3,y,&yl));
    authPub[0]=0x04; memcpy(authPub+1,x,32); memcpy(authPub+33,y,32);
}

// Derive the platform-side sharedSecret against the authenticator KA key.
static void platform_ss(const uint8_t platPriv[32], const uint8_t authPub[65], uint8_t ss[32]) {
    uint8_t sx[32]; mk_ecdh(platPriv,authPub,sx,nullptr); mk_sha256(sx,32,ss,nullptr);
}

static bool getinfo_clientpin(ctap2_cfg_t *cfg) {
    uint8_t rq[1]={0x04}; uint8_t o[256]; uint16_t on=ctap2_handle(cfg,rq,1,o,sizeof o);
    CborParser p; CborValue mm,opts,cp; cbor_get_map(o+1,on-1,&p,&mm);
    if (cbor_map_enter(&mm,4,&opts)) return false;
    if (cbor_value_map_find_value(&opts,"clientPin",&cp)!=CborNoError || !cbor_value_is_boolean(&cp)) return false;
    bool b=false; cbor_value_get_boolean(&cp,&b); return b;
}

static void reset_pin_state(void){ g_pset=false; g_pr=0; memset(&g_rt,0,sizeof g_rt); }
static ctap2_cfg_t pin_cfg(void){ ctap2_cfg_t c=mk_cfg(); c.pin=&PS; c.pin_rt=&g_rt; return c; }

// setPIN "1234" via the real protocol, then confirm getInfo flips clientPin true.
static void test_clientpin_setpin(void) {
    reset_pin_state();
    ctap2_cfg_t cfg = pin_cfg();
    TEST_ASSERT_FALSE(getinfo_clientpin(&cfg));            // no PIN yet

    uint8_t authPub[65]; get_auth_ka(&cfg, authPub);
    uint8_t platPriv[32],platPub[65]; mk_keygen(platPriv,platPub,nullptr);
    uint8_t ss[32]; platform_ss(platPriv,authPub,ss);

    uint8_t pad[64]; memset(pad,0,64); memcpy(pad,"1234",4);
    uint8_t iv[16]={0}; uint8_t npe[64]; mk_aescbc(ss,iv,1,pad,64,npe,nullptr);
    uint8_t mac[32]; mk_hmac(ss,32,npe,64,mac,nullptr);

    uint8_t req[256]; req[0]=0x06;
    CborEncoder e,m; cbor_encoder_init(&e,req+1,sizeof req-1,0);
    cbor_encoder_create_map(&e,&m,5);
    cbor_encode_int(&m,1); cbor_encode_int(&m,1);
    cbor_encode_int(&m,2); cbor_encode_int(&m,3);
    cbor_encode_int(&m,3); enc_cose_key(&m,platPub);
    cbor_encode_int(&m,4); cbor_encode_byte_string(&m,mac,16);
    cbor_encode_int(&m,5); cbor_encode_byte_string(&m,npe,64);
    cbor_encoder_close_container(&e,&m);
    uint16_t rl=(uint16_t)(1+cbor_encoder_get_buffer_size(&e,req+1));
    uint8_t out[256]; ctap2_handle(&cfg,req,rl,out,sizeof out);
    TEST_ASSERT_EQUAL_UINT8(0x00,out[0]);                  // setPIN ok
    TEST_ASSERT_TRUE(g_pset);
    TEST_ASSERT_TRUE(getinfo_clientpin(&cfg));             // now advertises clientPin true
}

// Build a getPinToken (subCmd 5) request with the given pinHashEnc.
static uint16_t build_gettoken(uint8_t *req, const uint8_t platPub[65], const uint8_t phe[16]) {
    req[0]=0x06; CborEncoder e,m; cbor_encoder_init(&e,req+1,240,0);
    cbor_encoder_create_map(&e,&m,4);
    cbor_encode_int(&m,1); cbor_encode_int(&m,1);
    cbor_encode_int(&m,2); cbor_encode_int(&m,5);
    cbor_encode_int(&m,3); enc_cose_key(&m,platPub);
    cbor_encode_int(&m,6); cbor_encode_byte_string(&m,phe,16);
    cbor_encoder_close_container(&e,&m);
    return (uint16_t)(1+cbor_encoder_get_buffer_size(&e,req+1));
}

// Right PIN -> token issued; wrong PIN -> PIN_INVALID and retry decrement.
static void test_clientpin_token(void) {
    reset_pin_state();
    ctap2_cfg_t cfg = pin_cfg();
    // set the PIN first (reuse the flow)
    { uint8_t ap[65]; get_auth_ka(&cfg,ap); uint8_t pp[32],pub[65]; mk_keygen(pp,pub,nullptr);
      uint8_t ss[32]; platform_ss(pp,ap,ss);
      uint8_t pad[64]; memset(pad,0,64); memcpy(pad,"1234",4);
      uint8_t iv[16]={0}; uint8_t npe[64]; mk_aescbc(ss,iv,1,pad,64,npe,nullptr);
      uint8_t mac[32]; mk_hmac(ss,32,npe,64,mac,nullptr);
      uint8_t req[256]; req[0]=0x06; CborEncoder e,m; cbor_encoder_init(&e,req+1,240,0);
      cbor_encoder_create_map(&e,&m,5);
      cbor_encode_int(&m,1);cbor_encode_int(&m,1); cbor_encode_int(&m,2);cbor_encode_int(&m,3);
      cbor_encode_int(&m,3);enc_cose_key(&m,pub); cbor_encode_int(&m,4);cbor_encode_byte_string(&m,mac,16);
      cbor_encode_int(&m,5);cbor_encode_byte_string(&m,npe,64); cbor_encoder_close_container(&e,&m);
      uint16_t rl=(uint16_t)(1+cbor_encoder_get_buffer_size(&e,req+1));
      uint8_t o[256]; ctap2_handle(&cfg,req,rl,o,sizeof o); TEST_ASSERT_EQUAL_UINT8(0,o[0]); }

    uint8_t authPub[65]; get_auth_ka(&cfg,authPub);
    uint8_t platPriv[32],platPub[65]; mk_keygen(platPriv,platPub,nullptr);
    uint8_t ss[32]; platform_ss(platPriv,authPub,ss);
    uint8_t iv[16]={0};

    // correct PIN
    uint8_t ph[32]; mk_sha256((const uint8_t*)"1234",4,ph,nullptr);
    uint8_t phe[16]; mk_aescbc(ss,iv,1,ph,16,phe,nullptr);
    uint8_t req[256]; uint16_t rl=build_gettoken(req,platPub,phe);
    uint8_t out[256]; ctap2_handle(&cfg,req,rl,out,sizeof out);
    TEST_ASSERT_EQUAL_UINT8(0x00,out[0]);                  // token issued
    TEST_ASSERT_TRUE(g_rt.token_set);
    TEST_ASSERT_EQUAL_UINT8(8,g_pr);                       // retries restored to 8

    // wrong PIN -> 0x31 and retries 8 -> 7
    get_auth_ka(&cfg,authPub); platform_ss(platPriv,authPub,ss);
    uint8_t bad[32]; mk_sha256((const uint8_t*)"9999",4,bad,nullptr);
    uint8_t bphe[16]; mk_aescbc(ss,iv,1,bad,16,bphe,nullptr);
    rl=build_gettoken(req,platPub,bphe);
    ctap2_handle(&cfg,req,rl,out,sizeof out);
    TEST_ASSERT_EQUAL_UINT8(0x31,out[0]);                  // CTAP2_ERR_PIN_INVALID
    TEST_ASSERT_EQUAL_UINT8(7,g_pr);
}

// authenticatorReset wipes the PIN (with user presence).
static void test_reset_wipes_pin(void) {
    reset_pin_state();
    ctap2_cfg_t cfg = pin_cfg(); g_present2 = true;
    g_pset = true; memset(g_ph,0x22,16); g_pr = 5;         // pretend a PIN is set
    uint8_t req[1]={0x07}; uint8_t out[8];
    ctap2_handle(&cfg,req,1,out,sizeof out);
    TEST_ASSERT_EQUAL_UINT8(0x00,out[0]);
    TEST_ASSERT_FALSE(g_pset);                             // PIN wiped
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
    RUN_TEST(test_cred_store_mem);
    RUN_TEST(test_resident_make_and_discover);
    RUN_TEST(test_clientpin_setpin);
    RUN_TEST(test_clientpin_token);
    RUN_TEST(test_reset_wipes_pin);
    return UNITY_END();
}


