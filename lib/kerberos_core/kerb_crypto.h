#pragma once
#include <stdint.h>
#include <stddef.h>

// Injected so the core is testable with a mock and runs mbedtls on device.
typedef struct kerb_crypto_s {
    // Fill dst with n random bytes. Returns 0 on success.
    int (*rand)(uint8_t *dst, size_t n, void *ctx);
    // SHA-256 of msg[len] into out[32]. Returns 0 on success.
    int (*sha256)(const uint8_t *msg, size_t len, uint8_t out[32], void *ctx);
    // Generate a P256 keypair. priv[32] raw scalar, pub[65] uncompressed 0x04||X||Y.
    int (*p256_keygen)(uint8_t priv[32], uint8_t pub[65], void *ctx);
    // ECDSA-P256 sign of the SHA-256 of msg[len] with priv[32].
    // Writes a DER ECDSA signature to sig, sets *sig_len. sig has room for 72 bytes.
    int (*p256_sign)(const uint8_t priv[32], const uint8_t *msg, size_t len,
                     uint8_t *sig, size_t *sig_len, void *ctx);
    // AES-256-GCM used by keywrap. key[32], iv[12]. Encrypt: in->out, tag[16] out.
    int (*aes_gcm_seal)(const uint8_t key[32], const uint8_t iv[12],
                        const uint8_t *aad, size_t aad_len,
                        const uint8_t *in, size_t len, uint8_t *out, uint8_t tag[16], void *ctx);
    // Decrypt: verifies tag, in->out. Returns 0 on success, nonzero on auth failure.
    int (*aes_gcm_open)(const uint8_t key[32], const uint8_t iv[12],
                        const uint8_t *aad, size_t aad_len,
                        const uint8_t *in, size_t len, const uint8_t tag[16], uint8_t *out, void *ctx);
    void *ctx;
} kerb_crypto_t;
