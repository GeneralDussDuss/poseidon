#pragma once
#include "kerb_crypto.h"

typedef struct {
    const kerb_crypto_t *cy;
    const uint8_t *devkey;         // 32-byte wrapping key
    const uint8_t *att_cert;       // DER X.509
    uint16_t       att_cert_len;
    const uint8_t *att_priv;       // 32-byte attestation private key
    uint32_t      *counter;        // monotonic signature counter (persisted by caller)
    bool         (*user_present)(void *ui);   // blocks until Enter or returns false on abort
    void          *ui;
} u2f_cfg_t;

uint16_t u2f_handle(const u2f_cfg_t *cfg, const uint8_t *apdu, uint16_t len,
                    uint8_t *out, uint16_t cap);
