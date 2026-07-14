#pragma once
#include <stdint.h>

// Portable BLE deep-clone profile format (Chimera BLE Phase 3, Task 3.1).
// A compact, human-diffable text serialization of a target's GATT tree +
// advertising payload, stored on SD as /poseidon/clones/<name>.pcl.
// No NimBLE / Arduino deps so the parse/format is host-unit-testable.

#define CLONE_NAME_MAX    24
#define CLONE_ADV_MAX     31
#define CLONE_UUID_MAX    37   // 128-bit UUID string + NUL
#define CLONE_MAX_VALUE   32   // stored characteristic/descriptor value bytes
#define CLONE_MAX_ENTRIES 48

enum clone_entry_type_t { CE_SERVICE = 0, CE_CHAR = 1, CE_DESC = 2 };

typedef struct {
    uint8_t type;                    // clone_entry_type_t
    char    uuid[CLONE_UUID_MAX];    // 16- or 128-bit UUID string form
    uint8_t props;                   // characteristic property bitmap (0 for svc/desc)
    uint8_t val[CLONE_MAX_VALUE];    // stored value bytes
    uint8_t val_len;
} clone_entry_t;

typedef struct {
    char    name[CLONE_NAME_MAX];
    uint8_t mac[6];
    uint8_t adv[CLONE_ADV_MAX];
    uint8_t adv_len;
    clone_entry_t entries[CLONE_MAX_ENTRIES];
    int     n_entries;
} clone_profile_t;

// Serialize a profile to text. Returns bytes written (excluding NUL) or -1
// if the output buffer is too small.
int  clone_profile_serialize(const clone_profile_t *p, char *out, int out_sz);

// Parse profile text. Returns true and fills *out on success; false on any
// malformed / unrecognized input.
bool clone_profile_deserialize(const char *text, clone_profile_t *out);
