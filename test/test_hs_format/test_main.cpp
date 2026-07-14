/* Native Unity tests for the REAL hs_format.cpp — the hashcat-22000 line
 * builder. Uses a synthetic M2 802.1X frame with recognizable byte patterns so
 * the three historical bugs are each pinned: MIC must be pulled from the frame,
 * the MIC bytes must be zeroed in the emitted EAPOL field, and the ESSID comes
 * from the caller, not the (empty) wire field. */
#include <unity.h>
#include <string.h>
#include "../../src/hs_format.cpp"

/* Synthetic 121-byte M2 802.1X frame. Offsets (from the 802.1X version byte):
 *   [0]=ver [1]=type [2..3]=len [4]=descriptor [5..6]=key info
 *   [9..16]=replay [17..48]=SNonce(0xAA) [81..96]=MIC(0xBB) [99..120]=keydata(0xCC) */
#define FRAME_LEN 121
static uint8_t s_frame[FRAME_LEN];
static void build_frame(void) {
    memset(s_frame, 0x00, sizeof(s_frame));
    s_frame[0] = 0x02;            /* 802.1X version 2      */
    s_frame[1] = 0x03;            /* type = EAPOL-Key      */
    s_frame[2] = 0x00; s_frame[3] = 0x75;
    s_frame[4] = 0x02;            /* RSN key descriptor    */
    s_frame[5] = 0x01; s_frame[6] = 0x0a;   /* key info: MIC set */
    memset(s_frame + 17, 0xAA, 32);         /* SNonce            */
    memset(s_frame + 81, 0xBB, 16);         /* Key MIC           */
    memset(s_frame + 99, 0xCC, 22);         /* key data (RSN IE) */
}

static const uint8_t BSSID[6]  = {0x00,0x11,0x22,0x33,0x44,0x55};
static const uint8_t STA[6]    = {0x66,0x77,0x88,0x99,0xaa,0xbb};
static uint8_t ANONCE[32];

static char s_out[512];
static char *s_tok[12];
static int   s_ntok;
static void run_format(void) {
    memset(ANONCE, 0xDD, sizeof(ANONCE));
    build_frame();
    int n = hs_format_22000(s_out, sizeof(s_out), BSSID, STA, ANONCE,
                            s_frame, FRAME_LEN, "TestNet");
    TEST_ASSERT_GREATER_THAN(0, n);
    /* Split on '*' (destructive) into s_tok. */
    s_ntok = 0;
    char *p = strtok(s_out, "*");
    while (p && s_ntok < 12) { s_tok[s_ntok++] = p; p = strtok(NULL, "*"); }
}

void setUp(void) { run_format(); }
void tearDown(void) {}

/* Nine fields: WPA 02 mic bssid sta essid anonce eapol messagepair. */
static void test_field_layout(void) {
    TEST_ASSERT_EQUAL_INT(9, s_ntok);
    TEST_ASSERT_EQUAL_STRING("WPA", s_tok[0]);
    TEST_ASSERT_EQUAL_STRING("02",  s_tok[1]);
    TEST_ASSERT_EQUAL_STRING("00",  s_tok[8]);   /* message-pair M1+M2 */
}

/* MIC field is the 16 MIC bytes (0xBB) from offset 81, hex. */
static void test_mic_extracted(void) {
    TEST_ASSERT_EQUAL_STRING("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", s_tok[2]);
}

static void test_macs(void) {
    TEST_ASSERT_EQUAL_STRING("001122334455", s_tok[3]);
    TEST_ASSERT_EQUAL_STRING("66778899aabb", s_tok[4]);
}

/* ESSID hex of "TestNet" — proves it comes from the caller arg. */
static void test_essid(void) {
    TEST_ASSERT_EQUAL_STRING("546573744e6574", s_tok[5]);
}

static void test_anonce(void) {
    char expect[65]; for (int i = 0; i < 32; ++i) memcpy(expect + i*2, "dd", 2);
    expect[64] = '\0';
    TEST_ASSERT_EQUAL_STRING(expect, s_tok[6]);
}

/* EAPOL field: whole frame hex (242 chars) with the 16 MIC bytes ZEROED. */
static void test_eapol_mic_zeroed(void) {
    TEST_ASSERT_EQUAL_INT(FRAME_LEN * 2, (int)strlen(s_tok[7]));
    const char *eap = s_tok[7];
    /* MIC region (offset 81) is 16 zero bytes = 32 '0' chars at char 162. */
    for (int i = 0; i < 32; ++i) TEST_ASSERT_EQUAL_CHAR('0', eap[162 + i]);
    /* SNonce region (offset 17) survives as 0xAA. */
    TEST_ASSERT_EQUAL_CHAR('a', eap[34]);
    TEST_ASSERT_EQUAL_CHAR('a', eap[35]);
}

/* Truncation guard: a tiny buffer returns -1, does not overflow. */
static void test_truncation_guard(void) {
    char tiny[8];
    int n = hs_format_22000(tiny, sizeof(tiny), BSSID, STA, ANONCE,
                            s_frame, FRAME_LEN, "TestNet");
    TEST_ASSERT_EQUAL_INT(-1, n);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_field_layout);
    RUN_TEST(test_mic_extracted);
    RUN_TEST(test_macs);
    RUN_TEST(test_essid);
    RUN_TEST(test_anonce);
    RUN_TEST(test_eapol_mic_zeroed);
    RUN_TEST(test_truncation_guard);
    return UNITY_END();
}
