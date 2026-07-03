/*
 * NVS-backed resident credential store for KERBEROS Phase 2.
 *
 * Records live in the "kerbcred" namespace: "n" holds the count, "r<i>" holds
 * each cred_record blob (the private key inside is already keywrap-encrypted).
 * Phase 4 will move the wrapping key to the eFuse-derived key. No deletion yet.
 */
#include "cred_store.h"
#include <Preferences.h>
#include <string.h>
#include <stdio.h>

static const char *NS = "kerbcred";

static int nvs_count(void) {
    Preferences p; p.begin(NS, true);
    int n = (int)p.getUInt("n", 0);
    p.end();
    return n;
}
static bool nvs_load(int i, cred_record *r) {
    char key[12]; snprintf(key, sizeof key, "r%d", i);
    Preferences p; p.begin(NS, true);
    size_t got = p.getBytes(key, r, sizeof *r);
    p.end();
    return got == sizeof *r;
}
static bool nvs_save(int i, const cred_record *r) {
    char key[12]; snprintf(key, sizeof key, "r%d", i);
    Preferences p; p.begin(NS, false);
    size_t put = p.putBytes(key, r, sizeof *r);
    p.end();
    return put == sizeof *r;
}

static int nvs_add(cred_store *, const cred_record *r) {
    int n = nvs_count();
    if (!nvs_save(n, r)) return -1;
    Preferences p; p.begin(NS, false); p.putUInt("n", (uint32_t)(n + 1)); p.end();
    return 0;
}
static int nvs_find(cred_store *, const uint8_t rp[32], cred_record *out, int index, int *total) {
    int n = nvs_count(), count = 0, ret = -1;
    for (int i = 0; i < n; i++) {
        cred_record r;
        if (!nvs_load(i, &r)) continue;
        if (memcmp(r.rpIdHash, rp, 32) == 0) {
            if (count == index) { *out = r; ret = 0; }
            count++;
        }
    }
    if (total) *total = count;
    return ret;
}
static int nvs_update(cred_store *, const uint8_t id[32], uint32_t nc) {
    int n = nvs_count();
    for (int i = 0; i < n; i++) {
        cred_record r;
        if (!nvs_load(i, &r)) continue;
        if (memcmp(r.id, id, 32) == 0) { r.signCount = nc; return nvs_save(i, &r) ? 0 : -1; }
    }
    return -1;
}

static cred_store S = { nvs_add, nvs_find, nvs_update };
cred_store *cred_store_nvs(void) { return &S; }
