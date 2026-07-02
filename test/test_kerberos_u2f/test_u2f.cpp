#include <unity.h>
#include <string.h>
#include "kerb_crypto.h"
#include "keywrap.h"
#include "u2f.h"

void setUp(void) {}
void tearDown(void) {}

// --- Reversible mock cipher: XOR keystream, tag carries an AAD fingerprint so
// --- open() can detect a wrong app id (the GCM AAD-binding behaviour we rely on).
static int mk_rand(uint8_t*d,size_t n,void*){ for(size_t i=0;i<n;i++) d[i]=(uint8_t)(0x11+i); return 0; }
static uint8_t aad_fp(const uint8_t*a,size_t n){ uint8_t f=0; for(size_t i=0;i<n;i++) f^=a[i]; return f; }
static int mk_seal(const uint8_t k[32],const uint8_t iv[12],const uint8_t*aad,size_t al,
                   const uint8_t*in,size_t len,uint8_t*out,uint8_t tag[16],void*){
    for(size_t i=0;i<len;i++) out[i]=in[i]^k[i%32]^iv[i%12];
    memset(tag,0,16); tag[0]=aad_fp(aad,al); return 0;
}
static int mk_open(const uint8_t k[32],const uint8_t iv[12],const uint8_t*aad,size_t al,
                   const uint8_t*in,size_t len,const uint8_t tag[16],uint8_t*out,void*){
    if(tag[0]!=aad_fp(aad,al)) return -1;         // wrong app id
    for(size_t i=0;i<len;i++) out[i]=in[i]^k[i%32]^iv[i%12];
    return 0;
}
// Deterministic (non-cryptographic) stand-ins so the U2F logic is exercised
// without real crypto. Real crypto correctness is validated on-device.
static int mk_sha256(const uint8_t*m,size_t n,uint8_t out[32],void*){
    for(int i=0;i<32;i++){ uint8_t v=(uint8_t)i; for(size_t j=i;j<n;j+=32) v^=m[j]; out[i]=v; } return 0;
}
static int mk_keygen(uint8_t priv[32],uint8_t pub[65],void*){
    for(int i=0;i<32;i++) priv[i]=(uint8_t)(0x30+i);
    pub[0]=0x04; for(int i=1;i<65;i++) pub[i]=(uint8_t)(0x50+i); return 0;
}
static int mk_sign(const uint8_t priv[32],const uint8_t*m,size_t n,
                   uint8_t*sig,size_t*sl,void*){
    (void)priv;(void)m;(void)n;
    static const uint8_t der[]={0x30,0x06,0x02,0x01,0x01,0x02,0x01,0x01};
    memcpy(sig,der,sizeof der); *sl=sizeof der; return 0;
}
static kerb_crypto_t MOCK = { mk_rand, mk_sha256, mk_keygen, mk_sign, mk_seal, mk_open, nullptr };

// --- U2F config helper ---
static bool g_present = true;
static bool up_cb(void*){ return g_present; }
static u2f_cfg_t make_test_cfg(bool present) {
    g_present = present;
    static uint8_t devkey[32];  for(int i=0;i<32;i++) devkey[i]=(uint8_t)(0xC0+i);
    static uint8_t attpriv[32]; for(int i=0;i<32;i++) attpriv[i]=(uint8_t)(0x70+i);
    static const uint8_t attcert[]={0x30,0x03,0x01,0x02,0x03};   // stand-in DER
    static uint32_t counter = 0;
    u2f_cfg_t c;
    c.cy=&MOCK; c.devkey=devkey; c.att_cert=attcert; c.att_cert_len=sizeof attcert;
    c.att_priv=attpriv; c.counter=&counter; c.user_present=up_cb; c.ui=nullptr;
    return c;
}

static void test_wrap_unwrap_roundtrip(void) {
    uint8_t devkey[32]; for(int i=0;i<32;i++) devkey[i]=(uint8_t)i;
    uint8_t priv[32];   for(int i=0;i<32;i++) priv[i]=(uint8_t)(0x40+i);
    uint8_t appid[32];  for(int i=0;i<32;i++) appid[i]=(uint8_t)(0x80+i);
    uint8_t handle[128]; size_t hl=0;
    TEST_ASSERT_EQUAL_INT(0, kw_wrap(&MOCK, devkey, priv, appid, handle, &hl));
    TEST_ASSERT_EQUAL_INT(60, (int)hl);
    uint8_t got[32];
    TEST_ASSERT_EQUAL_INT(0, kw_unwrap(&MOCK, devkey, handle, hl, appid, got));
    TEST_ASSERT_EQUAL_MEMORY(priv, got, 32);
}

static void test_wrong_appid_fails(void) {
    uint8_t devkey[32]={0}, priv[32]={1}, appid[32]={2}, bad[32]={3};
    uint8_t handle[128]; size_t hl=0; uint8_t got[32];
    kw_wrap(&MOCK, devkey, priv, appid, handle, &hl);
    TEST_ASSERT_NOT_EQUAL(0, kw_unwrap(&MOCK, devkey, handle, hl, bad, got));
}

static void test_version_returns_u2f_v2(void) {
    u2f_cfg_t cfg = make_test_cfg(true);
    uint8_t apdu[] = {0x00,0x03,0x00,0x00, 0,0,0, 0,0};  // Version, Lc=0
    uint8_t out[256];
    uint16_t n = u2f_handle(&cfg, apdu, sizeof apdu, out, sizeof out);
    TEST_ASSERT_EQUAL_MEMORY("U2F_V2", out, 6);
    TEST_ASSERT_EQUAL_UINT8(0x90, out[n-2]);
    TEST_ASSERT_EQUAL_UINT8(0x00, out[n-1]);
}

static void test_register_shape(void) {
    u2f_cfg_t cfg = make_test_cfg(true);
    uint8_t data[64]; memset(data,0xAB,64);              // challenge(32)+appid(32)
    uint8_t apdu[7+64+2]={0}; apdu[1]=0x01; apdu[6]=64;
    memcpy(apdu+7, data, 64);
    uint8_t out[512];
    uint16_t n = u2f_handle(&cfg, apdu, sizeof apdu, out, sizeof out);
    TEST_ASSERT_EQUAL_UINT8(0x05, out[0]);              // reserved byte
    TEST_ASSERT_EQUAL_UINT8(0x04, out[1]);              // pubkey uncompressed prefix
    TEST_ASSERT_EQUAL_UINT8(0x90, out[n-2]);            // SW success
}

static void test_register_denied_without_presence(void) {
    u2f_cfg_t cfg = make_test_cfg(false);
    uint8_t apdu[7+64+2]={0}; apdu[1]=0x01; apdu[6]=64;
    uint8_t out[512];
    uint16_t n = u2f_handle(&cfg, apdu, sizeof apdu, out, sizeof out);
    TEST_ASSERT_EQUAL_UINT8(0x69, out[n-2]);            // 0x6985 condition not satisfied
    TEST_ASSERT_EQUAL_UINT8(0x85, out[n-1]);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_wrap_unwrap_roundtrip);
    RUN_TEST(test_wrong_appid_fails);
    RUN_TEST(test_version_returns_u2f_v2);
    RUN_TEST(test_register_shape);
    RUN_TEST(test_register_denied_without_presence);
    return UNITY_END();
}
