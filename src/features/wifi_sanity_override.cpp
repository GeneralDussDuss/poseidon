/*
 * wifi_sanity_override.cpp — link-time override of libnet80211.a's
 * raw-frame filter.
 *
 * The ESP-IDF WiFi blob (libnet80211.a) contains a function:
 *   int ieee80211_raw_frame_sanity_check(int32_t, int32_t, int32_t);
 *
 * which is called from the TX path before every raw frame is sent.
 * If this function returns non-zero, the frame is rejected with
 * ESP_ERR_INVALID_ARG (rc=258) and the blob logs
 *   E (xxx) wifi:unsupport frame type: 0c0
 *
 * That's what was blocking all POSEIDON deauth frames — not the
 * Marauder/Bruce "silent AP" pattern, not the mode, not the interface.
 * The subtype filter in this function rejects 0xC (deauth) and 0xA
 * (disassoc) unconditionally on stock IDF 5.x libs, including the
 * "patched" bmorcelli / Bruce-lib-builder zips (which turn out not
 * to actually patch this function).
 *
 * The fix: define the symbol in OUR code. When the linker resolves
 * esp_wifi_80211_tx calls to ieee80211_raw_frame_sanity_check, it
 * uses OUR version (strong symbol, from our .o) instead of the blob's
 * (weak/archive-library symbol). Our version always returns 0 = OK,
 * so deauth/disassoc/any subtype passes through to the hardware.
 *
 * Credit: GANESH-ICMC/esp32-deauther figured this out. Bruce uses the
 * same trick. The `if (arg == 31337) return 1` path is legacy sentinel
 * so anyone who wants to test "is the override linked?" can call with
 * arg=31337 and see it return 1 instead of 0.
 */
#include <stdint.h>

extern "C" int ieee80211_raw_frame_sanity_check(int32_t arg, int32_t arg2, int32_t arg3)
{
    (void)arg2; (void)arg3;
    if (arg == 31337) return 1;
    return 0;   /* allow everything, including deauth/disassoc */
}
