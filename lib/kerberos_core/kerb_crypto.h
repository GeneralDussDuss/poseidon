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

    // ---- CTAP2 clientPIN / pinUvAuthProtocol-1 primitives ----
    // ECDH P-256: shared_x = the X-coordinate of (priv * peer_pub). peer_pub is
    // 65 bytes uncompressed (0x04||X||Y). Returns 0 on success. The PIN protocol
    // derives sharedSecret = SHA-256(shared_x).
    int (*ecdh_p256)(const uint8_t priv[32], const uint8_t peer_pub[65],
                     uint8_t shared_x[32], void *ctx);
    // HMAC-SHA-256(key[key_len], msg[len]) -> out[32]. Returns 0 on success.
    int (*hmac_sha256)(const uint8_t *key, size_t key_len,
                       const uint8_t *msg, size_t len, uint8_t out[32], void *ctx);
    // AES-256-CBC, NO padding (len must be a multiple of 16). encrypt != 0
    // encrypts, == 0 decrypts. iv[16] is consumed (not mutated by the caller's
    // copy). in and out may be the same buffer. Returns 0 on success.
    int (*aes_cbc)(const uint8_t key[32], const uint8_t iv[16], int encrypt,
                   const uint8_t *in, size_t len, uint8_t *out, void *ctx);

    void *ctx;
} kerb_crypto_t;
