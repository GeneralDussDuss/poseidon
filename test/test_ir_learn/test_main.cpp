/* Native Unity tests for ir_learn_decode.cpp — the pure RMT-symbol → mark/space
 * µs flattener. No hardware: feeds synthetic symbol pairs and checks the
 * flattened durations, the end-of-frame stop, and the truncation cap. */
#include <unity.h>
#include "../../src/ir_learn_decode.cpp"

/* A short NEC-ish frame: header 9000/4500, one bit 560/1690, trailer 560 then
 * an end-of-frame zero. */
static const ir_edge_pair_t FRAME[] = {
    {9000, 1, 4500, 0},
    { 560, 1, 1690, 0},
    { 560, 1,    0, 0},   /* second duration 0 = end of frame */
};

void setUp(void) {}
void tearDown(void) {}

static void test_flatten(void) {
    uint16_t out[16]; bool trunc = true;
    uint16_t n = ir_symbols_to_us(FRAME, 3, out, 16, &trunc);
    TEST_ASSERT_EQUAL_UINT16(5, n);
    TEST_ASSERT_FALSE(trunc);
    uint16_t expect[5] = {9000, 4500, 560, 1690, 560};
    for (int i = 0; i < 5; ++i) TEST_ASSERT_EQUAL_UINT16(expect[i], out[i]);
}

static void test_truncation(void) {
    uint16_t out[3]; bool trunc = false;
    uint16_t n = ir_symbols_to_us(FRAME, 3, out, 3, &trunc);
    TEST_ASSERT_EQUAL_UINT16(3, n);
    TEST_ASSERT_TRUE(trunc);
    TEST_ASSERT_EQUAL_UINT16(9000, out[0]);
    TEST_ASSERT_EQUAL_UINT16(4500, out[1]);
    TEST_ASSERT_EQUAL_UINT16( 560, out[2]);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_flatten);
    RUN_TEST(test_truncation);
    return UNITY_END();
}
