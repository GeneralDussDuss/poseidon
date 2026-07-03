#pragma once
#include "kerb_crypto.h"

struct cred_store;   // forward; resident credential storage (Task 6)

// Everything a CTAP2 command needs. Reuses the Phase 1 crypto vtable, device
// wrapping key, attestation identity, and on-device presence callback.
typedef struct {
    const kerb_crypto_t *cy;
    const uint8_t *devkey;         // 32-byte wrapping key (keywrap)
    const uint8_t *aaguid;         // 16 bytes
    const uint8_t *att_cert;       // DER (unused by self attestation, kept for parity)
    uint16_t       att_cert_len;
    const uint8_t *att_priv;       // 32-byte attestation key (unused by self attestation)
    bool         (*user_present)(void *ui);
    void          *ui;
    struct cred_store *store;      // resident credentials; may be null
} ctap2_cfg_t;

enum {
    CTAP2_MAKE_CRED  = 0x01,
    CTAP2_GET_ASSERT = 0x02,
    CTAP2_GET_INFO   = 0x04,
};

// Handle one CTAP2 request. out[0] is the CTAP2 status byte (0x00 = success),
// followed by response CBOR. Returns total length written.
uint16_t ctap2_handle(const ctap2_cfg_t *cfg, const uint8_t *req, uint16_t len,
                      uint8_t *out, uint16_t cap);
