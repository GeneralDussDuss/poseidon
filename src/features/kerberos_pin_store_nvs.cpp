/*
 * NVS-backed clientPIN store for KERBEROS. Namespace "kerbpin":
 *   "set" (bool)  — whether a PIN is configured
 *   "h"   (16 B)  — LEFT-16 of SHA-256(PIN)
 *   "r"   (uint8) — remaining retry counter (starts at 8)
 * authenticatorReset clears the namespace.
 */
#include "ctap2.h"
#include <Preferences.h>
#include <string.h>

static const char *NS = "kerbpin";

static int ps_load(pin_store *, uint8_t h[16], uint8_t *r) {
    Preferences p; p.begin(NS, true);
    bool set   = p.getBool("set", false);
    size_t got = p.getBytes("h", h, 16);
    uint8_t rr = p.getUChar("r", 8);
    p.end();
    if (!set || got != 16) return 0;
    *r = rr;
    return 1;
}
static int ps_save(pin_store *, const uint8_t h[16], uint8_t r) {
    Preferences p; p.begin(NS, false);
    p.putBytes("h", h, 16); p.putUChar("r", r); p.putBool("set", true);
    p.end();
    return 0;
}
static int ps_save_retries(pin_store *, uint8_t r) {
    Preferences p; p.begin(NS, false); p.putUChar("r", r); p.end();
    return 0;
}
static void ps_wipe(pin_store *) {
    Preferences p; p.begin(NS, false); p.clear(); p.end();
}

static pin_store S = { ps_load, ps_save, ps_save_retries, ps_wipe };
pin_store *pin_store_nvs(void) { return &S; }
