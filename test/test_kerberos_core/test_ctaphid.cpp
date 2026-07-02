#include <unity.h>
#include <string.h>
#include "ctaphid.h"
#include "ctaphid_dispatch.h"

void setUp(void) {}
void tearDown(void) {}

// Collect fragmented output packets.
static uint8_t g_out[64 * 4];
static int g_out_n;
static void sink(const uint8_t pkt[64], void *) {
    memcpy(g_out + g_out_n * 64, pkt, 64);
    g_out_n++;
}

static void test_single_packet_message(void) {
    // Build one init packet: CID=1, CMD=0x83 (MSG), bcnt=3, data "abc".
    uint8_t pkt[64] = {0};
    pkt[0]=0;pkt[1]=0;pkt[2]=0;pkt[3]=1;      // CID big-endian
    pkt[4]=0x83; pkt[5]=0x00; pkt[6]=0x03;    // CMD, BCNTH, BCNTL
    pkt[7]='a'; pkt[8]='b'; pkt[9]='c';
    ctaphid_assembler_t a; memset(&a,0,sizeof a);
    int r = ctaphid_feed(&a, pkt);
    TEST_ASSERT_EQUAL_INT(1, r);
    TEST_ASSERT_EQUAL_UINT32(1, a.cid);
    TEST_ASSERT_EQUAL_UINT8(0x83, a.cmd);
    TEST_ASSERT_EQUAL_UINT16(3, a.bcnt);
    TEST_ASSERT_EQUAL_MEMORY("abc", a.buf, 3);
}

static void test_multi_packet_reassembly(void) {
    // 60-byte payload spans an init packet (57) + one continuation (3).
    uint8_t payload[60];
    for (int i=0;i<60;i++) payload[i]=(uint8_t)i;
    uint8_t init[64]={0}; init[3]=2; init[4]=0x90; init[5]=0; init[6]=60;
    memcpy(init+7, payload, 57);
    uint8_t cont[64]={0}; cont[3]=2; cont[4]=0x00; // SEQ 0
    memcpy(cont+5, payload+57, 3);
    ctaphid_assembler_t a; memset(&a,0,sizeof a);
    TEST_ASSERT_EQUAL_INT(0, ctaphid_feed(&a, init));
    TEST_ASSERT_EQUAL_INT(1, ctaphid_feed(&a, cont));
    TEST_ASSERT_EQUAL_UINT16(60, a.bcnt);
    TEST_ASSERT_EQUAL_MEMORY(payload, a.buf, 60);
}

static void test_send_fragments_60_bytes(void) {
    uint8_t payload[60]; for(int i=0;i<60;i++) payload[i]=(uint8_t)(i+1);
    g_out_n=0;
    ctaphid_send(2, 0x90, payload, 60, sink, nullptr);
    TEST_ASSERT_EQUAL_INT(2, g_out_n);          // init + 1 continuation
    TEST_ASSERT_EQUAL_UINT8(0x90, g_out[4]);    // CMD in init
    TEST_ASSERT_EQUAL_UINT8(60, g_out[6]);      // BCNTL
    TEST_ASSERT_EQUAL_UINT8(0x00, g_out[64+4]); // SEQ 0 in continuation
}

// INIT on the broadcast channel returns nonce echo + a fresh non-zero CID.
static void test_init_allocates_channel(void) {
    uint8_t pkt[64]={0};
    pkt[0]=pkt[1]=pkt[2]=pkt[3]=0xFF;   // broadcast CID
    pkt[4]=0x86; pkt[5]=0; pkt[6]=8;     // INIT, 8-byte nonce
    for(int i=0;i<8;i++) pkt[7+i]=(uint8_t)(0xA0+i);
    g_out_n=0;
    ctaphid_ctx_t c; ctaphid_ctx_init(&c, sink, nullptr, nullptr, nullptr);
    ctaphid_dispatch(&c, pkt);
    TEST_ASSERT_EQUAL_INT(1, g_out_n);
    TEST_ASSERT_EQUAL_UINT8(0x86, g_out[4]);              // echoes INIT cmd
    TEST_ASSERT_EQUAL_MEMORY(pkt+7, g_out+7, 8);          // nonce echoed
    // new CID at resp offset 7+8 must be non-zero and not broadcast
    uint8_t *cidp = g_out + 7 + 8;
    TEST_ASSERT_TRUE(!(cidp[0]==0&&cidp[1]==0&&cidp[2]==0&&cidp[3]==0));
}

// PING echoes its payload back verbatim.
static void test_ping_echo(void) {
    uint8_t pkt[64]={0}; pkt[3]=7; pkt[4]=0x81; pkt[5]=0; pkt[6]=4;
    pkt[7]='p';pkt[8]='o';pkt[9]='n';pkt[10]='g';
    g_out_n=0;
    ctaphid_ctx_t c; ctaphid_ctx_init(&c, sink, nullptr, nullptr, nullptr);
    ctaphid_dispatch(&c, pkt);
    TEST_ASSERT_EQUAL_UINT8(0x81, g_out[4]);
    TEST_ASSERT_EQUAL_MEMORY("pong", g_out+7, 4);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_single_packet_message);
    RUN_TEST(test_multi_packet_reassembly);
    RUN_TEST(test_send_fragments_60_bytes);
    RUN_TEST(test_init_allocates_channel);
    RUN_TEST(test_ping_echo);
    return UNITY_END();
}
