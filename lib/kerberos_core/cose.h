#pragma once
#include <stdint.h>
#include <stddef.h>

// Encode a COSE_Key for an ES256 (P-256) public key, from an uncompressed EC
// point (pub[0]=0x04, pub[1..32]=X, pub[33..64]=Y). Canonical map, keys in the
// order 1, 3, -1, -2, -3:  {1:2 (EC2), 3:-7 (ES256), -1:1 (P-256), -2:X, -3:Y}.
// Returns the encoded length.
size_t cose_es256_from_pubkey(const uint8_t pub[65], uint8_t *out, size_t cap);
