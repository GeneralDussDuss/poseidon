# KERBEROS Phase 2 — CTAP2 / WebAuthn Passkeys Design

Date: 2026-07-03
Status: Approved design, ready for implementation planning
Builds on: Phase 1 U2F key (done, verified end to end with python-fido2)
Parent spec: `docs/specs/2026-07-01-kerberos-fido2-design.md`
Target: M5Stack Cardputer-Adv (ESP32-S3), inside POSEIDON at `Projects/poseidon-suite/poseidon`

## 1. Summary

Phase 2 upgrades KERBEROS from a U2F second factor into a CTAP2 authenticator, so it works as a real WebAuthn passkey. It adds three CTAP2 commands (getInfo, makeCredential, getAssertion) over the transport Phase 1 already proved, supports both non-resident and resident (discoverable) credentials, and encodes everything with the framework's TinyCBOR. Client PIN and user verification stay in Phase 3, so Phase 2 passkeys authorise with user presence (an Enter press) only.

## 2. Goals and non goals

Goals:
- authenticatorGetInfo advertising FIDO_2_0 and U2F_V2, an AAGUID, and options rk=true, up=true, uv=false.
- authenticatorMakeCredential producing an ES256 (P-256) credential with self attestation.
- authenticatorGetAssertion signing challenges with per-credential counters.
- Non-resident credentials (stateless wrapped key handle, as in Phase 1) and resident/discoverable credentials stored on device.
- Native unit tests for CBOR, COSE, and command logic; on-device python-fido2 CTAP2 verification.

Non goals this phase:
- Client PIN and user verification (Phase 3).
- eFuse-hardened credential storage (Phase 4); Phase 2 uses plain NVS, same as Phase 1's device key.
- credProtect, largeBlob, hmac-secret, enterprise attestation.
- Account picker UI polish (a minimal picker is fine; full UX is later).

## 3. Reused Phase 1 foundations

Unchanged and depended upon:
- CTAPHID transport (`lib/kerberos_core/ctaphid*`) including the direct `tud_hid_n_report` send path that fixed the dead transport.
- Boot key mode (`kerberos_bootmode`): FIDO is the sole USB HID device; entering KERBEROS reboots into key mode.
- mbedtls crypto vtable (`kerberos_crypto`): random, SHA-256, P-256 keygen, ECDSA sign, AES-256-GCM.
- Stateless key handle wrap/unwrap (`keywrap`) bound to the RP ID hash.
- The embedded self-signed attestation cert and key.
- On-device presence prompt (`kerberos_user_present`) with CTAPHID KEEPALIVE during the wait.

## 4. Architecture

New units, mirroring the Phase 1 layout. The portable core stays hardware free and native-testable behind the injected crypto vtable.

```
  ctaphid_dispatch  ── CTAPHID_CBOR (0x10) ──►  ctap2_handle()      NEW lib/kerberos_core/ctap2.*
                                                    │
                                                    ├─ cbor_util.*   NEW  TinyCBOR map build/parse helpers
                                                    ├─ kerb_crypto   (P-256, ECDSA, SHA-256, AES-GCM)
                                                    ├─ keywrap       (non-resident credential IDs)
                                                    └─ cred_store.*  NEW  resident credential records
```

New files:
- `lib/kerberos_core/cbor_util.h` / `.cpp` — thin helpers over TinyCBOR for building and parsing the CTAP2 CBOR maps (keeps command code readable and keeps TinyCBOR usage in one place).
- `lib/kerberos_core/ctap2.h` / `.cpp` — `uint16_t ctap2_handle(const ctap2_cfg_t*, const uint8_t *req, uint16_t len, uint8_t *out, uint16_t cap)`. Parses the command byte, dispatches to getInfo / makeCredential / getAssertion, assembles COSE keys, authenticatorData, and the attestation object. Returns the CTAP2 status byte followed by CBOR.
- `lib/kerberos_core/cred_store.h` / `.cpp` — resident credential interface: add, lookup by RP ID hash, enumerate, update counter. On-device impl backs it with NVS; tests use an in-memory impl.

Changes to existing files:
- `ctaphid_dispatch.cpp` — route `CTAPHID_CBOR` to `ctap2_handle` (currently rejected), and set the CBOR capability flag in the INIT response.
- `kerberos.cpp` — build a `ctap2_cfg_t` (crypto, devkey, AAGUID, presence callback, cred_store) and register both the U2F and CTAP2 handlers.

## 5. Command behaviour

authenticatorGetInfo (0x04):
- versions: ["U2F_V2", "FIDO_2_0"], aaguid: 16 fixed bytes, options: {rk: true, up: true}. No clientPin option advertised (Phase 3). maxMsgSize reported to match the transport buffer.

authenticatorMakeCredential (0x01):
- Parse clientDataHash, rp, user, pubKeyCredParams (require ES256 / -7, else CTAP2_ERR_UNSUPPORTED_ALGORITHM), options (rk, up).
- Presence via Enter; deny returns CTAP2_ERR_OPERATION_DENIED.
- Generate a P-256 keypair. Credential ID: if rk=false, the wrapped private key (stateless, bound to rpIdHash); if rk=true, a random 32-byte ID with the record saved to cred_store.
- Build authenticatorData (rpIdHash, flags with AT set and UP set, signCount, attestedCredentialData with the COSE ES256 public key) and a "packed" self attestation statement signed by the attestation key.

authenticatorGetAssertion (0x02):
- Parse rpId hash, clientDataHash, allowList, options.
- Resident path: if allowList empty, look up cred_store by rpIdHash; return the first match now (numberOfCredentials for a later picker).
- Non-resident path: unwrap each allowList credential ID against rpIdHash; use the first that authenticates.
- Presence via Enter; sign authenticatorData || clientDataHash with the credential key; increment and persist the counter.

## 6. Resident credential storage

Records in NVS (namespace `kerberos`), one blob per credential under an indexed key, plus a small index of live slots. Each record:
- version byte
- credential ID (32 bytes)
- RP ID hash (32 bytes)
- user handle (up to 64 bytes) and a truncated display name
- wrapped private key (via `keywrap`, so the key at rest is encrypted under the device key)
- sign counter (4 bytes)

Phase 2 uses the same plain-NVS device key as Phase 1; Phase 4 replaces it with the eFuse+PIN derived key and migrates records. Capacity target: at least 50 resident credentials (well within 8 MB flash). Counters persist on every assertion so relying parties never see a regression.

## 7. Testing

- Native Unity (host, no hardware): CBOR round-trips, COSE ES256 key encoding against known vectors, makeCredential/getAssertion output shape with the mock crypto, cred_store add/lookup/enumerate with the in-memory impl, and non-resident unwrap-wrong-rp rejection.
- On device: extend the elevated python-fido2 harness to drive CTAP2 (`Ctap2`, `client.make_credential`, `client.get_assertion`), verifying attestation and assertion signatures. Device must be in key mode.
- Real world: register a passkey on a site that allows presence-only security keys; confirm sign in.

## 8. Delivery phases (each independently testable)

1. CBOR helpers + authenticatorGetInfo. Prove `Ctap2(dev).get_info()` returns FIDO_2_0 over CTAPHID_CBOR.
2. makeCredential, non-resident. ES256 credential, self attestation, presence. python-fido2 verifies attestation.
3. getAssertion, non-resident. Sign + counter; full make/get round trip verified.
4. Resident credentials. cred_store (NVS), rk=true make + discoverable get, enumerate.
5. Polish + real site. AAGUID, minimal account picker, a real passkey registration.

## 9. Open risks

- Multi-packet CTAPHID: CTAP2 responses (attestation objects) exceed 64 bytes, so the transport's fragmentation (already implemented and unit-tested) gets its first real multi-packet exercise on device; watch the send pacing.
- COSE / authenticatorData byte-exactness: these are signed, so any layout error fails signature verification. Native vectors guard this.
- NVS wear and capacity: many resident credentials plus per-assertion counter writes; acceptable at this scale, revisit with wear-levelling if it grows.
- uv=false passkeys: some relying parties require user verification for passwordless and will reject presence-only. Expected; full passwordless arrives with Phase 3 PIN.
