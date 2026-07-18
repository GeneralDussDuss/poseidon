#pragma once
#include <stdint.h>
#include <stddef.h>

// One resident (discoverable) credential. The private key is stored wrapped
// (keywrap, 60 bytes) so it is encrypted at rest under the device key.
typedef struct {
    uint8_t  id[32];          // credential id
    uint8_t  rpIdHash[32];
    uint8_t  userId[64];
    uint8_t  userIdLen;
    char     name[32];        // truncated display name (optional)
    uint8_t  wrappedKey[60];  // KW_HANDLE_LEN
    uint32_t signCount;
} cred_record;

// Storage interface. The in-memory impl is for host tests; the NVS impl backs
// the device.
typedef struct cred_store {
    int (*add)(struct cred_store *, const cred_record *);
    // Return the match at `index` among credentials for rpIdHash into `out`,
    // set *total to how many match. Returns 0 if `index` was valid.
    int (*find_by_rp)(struct cred_store *, const uint8_t rpIdHash[32],
                      cred_record *out, int index, int *total);
    int (*update_counter)(struct cred_store *, const uint8_t id[32], uint32_t newCount);
    // Remove all stored credentials (authenticatorReset). May be null on stores
    // that don't support it (reset then leaves creds intact).
    void (*wipe)(struct cred_store *);
} cred_store;

cred_store *cred_store_mem(void);   // in-memory, reset on each call (tests)
cred_store *cred_store_nvs(void);   // NVS-backed (device only; defined in src/features)
