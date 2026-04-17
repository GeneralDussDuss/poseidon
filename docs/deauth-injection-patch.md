# Deauth Injection — Why Stock ESP-IDF Isn't Enough

## The problem

POSEIDON's WiFi injection uses `esp_wifi_80211_tx(WIFI_IF_STA, buf, len, false)`. The in-code fixes (sequence numbers, disassoc pairs, correct addressing, client-aware unicast rounds) make every frame we *construct* correct. But the **Espressif WiFi binary blob** is the last mile: it decides whether a given raw frame actually reaches the antenna.

On the default `espressif32@6.7.0` platform that POSEIDON is pinned to, the blob enforces a sanity filter on arbitrary mgmt frames:

- **Spoofed source MAC (addr2 ≠ STA's own MAC):** stock blob silently drops most frames where the transmitter address doesn't match the chip's active MAC. Deauth is specifically this case — we spoof the AP's BSSID as addr2.
- **Arbitrary subtype injection:** some subtypes are restricted to internal driver use. On some IDF point releases, mgmt subtype 0xC0 (deauth) and 0xA0 (disassoc) pass but with rate-limiting; on others they're dropped silently.
- **Return value:** `esp_wifi_80211_tx` returns `ESP_OK` even when the frame is dropped at the FIFO. Our error counter tracks real rejects but cannot see driver-internal drops.

Net result: testers see POSEIDON report "flooding, 40 fps" while another tool on the same target (Flipper Marauder, Ghost ESP, aircrack-ng on a laptop) kicks clients instantly. This is not a POSEIDON bug anymore — it's the blob.

## What the other tools do differently

**ESP32 Marauder / Ghost ESP / Bruce** ship with a **patched ESP32 Arduino Core** where the `esp_wifi` component has the blob's injection filter disabled. Specific patches that are applied (from the public JustCallMeKoko / Marauder fork):

1. `libnet80211.a` → the `ieee80211_raw_frame_sanity_check` symbol returns 0 unconditionally (vs filtering spoofed addr2)
2. `libpp.a` → relaxed checks in `pp_hw_tx_param` / `lmacProcessTxSucc`
3. Sometimes a Kconfig toggle: `CONFIG_ESP_WIFI_ENABLE_RAW_TX`

These ship as pre-patched static libraries inside a custom Arduino Core fork, and PlatformIO picks them up via a platform reference.

## How to apply to POSEIDON

### Option A — use Marauder's platform fork (recommended)

Patch `platformio.ini`:

```ini
[env:m5stack-cardputer]
platform = https://github.com/justcallmekoko/platform-espressif32
; or Bruce's fork, which tracks more recent IDFs:
; platform = https://github.com/pr3y/platform-espressif32
framework = arduino
board = m5stack-stamps3
```

First build will pull their platform + patched Arduino Core. Takes ~15 min on a fresh machine.

### Option B — build with your own patched core

1. Clone `https://github.com/espressif/arduino-esp32`
2. Apply the Marauder patches from `JustCallMeKoko/ESP32Marauder/tree/master/esp32_marauder/precompiled_libs`
3. Drop the patched `libnet80211.a` + `libpp.a` into `framework-arduinoespressif32/tools/sdk/esp32s3/lib/`
4. Point `platformio.ini` at your local path via `platform_packages = framework-arduinoespressif32 @ file:///path/to/patched`

More brittle. Breaks on every Arduino-ESP32 release. Only do this if you need a specific IDF version Marauder's fork hasn't merged.

### Option C — stay on stock, accept the reduced effectiveness

Valid for legal/compliance reasons (some orgs ban "unauthorized frame injection" firmware even on authorized testing devices). POSEIDON's code is now correct; the output is just filtered by the blob. Broadcast deauth to legacy WPA2 devices will still work on many cards because the filter is less strict on broadcast addr1.

## How to verify the patch is active

Run `feat_wifi_deauth_broadcast` against a known non-PMF target with a second monitor-mode device nearby. In Wireshark filter:

```
wlan.fc.type_subtype == 0x0C && wlan.bssid == <target>
```

Stock blob with spoofed addr2: you'll see **zero** or a handful of frames.
Patched blob: you'll see POSEIDON's full 16 pairs/rotation hitting the target.

Also: **drops counter** on the POSEIDON UI. With the patch you should see `drops: 0` or very low. Without the patch, `esp_wifi_80211_tx` may still return `ESP_OK` (driver-internal drop), but any frame that hits `ESP_ERR_*` will bump the counter.

## What's NOT a patch problem

If deauth doesn't work against:
- **WPA3 / WPA2-Enterprise** — PMF (802.11w) drops cryptographically, patched or not. POSEIDON now warns on these
- **Brand-new client drivers (iOS 17+, Android 14+)** — some ignore all unprotected mgmt frames regardless of subtype, moving to 802.11w by default. Can't be fixed in software on our end
- **High-power corporate APs with client-isolation** — they may rebroadcast the client's deauth to drop the *POSEIDON* pseudo-STA instead. Rare but seen

## TL;DR

Fix the addresses / sequence / disassoc / sniffer stuff (done in the current commit). **Then swap the platform to Marauder's fork** to get parity with other ESP32-based tools. Without step 2, the stock blob will keep throttling spoofed-addr2 frames.
