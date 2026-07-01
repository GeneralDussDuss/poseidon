# KERBEROS — FIDO2 / WebAuthn Hardware Key for POSEIDON

Date: 2026-07-01
Status: Approved design, ready for implementation planning
Feature name: KERBEROS (the guardian of the gate)
Target: M5Stack Cardputer-Adv (ESP32-S3), inside the existing POSEIDON firmware at `Projects/poseidon-suite/poseidon`

## 1. Summary

KERBEROS turns the Cardputer into a real FIDO2 / WebAuthn hardware authenticator (a "passkey" / security key), living as one feature inside POSEIDON alongside the other 80. When active it enumerates over USB as a security key and works for Gmail, Discord, GitHub, Microsoft accounts, and any WebAuthn relying party. It speaks CTAP2.1 (passkeys, discoverable credentials, client PIN) and CTAP1 / U2F (legacy second factor).

Its advantage over a blind touch key like a YubiKey is the screen and keyboard: it shows you which site is asking before you approve, takes user presence as a physical Enter press, and takes user verification as a PIN typed on the keyboard.

Windows compatibility is a hard requirement and is called out throughout, not treated as an afterthought.

## 2. Goals and non goals

Goals:
- A conformant FIDO2 authenticator over USB HID that Windows, macOS, Linux, Chrome, Edge, and Firefox all recognize with no drivers.
- Passkey (discoverable credential) support so it works for passwordless sign in.
- Client PIN for user verification, entered on the Cardputer keyboard.
- On screen display of the relying party during registration and authentication.
- Encrypted credential storage at rest that survives POSEIDON's frequent reflash workflow.
- Encrypted backup and restore of the credential store to microSD, portable to a replacement Cardputer.
- A documented Windows login story, including the local account fallback.

Non goals for the first delivery:
- BLE / hybrid (caBLE) transport. USB only to start.
- NFC.
- Enterprise attestation. Self attestation only.
- Acting as a general password manager.

## 3. Where FIDO2 works, and the Windows reality

| Target | CTAP2 passkey | CTAP1 / U2F 2FA | Notes |
| --- | --- | --- | --- |
| Google / Gmail | Yes | Yes | Security key and passkey both accepted |
| Discord | Yes | Yes | WebAuthn security keys supported |
| GitHub, most WebAuthn sites | Yes | Yes | No caveats |
| Microsoft / Entra Windows sign in | Yes | n/a | Passwordless Windows login works with a Microsoft or Entra account |
| Local Windows account sign in | No | No | Microsoft never wired FIDO2 into the local account lock screen |

Local account Windows fallback: KERBEROS reuses POSEIDON's existing `badusb.cpp` HID typer. You approve on the Cardputer, it types your PIN or password over USB HID. Not cryptographic, but it unlocks the machine. This ships as a secondary mode inside KERBEROS, clearly labeled as the non FIDO fallback.

## 4. Architecture

Four layers. Only the top feature file and the bottom shim layer are new POSEIDON code. The CTAP core is vendored C from pico-fido (CTAP2.1 / U2F, CBOR, credential management), which is framework neutral and already built on mbedtls.

```
  POSEIDON menu tree (menu.cpp)  ──►  feat_kerberos()        NEW  src/features/kerberos.cpp
                                        │
  UI + policy layer              ──►  RP display, presence prompt, PIN entry, account picker
                                        │   (M5Cardputer.Display, input_poll, ui_*, PK_*)
                                        │
  CTAP core (vendored)           ──►  CTAP2.1 + U2F state machine, CBOR, credential mgmt
                                        │   (mbedtls for ES256 / P-256, ECDH, HMAC, AES)
                                        │
  HAL shims                      ──►  USB CTAPHID (TinyUSB)  |  crypto (mbedtls)
                                       storage (encrypted NVS)  |  UP/UV (POSEIDON UI callbacks)
```

New files (proposed):
- `src/features/kerberos.cpp` — feature entry `void feat_kerberos(void)`, mode UI, dispatch.
- `src/features/kerberos_hid.cpp` / `.h` — TinyUSB FIDO HID interface and CTAPHID transport.
- `src/features/kerberos_ctap.*` — vendored pico-fido CTAP core plus a thin POSEIDON glue header.
- `src/features/kerberos_store.cpp` / `.h` — encrypted credential store over a dedicated NVS partition.
- `src/features/kerberos_crypto.cpp` / `.h` — mbedtls bindings the CTAP core calls.

Menu registration: add a KERBEROS node to `MENU_ROOT` in `src/menu.cpp`, following the existing hotkey mnemonic pattern, pointing at `feat_kerberos`.

## 5. USB transport and mode model

KERBEROS is a mode you enter from the menu, identical in spirit to `feat_badusb`. On entry it checks the shared CDC busy guard (`g_mimir_cdc_active`, `g_trident_cdc_active`) and refuses if another feature owns the serial link, exactly as badusb does. It then brings up USB via `USB.begin()` with a FIDO HID interface registered, so the single USB-C PHY switches from USB-Serial-JTAG (CDC) to the TinyUSB OTG stack and the host enumerates a security key. On exit it tears the interface down and returns to normal.

While in KERBEROS mode the Cardputer stays enumerated as a key so browser and OS ceremonies can drive it. Exiting the screen ends the key session. A boot straight to key variant can come later if plug and go is wanted.

FIDO HID interface requirements (these are what make Windows accept it):
- HID report descriptor with Usage Page `0xF1D0` (FIDO Alliance), Usage `0x01` (CTAPHID), one 64 byte input report and one 64 byte output report, report ID 0.
- Implemented over the same Arduino TinyUSB `USBHID` mechanism `badusb.cpp` already uses, via a custom `USBHIDDevice` subclass that supplies the FIDO report descriptor. No new USB stack.
- A stable USB VID and PID so the OS caches the device identity consistently. A test VID/PID is acceptable for personal use; this is not a shippable commercial identity and the spec notes that.

CTAPHID transport layer (Windows sensitive):
- Channel management: `CTAPHID_INIT` with nonce echo and channel id allocation.
- Message routing: `CTAPHID_MSG` for U2F, `CTAPHID_CBOR` for CTAP2, `CTAPHID_PING`, `CTAPHID_CANCEL`, `CTAPHID_WINK`, `CTAPHID_ERROR`.
- Capability flags advertised in INIT: CBOR and WINK set, NMSG only if U2F is disabled.
- KEEPALIVE: while the CTAP core is blocked waiting for the user to press Enter or enter a PIN, the transport must emit `CTAPHID_KEEPALIVE` frames (status "user presence needed" / "processing") on a timer. Windows and Chromium abort the ceremony if the device goes silent, and our presence check is a human in the loop that can take several seconds. The presence and PIN callbacks are therefore non blocking to the transport: they yield so KEEPALIVE keeps flowing.

## 6. On device flows

All rendering uses the existing POSEIDON UI helpers (`ui_clear_body`, `ui_draw_footer`, `ui_toast`, `M5Cardputer.Display`) and input via `input_poll` with `PK_ENTER`, `PK_ESC`, `PK_UP`, `PK_DOWN`.

Register (authenticatorMakeCredential):
1. Screen shows the relying party, for example `github.com wants to create a passkey`.
2. If verification is requested, prompt for the PIN on the keyboard.
3. Press Enter to confirm presence. Escape aborts, returning `CTAP2_ERR_OPERATION_DENIED`.
4. Generate an ES256 (P-256) credential, store it, return the attestation object with self attestation.

Authenticate (authenticatorGetAssertion):
1. Screen shows the relying party, for example `google.com sign in`.
2. If several discoverable credentials match, show an account picker (the same scrollable list widget `pick_list_scrollable` already provides).
3. PIN if user verification is required.
4. Enter to approve. Sign, increment the per credential counter, return the assertion.

Client PIN:
- Set on first use. Entered on the physical keyboard.
- Retry counter and lockout per the CTAP2 client PIN spec: decrement on wrong PIN, lock after the limit, require a power cycle after a run of failures.
- Supports PIN/UV auth protocol v2, with v1 for compatibility.
- The PIN both satisfies FIDO user verification and unlocks the credential store key (see section 7).

## 7. Credential storage and at rest security

Full release mode Secure Boot v2 plus flash encryption is an irreversible eFuse burn that would break POSEIDON's constant reflash development loop. So KERBEROS does not depend on secure boot for confidentiality. It encrypts credentials at the application layer instead:

- A random device master secret is burned once into an ESP32-S3 eFuse key block with an HMAC purpose that software cannot read back.
- At unlock time the storage key is derived by the S3 HMAC peripheral over the user PIN plus a stored salt, keyed by that eFuse secret. Neither the PIN nor the derived key is ever written to flash.
- Credentials (key handles, resident credentials, signature counters, PIN state) live in a dedicated NVS partition, `nvs_kerb`, separate from POSEIDON's general storage, with records encrypted under the derived key (AES via mbedtls) and integrity tagged.
- Signature counters persist per credential across reboots. Relying parties flag cloned keys if the counter ever goes backward, so counter durability is a correctness requirement, not a nicety.

Result: dumping the flash yields ciphertext with no PIN and no eFuse secret, so it does not reveal passkeys, and full dev flashability is retained. An optional later lockdown step can enable real Secure Boot v2 on a frozen build.

Partition tables: add the `nvs_kerb` data partition (subtype nvs) to both `default_8MB.csv` and `support_files/launcher_8Mb.csv`. The 8 MB flash has room with PSRAM disabled and no other large data regions contending.

Residual risk, stated plainly: while the device is unlocked and in your hand, the unlocked store shares the SoC with POSEIDON's attack features. The eFuse and PIN protect data at rest, not a live unlocked session. This is accepted under the co-resident, hardened choice.

### 7.1 Passkey type and why keys never touch Google

KERBEROS issues device bound passkeys, not synced ones. A synced passkey (Google Password Manager, iCloud Keychain, 1Password) keeps an encrypted copy of the private key in a cloud account and syncs it across devices. KERBEROS instead generates each private key inside the Cardputer where it physically cannot leave; the relying party stores only the matching public key and a credential id. So a KERBEROS passkey lives in the `nvs_kerb` partition, never in Google's or anyone's cloud. That is the security win, and the recovery cost that section 7.2 addresses. Capacity note: unlike a hardware key that holds a couple dozen resident credentials, the 8 MB flash lets KERBEROS hold hundreds of discoverable credentials.

### 7.2 Backup and recovery

Device bound means a lost Cardputer loses its passkeys, so KERBEROS supports two independent recovery paths and the user picks per account.

Encrypted SD backup (the "kennel" backup):
- Export serializes the credential records and writes an encrypted blob to microSD, for example `/poseidon/kerberos/kennel-<stamp>.bak`. Restore reads it back on any Cardputer.
- Critical design point: the backup is NOT encrypted under the device eFuse secret, because a replacement Cardputer has a different eFuse secret and could never decrypt it. Instead the backup is encrypted under a separate, longer recovery passphrase the user sets once. The day to day PIN stays short and eFuse bound for at rest storage and fast user verification; the recovery passphrase is long and used only for export and restore. This keeps backups portable across devices.
- Because a short PIN cannot protect an offline file, the backup key is derived from the recovery passphrase with a strong, slow KDF (for example PBKDF2 or Argon2 style stretching within the S3's means) plus a per file salt stored in the blob, and the blob is integrity tagged. A stolen `.bak` is then only as weak as the chosen passphrase.

Second authenticator (FIDO best practice, documented not built):
- For any important account, register a backup authenticator alongside KERBEROS: a real hardware key, or a synced Google or Apple passkey. If KERBEROS is lost, the backup key still signs in, so loss never means lockout. This is the recommended primary safety net; the SD backup is the convenience layer on top.

Security trade-off, stated plainly: an SD backup means a copy of the credential material exists outside the device, which is weaker than the pure "the secret only ever lived here" model. The recovery passphrase and slow KDF bound that risk. Users who want the purest model can skip backups and rely only on a second authenticator.

## 8. Windows compatibility checklist

Treated as acceptance criteria, not aspirations:
- FIDO HID profile exactly as in section 5 (usage page `0xF1D0`, 64 byte reports), so the Windows WebAuthn platform API recognizes the authenticator with no driver.
- Full CTAPHID including INIT channel allocation and KEEPALIVE during user presence and PIN waits.
- clientPIN and user verification, since Windows Hello passwordless requires UV.
- Discoverable credentials (resident keys) with credProtect, required for passwordless Windows sign in.
- Self attestation accepted by consumer Microsoft sign in; no enterprise attestation needed.
- Stable VID/PID so Windows does not re register the device each session.
- Verified against the Windows built in flow at Settings, Accounts, Sign in options, Security Key, and against browser WebAuthn (Edge and Chrome) at webauthn.io, plus real Gmail, Discord, and a Microsoft account.

## 9. Delivery phases

Each phase is independently testable on hardware.

1. CTAPHID plumbing and U2F. FIDO HID interface up, `CTAPHID_INIT` and `CTAPHID_PING` answered, U2F register and authenticate working. Prove a browser sees a key at webauthn.io in U2F mode. Includes the USB mode enter and exit and the CDC busy guard.
2. CTAP2 makeCredential and getAssertion. CBOR, P-256 credentials, presence via Enter, RP shown on screen. Gmail and Discord passkeys work end to end. KEEPALIVE proven under a slow human approval.
3. Client PIN and user verification. Keyboard PIN entry, retry and lockout, account picker for multiple credentials.
4. Hardened storage. eFuse HMAC key, PIN derived encryption, the `nvs_kerb` partition, persistent signature counters.
5. Backup and recovery. Recovery passphrase, encrypted SD export and restore of the credential store, restore verified onto a second Cardputer, and the documented "register a second authenticator" guidance.
6. Windows and polish. Entra passwordless verification, the badusb local account fallback mode, POSEIDON theming, and the full section 8 checklist run on Windows.

## 10. Testing

- Protocol conformance using pico-fido's python-fido2 based CTAP test suite run against the device over USB.
- Manual WebAuthn against webauthn.io across Edge, Chrome, and Firefox.
- Real logins: Gmail, Discord, GitHub, a Microsoft account.
- Windows security key management flow in Settings.
- Reboot tests confirming signature counters never regress and credentials survive power cycles.
- A wrong PIN lockout test confirming the retry counter and recovery behave per spec.
- Backup and restore round trip: export the kennel on one Cardputer, restore onto a second, and confirm the restored passkeys authenticate to Gmail and Discord. A wrong recovery passphrase must fail the restore cleanly.

## 11. Open risks

- TinyUSB descriptor coexistence: switching between KERBEROS, badusb, and mass storage must not wedge USB enumeration. Mitigation: a single owner model enforced by the existing CDC busy guard, extended to cover the FIDO interface.
- Vendored pico-fido core: we take a pinned snapshot rather than a live dependency, so upstream fixes are pulled in deliberately. The port surface is the small HAL (USB, crypto, storage, UP/UV), which bounds the work.
- eFuse burn is one time: the storage master secret burn must be gated behind an explicit user action with a clear warning, since it cannot be undone on that unit.
- VID/PID identity: a test identity is fine for personal use but is called out as not commercially shippable.
- SD backup offline attack: a stolen `.bak` is exposed to offline guessing of the recovery passphrase. Mitigation is a strong slow KDF plus a genuinely long passphrase; the spec forbids deriving the backup from the short device PIN. Users wanting no external copy can skip backups and rely on a second authenticator.
- Restore trust: importing a `.bak` writes credentials the device did not originate. Restore is gated behind the recovery passphrase and an explicit on device confirmation so a planted file cannot silently inject credentials.
```
