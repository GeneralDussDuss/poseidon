#include "kerberos_bootmode.h"
#include <esp_attr.h>
#include <esp_system.h>

// Lives in RTC memory. RTC_NOINIT (not RTC_DATA) is the key point: the
// bootloader re-initialises RTC_DATA on a software reset, which would wipe the
// flag we set just before esp_restart(). RTC_NOINIT is left untouched across
// resets and is only garbage on a cold power-on, which the magic value below
// filters out. No initializer allowed on a NOINIT variable.
static RTC_NOINIT_ATTR uint32_t s_kerb_mode;
#define KERB_KEY_MAGIC 0x6B657942u   // 'keyB'

bool kerb_boot_key_mode(void) {
    return s_kerb_mode == KERB_KEY_MAGIC;
}

void kerb_request_key_mode(bool on) {
    s_kerb_mode = on ? KERB_KEY_MAGIC : 0;
    esp_restart();
}
