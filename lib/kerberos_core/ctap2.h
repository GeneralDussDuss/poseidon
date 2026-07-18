#pragma once
#include "kerb_crypto.h"
#include <stdbool.h>

struct cred_store;   // forward; resident credential storage (Task 6)

// ---- CTAP2 clientPIN / PIN-UV state (pinUvAuthProtocol 1) ----

// Persistent PIN storage (NVS on device, in-memory for tests). If cfg->pin is
// null, the authenticator advertises no clientPin and rejects clientPIN/reset,
// matching the pre-PIN behavior.
typedef struct pin_store {
    // If a PIN is set, copy the 16-byte pinHash + retry counter, return 1.
    // Return 0 if no PIN is set.
    int  (*load)(struct pin_store *, uint8_t hash16[16], uint8_t *retries);
    // Persist pinHash + retries (setPIN / changePIN). Returns 0 on success.
    int  (*save)(struct pin_store *, const uint8_t hash16[16], uint8_t retries);
    // Persist just the retry counter after a failed attempt. Returns 0 on success.
    int  (*save_retries)(struct pin_store *, uint8_t retries);
    // Remove the PIN entirely (authenticatorReset).
    void (*wipe)(struct pin_store *);
} pin_store;

// Volatile per-boot PIN runtime. Caller zero-inits one at key-mode boot; the
// core fills the key-agreement keypair and token lazily.
typedef struct {
    uint8_t  ka_priv[32];   // authenticator key-agreement private (per boot)
    uint8_t  ka_pub[65];    // 0x04 || X || Y
    bool     ka_ready;
    uint8_t  token[32];     // current pinUvAuthToken
    bool     token_set;
    uint8_t  token_rp[32];  // rpIdHash the token is bound to (permissions form)
    bool     token_rp_set;
    uint8_t  token_perms;   // PIN_PERM_* bits
    uint8_t  boot_fails;    // consecutive PIN failures w/o power-cycle (>=3 -> auth-blocked)
    uint32_t boot_ms;       // millis() at key-mode boot (reset 10s window); 0 = unknown
} ctap2_pin_rt;

enum {
    PIN_PERM_MC = 0x01,     // makeCredential
    PIN_PERM_GA = 0x02,     // getAssertion
};

pin_store *pin_store_nvs(void);   // NVS-backed PIN hash + retry counter (device only)

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
    uint32_t      *counter;        // global signature counter (shared with U2F); persisted by caller
    struct cred_store *store;      // resident credentials; may be null
    struct pin_store  *pin;        // persistent PIN store; null = no clientPin support
    ctap2_pin_rt      *pin_rt;     // volatile per-boot PIN runtime; null = no clientPin support
    uint32_t           now_ms;     // millis() now, for the reset 10s window (0 = unknown/tests)
} ctap2_cfg_t;

enum {
    CTAP2_MAKE_CRED  = 0x01,
    CTAP2_GET_ASSERT = 0x02,
    CTAP2_GET_INFO   = 0x04,
    CTAP2_CLIENT_PIN = 0x06,
    CTAP2_RESET      = 0x07,
};

// Handle one CTAP2 request. out[0] is the CTAP2 status byte (0x00 = success),
// followed by response CBOR. Returns total length written.
uint16_t ctap2_handle(const ctap2_cfg_t *cfg, const uint8_t *req, uint16_t len,
                      uint8_t *out, uint16_t cap);
