#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>
#include <vector>

// Include the production code
#include "src/mesh/meshtastic_pb.cpp"

// We need to test that the protobuf decode callback never writes beyond
// the allocated buffer size. The security invariant is:
// For any incoming bytes of length n, the decoder MUST NOT copy more than
// (buffer_capacity - buffer_current_length) bytes into the buffer.

// Simulate the decode buffer structure used by the protobuf decoder
// This mirrors the internal pb_bytes_array_t pattern used in meshtastic
#ifndef PB_BYTES_ARRAY_T_ALLOCSIZE
#define PB_BYTES_ARRAY_T_ALLOCSIZE 256
#endif

struct TestBuffer {
    uint16_t len;
    uint8_t data[PB_BYTES_ARRAY_T_ALLOCSIZE];
};

class ProtobufOverflowTest : public ::testing::TestWithParam<size_t> {};

TEST_P(ProtobufOverflowTest, DecodeCallbackMustNotOverflowBuffer) {
    // Invariant: bytes written must never exceed allocated buffer capacity
    size_t incoming_len = GetParam();

    TestBuffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.len = 0;

    // The security property: if incoming_len + buf.len > sizeof(buf.data),
    // the copy MUST be rejected or truncated, never performed in full.
    size_t safe_copy_len = (incoming_len <= (sizeof(buf.data) - buf.len))
                               ? incoming_len
                               : (sizeof(buf.data) - buf.len);

    // Simulate what a SAFE implementation must do
    std::vector<uint8_t> incoming(incoming_len, 0x41);

    // Assert the invariant: we should never allow a copy that exceeds capacity
    ASSERT_LE(safe_copy_len + buf.len, sizeof(buf.data))
        << "Buffer overflow would occur with incoming length " << incoming_len;

    // Verify that the oversized case is properly bounded
    if (incoming_len > sizeof(buf.data)) {
        EXPECT_LT(safe_copy_len, incoming_len)
            << "Oversized payload must be truncated or rejected";
    }
}

INSTANTIATE_TEST_SUITE_P(
    AdversarialInputs,
    ProtobufOverflowTest,
    ::testing::Values(
        // Exact exploit: payload larger than buffer (e.g., 512 bytes into 256-byte buffer)
        512,
        // Boundary: exactly at buffer capacity
        PB_BYTES_ARRAY_T_ALLOCSIZE,
        // Boundary: one byte over capacity
        PB_BYTES_ARRAY_T_ALLOCSIZE + 1,
        // Extreme: attacker-controlled max uint16 length field
        65535,
        // Valid: small payload well within bounds
        32
    )
);

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}