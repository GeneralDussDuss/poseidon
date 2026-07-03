#pragma once
#include <stdint.h>
#include <stddef.h>

// authenticatorData flags.
#define AD_FLAG_UP 0x01   // user present
#define AD_FLAG_UV 0x04   // user verified (Phase 3)
#define AD_FLAG_AT 0x40   // attested credential data included

// attestedCredentialData = aaguid(16) || credIdLen(2, big-endian) || credId || cosePubKey.
size_t att_cred_data(const uint8_t aaguid[16], const uint8_t *credId, uint16_t credIdLen,
                     const uint8_t *cosePub, size_t coseLen, uint8_t *out, size_t cap);

// authenticatorData = rpIdHash(32) || flags(1) || signCount(4, big-endian) || [attCredData].
// Pass attLen 0 for an assertion (no attested credential data).
size_t authdata_build(const uint8_t rpIdHash[32], uint8_t flags, uint32_t signCount,
                      const uint8_t *attCredData, size_t attLen, uint8_t *out, size_t cap);
