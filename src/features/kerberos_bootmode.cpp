#include "kerberos_bootmode.h"
#include <esp_attr.h>
#include <esp_system.h>
#include <Preferences.h>

// Lives in RTC memory. RTC_NOINIT (not RTC_DATA) survives the SOFT reset used to
// switch personality, but is garbage on a COLD power-on (unplug/reinsert). The
// OS FIDO reset flow forces exactly that reinsert, so the RTC flag alone would
// drop us out of key mode mid-reset — the FIDO device would vanish. We mirror
// the personality in NVS and re-assert it on cold boot (kerb_boot_sync).
static RTC_NOINIT_ATTR uint32_t s_kerb_mode;
#define KERB_KEY_MAGIC 0x6B657942u   // 'keyB'
static const char *KERB_NS = "kerberos";

bool kerb_boot_key_mode(void) {
    return s_kerb_mode == KERB_KEY_MAGIC;
}

void kerb_request_key_mode(bool on) {
    Preferences p; p.begin(KERB_NS, false);   // persist across a full power-cycle
    p.putBool("keymode", on);
    p.end();
    s_kerb_mode = on ? KERB_KEY_MAGIC : 0;
    esp_restart();
}

void kerb_boot_sync(void) {
    // Call FIRST in setup(). On a soft reset the RTC flag is intact -> nothing to
    // do. On a cold boot the RTC flag is garbage; if NVS says we should be in key
    // mode, set the flag and reboot once so the USB FIDO interface — chosen at
    // static-init, before setup() runs — enumerates. Costs one extra reboot only
    // when a reinserted key needs to return to key mode.
    if (kerb_boot_key_mode()) return;
    Preferences p; p.begin(KERB_NS, true);
    bool want = p.getBool("keymode", false);
    p.end();
    if (want) { s_kerb_mode = KERB_KEY_MAGIC; esp_restart(); }
}
