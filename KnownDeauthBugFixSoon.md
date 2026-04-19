# ✅ Deauth Fixed in v0.4.0

**The v0.3 deauth issue is resolved as of v0.4.0 (2026-04-19).**

This file existed to warn testers that WiFi deauth frames weren't
reaching the air on v0.3. That's no longer the case. Keeping the file
around with a short post-mortem in case anyone hits the same class of
issue elsewhere.

## What was broken

WiFi deauth on POSEIDON v0.3 — targeted, broadcast-all, per-client, and
Triton autonomous modes all hit the same issue. The Cardputer UI
showed `frames:` at 0 and `drops:` climbing. Target devices didn't
disconnect.

## What was actually wrong

The ESP-IDF WiFi blob (`libnet80211.a`) contains a function:

```c
int ieee80211_raw_frame_sanity_check(int32_t, int32_t, int32_t);
```

which is called from `esp_wifi_80211_tx` before every raw-frame TX. It
returns non-zero for MGMT subtypes `0xC` (deauth) and `0xA` (disassoc),
causing TX to return `ESP_ERR_INVALID_ARG (258)` with the log:

```
E (xxx) wifi:unsupport frame type: 0c0
```

This filter runs regardless of:
- WiFi mode (STA, AP, AP_STA)
- Interface used (WIFI_IF_STA, WIFI_IF_AP)
- Promiscuous mode
- MAC spoofing
- `en_sys_seq` flag
- Internal vs public TX API

**It is also present in every "patched" libnet80211.a from
`bmorcelli/esp32-arduino-lib-builder`** — the "patched libs" claim
widely repeated in the ESP32 deauth community is wrong. The symbol is
still `T` (strong-global) in every Bruce zip we checked (20260407,
20260123, older ones). The patch was never there.

## The real fix

Link-time symbol override. Credit to
[GANESH-ICMC/esp32-deauther](https://github.com/GANESH-ICMC/esp32-deauther)
for figuring this out years ago; Bruce has been using it in
`src/modules/wifi/wifi_atks.cpp` without documenting it clearly.

1. Define our own `ieee80211_raw_frame_sanity_check` returning 0:
   see [`src/features/wifi_sanity_override.cpp`](src/features/wifi_sanity_override.cpp).
2. Add `-Wl,--allow-multiple-definition` to `build_flags` in
   [`platformio.ini`](platformio.ini).

That's it. The linker prefers our `.o` over the library's `.a`, the
filter function returns 0 (OK) for every subtype, and
`esp_wifi_80211_tx` happily transmits deauth / disassoc frames on any
interface in any mode.

Verified on-device via `src/deauth_autotest.cpp` (gated on
`-DPOSEIDON_AUTO_DEAUTH_TEST`): 200 bursts × 4 frames each,
`ok=800 fail=0 rate=99%` against a non-PMF WPA2 AP. Target client
kicked.

## What you can test now

- WiFi → Deauth → pick non-PMF WPA2 AP → press `D` → `frames:` climbs,
  `drops:` stays near zero, target client drops WiFi in 2-5 seconds
- Same for Deauth All, per-client Clients, all-channels Clients, and
  Triton autonomous hunt. All routed through the same fix.
- PMKID capture actually works now too — the same TX path was needed
  for the M1 force-reconnect trick.

## Known limitation

WPA3-PSK, WPA2-Enterprise, WPA3-transition networks use PMF (Protected
Management Frames) which cryptographically sign mgmt frames. Plain
deauth cannot kick those clients — that's a protocol-level defense,
not a firmware limit. The UI warns you before firing.

---

*Post-mortem added 2026-04-19. File kept for search discoverability.*
