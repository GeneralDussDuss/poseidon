#include <unity.h>
#include <string.h>
#include "kerb_crypto.h"
#include "keywrap.h"

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
static kerb_crypto_t MOCK = { mk_rand,nullptr,nullptr,nullptr, mk_seal, mk_open, nullptr };

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

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_wrap_unwrap_roundtrip);
    RUN_TEST(test_wrong_appid_fails);
    return UNITY_END();
}
