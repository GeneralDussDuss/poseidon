#include <unity.h>
#include <string.h>
#include "ctaphid.h"

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

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_single_packet_message);
    RUN_TEST(test_multi_packet_reassembly);
    RUN_TEST(test_send_fragments_60_bytes);
    return UNITY_END();
}
