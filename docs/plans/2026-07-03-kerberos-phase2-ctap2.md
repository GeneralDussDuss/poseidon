# KERBEROS Phase 2 (CTAP2 passkeys) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add CTAP2 (getInfo, makeCredential, getAssertion) to KERBEROS so the Cardputer works as a WebAuthn passkey, both non-resident and resident, presence-only.

**Architecture:** A portable, hardware-free CTAP2 core (`lib/kerberos_core/ctap2.*`, `cbor_util.*`, `cred_store.*`) built on vendored TinyCBOR and the injected Phase 1 crypto vtable, unit-tested natively. The device layer routes CTAPHID_CBOR to it and backs the credential store with NVS. Everything else (transport, crypto, keywrap, boot key mode, presence UI) is reused from Phase 1 unchanged.

**Tech Stack:** ESP32-S3, Arduino/pioarduino, TinyUSB (working direct `tud_hid_n_report` path), mbedtls, vendored TinyCBOR, PlatformIO Unity (native tests), python-fido2 (on-device CTAP2 verification).

## Global Constraints

- Board/framework/flags unchanged from Phase 1: `board = m5stack-stamps3`, `framework = arduino`, `-DARDUINO_USB_MODE=1 -DARDUINO_USB_CDC_ON_BOOT=1`, `-std=gnu++17`.
- The core library (`lib/kerberos_core`) must compile on the host with no Arduino/ESP-IDF/M5 headers. TinyCBOR is vendored so it compiles in both places.
- Reuse, do not reimplement: `ctaphid*`, `kerb_crypto.h` vtable, `keywrap`, `kerberos_bootmode`, `kerberos_crypto` (mbedtls), the attestation cert, and `kerberos_user_present`.
- CTAP2 responses must use canonical CBOR: definite lengths, and map keys emitted in canonical order (integer keys ascending; for the same we always hard-code the correct order). We control encode order; TinyCBOR does not reorder for us.
- Crypto is ES256 / COSE alg -7 (P-256) only. Attestation is "packed" self attestation.
- Copy rule for user-facing strings and docs: avoid hyphens and dashes in prose.
- Device must be in KEY MODE for any on-device test (FIDO enumerated). Raw FIDO access on Windows needs an elevated process. Test harness pattern from Phase 1 is in the scratchpad.
- Commit after every task; tests pass before commit.

---

## Scope

CTAP2 getInfo, makeCredential, getAssertion, with non-resident and resident credentials, presence only. Excluded (later phases): clientPIN / user verification (Phase 3), eFuse-hardened storage (Phase 4), credProtect / largeBlob / hmac-secret, enterprise attestation. Resident credentials use the same plain-NVS device key as Phase 1; Phase 4 hardens it.

## File structure

Portable core (host-testable):
- `lib/tinycbor/` — vendored TinyCBOR source (`cbor.h`, `cborencoder.c`, `cborparser.c`, and their private headers). Compiles on host and device.
- `lib/kerberos_core/cbor_util.h` / `.cpp` — small helpers wrapping TinyCBOR for the map/array/bytestring patterns CTAP2 uses, so command code stays readable.
- `lib/kerberos_core/cose.h` / `.cpp` — COSE ES256 public-key CBOR encoding (used inside attestedCredentialData).
- `lib/kerberos_core/authdata.h` / `.cpp` — authenticatorData assembly (rpIdHash, flags, signCount, attestedCredentialData).
- `lib/kerberos_core/cred_store.h` / `.cpp` — resident credential record interface; in-memory impl for tests.
- `lib/kerberos_core/ctap2.h` / `.cpp` — `ctap2_handle()`: parse command, dispatch to getInfo / makeCredential / getAssertion.

Device layer:
- `src/features/kerberos_cred_store_nvs.cpp` — NVS-backed `cred_store` implementation (Preferences).
- `src/features/kerberos.cpp` — build `ctap2_cfg_t`, register the CTAP2 handler.
- `lib/kerberos_core/ctaphid_dispatch.cpp` — route `CTAPHID_CBOR` (0x10) to `ctap2_handle`; set CBOR capability in INIT.

Tests:
- `test/test_kerberos_ctap2/` — Unity suite for cbor_util, cose, authdata, ctap2 commands, cred_store (in-memory).

---

### Task 0: Vendor TinyCBOR and prove it builds host + device

**Files:**
- Create: `lib/tinycbor/` (vendored: `cbor.h`, `cborencoder.c`, `cborparser.c`, `cborinternal_p.h`, `compilersupport_p.h`, `tinycbor-version.h`)
- Create: `lib/tinycbor/library.json` (so PlatformIO treats it as a lib)
- Create: `test/test_kerberos_ctap2/test_main.cpp`

**Interfaces:**
- Produces: TinyCBOR available to both `native-test` and `cardputer` builds via `#include "cbor.h"`.

- [ ] **Step 1: Vendor the source**

Download the pinned TinyCBOR sources (from `github.com/intel/tinycbor`, tag `v0.6.0`, `src/` directory) into `lib/tinycbor/`. Needed files: `cbor.h`, `cborencoder.c`, `cborparser.c`, `cborinternal_p.h`, `compilersupport_p.h`, `tinycbor-version.h`. Do not vendor the open_memstream / stdio helpers (`cborencoder_close_container_checked.c` is optional and can be included; the float/stdio parsers are not needed).

- [ ] **Step 2: Mark it a PlatformIO library**

`lib/tinycbor/library.json`:

```json
{ "name": "tinycbor", "version": "0.6.0", "build": { "srcDir": "." } }
```

- [ ] **Step 3: Write a round-trip test**

`test/test_kerberos_ctap2/test_main.cpp`:

```cpp
#include <unity.h>
#include <string.h>
#include "cbor.h"

void setUp(void) {}
void tearDown(void) {}

// Encode {1: 2} and confirm the canonical bytes A1 01 02.
static void test_cbor_encode_small_map(void) {
    uint8_t buf[16]; CborEncoder enc, map;
    cbor_encoder_init(&enc, buf, sizeof buf, 0);
    cbor_encoder_create_map(&enc, &map, 1);
    cbor_encode_int(&map, 1);
    cbor_encode_int(&map, 2);
    cbor_encoder_close_container(&enc, &map);
    size_t n = cbor_encoder_get_buffer_size(&enc, buf);
    TEST_ASSERT_EQUAL_UINT(3, n);
    uint8_t want[] = {0xA1, 0x01, 0x02};
    TEST_ASSERT_EQUAL_MEMORY(want, buf, 3);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_cbor_encode_small_map);
    return UNITY_END();
}
```

- [ ] **Step 4: Add the include path to the native-test env**

In `platformio.ini`, extend the `[env:native-test]` `build_flags` with `-I lib/tinycbor`:

```ini
build_flags   = -std=gnu++17 -I lib/kerberos_core -I lib/tinycbor
```

- [ ] **Step 5: Run the native test**

Run (from the Phase 1 memory: prepend MinGW to PATH):
`export PATH="/c/Users/D/AppData/Local/Microsoft/WinGet/Packages/BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe/mingw64/bin:$PATH"; python -m platformio test -e native-test`
Expected: `test_cbor_encode_small_map PASSED`.

- [ ] **Step 6: Confirm the device build still links**

Run: `python -m platformio run -d "<repo>" -e cardputer` (xtensa on PATH). Expected: SUCCESS (TinyCBOR compiles for target, no symbol clash with the framework cbor since nothing else references it).

- [ ] **Step 7: Commit**

```bash
git add lib/tinycbor test/test_kerberos_ctap2/test_main.cpp platformio.ini
git commit -m "build(kerberos): vendor TinyCBOR for host + device CBOR"
```

---

### Task 1: cbor_util helpers

**Files:**
- Create: `lib/kerberos_core/cbor_util.h` / `.cpp`
- Test: add to `test/test_kerberos_ctap2/` (new file `test_cbor_util.cpp`, its own `main`)

**Interfaces:**
- Consumes: TinyCBOR.
- Produces:
  - `struct cbor_writer { CborEncoder enc; CborEncoder map; uint8_t *buf; size_t cap; };`
  - `void cw_init(cbor_writer*, uint8_t *buf, size_t cap);`
  - `void cw_map(cbor_writer*, size_t pairs);` open the top map with `pairs` entries.
  - `void cw_key(cbor_writer*, int key);` / `void cw_bytes(cbor_writer*, const uint8_t*, size_t);` / `void cw_text(cbor_writer*, const char*);` / `void cw_uint(cbor_writer*, uint64_t);` / `void cw_bool(cbor_writer*, bool);`
  - `size_t cw_finish(cbor_writer*);` close and return byte length.
  - Parser side: `int cbor_get_map(const uint8_t*, size_t, CborValue *out_root, CborValue *out_map);` and typed getters by integer key: `int cbor_map_bytes(CborValue *map, int key, uint8_t *dst, size_t *len);`, `int cbor_map_text(...)`, `int cbor_map_uint(...)`, `int cbor_map_bool(...)`, `int cbor_map_enter(CborValue *map, int key, CborValue *out)`.

- [ ] **Step 1: Write failing tests** — encode a two-key map `{1: h'AABB', 3: "abc"}` and assert exact canonical bytes; parse it back and read key 1 as bytes and key 3 as text. (Provide the exact expected byte array `A2 01 42 AA BB 03 63 61 62 63`.)

```cpp
static void test_write_and_parse(void) {
    uint8_t buf[64]; cbor_writer w; cw_init(&w, buf, sizeof buf);
    cw_map(&w, 2);
    cw_key(&w, 1); uint8_t bs[2]={0xAA,0xBB}; cw_bytes(&w, bs, 2);
    cw_key(&w, 3); cw_text(&w, "abc");
    size_t n = cw_finish(&w);
    uint8_t want[] = {0xA2,0x01,0x42,0xAA,0xBB,0x03,0x63,'a','b','c'};
    TEST_ASSERT_EQUAL_UINT(sizeof want, n);
    TEST_ASSERT_EQUAL_MEMORY(want, buf, n);

    CborValue root, map; TEST_ASSERT_EQUAL_INT(0, cbor_get_map(buf, n, &root, &map));
    uint8_t got[4]; size_t gl = sizeof got;
    TEST_ASSERT_EQUAL_INT(0, cbor_map_bytes(&map, 1, got, &gl));
    TEST_ASSERT_EQUAL_UINT(2, gl); TEST_ASSERT_EQUAL_MEMORY(bs, got, 2);
    char t[8]; size_t tl = sizeof t;
    TEST_ASSERT_EQUAL_INT(0, cbor_map_text(&map, 3, t, &tl));
    TEST_ASSERT_EQUAL_STRING("abc", t);
}
```

- [ ] **Step 2: Run, expect compile failure** (`cbor_util.h` missing).
- [ ] **Step 3: Implement `cbor_util.h` / `.cpp`** using TinyCBOR (`cbor_encoder_*`, `cbor_value_map_find_value`, `cbor_value_copy_byte_string`, etc.). The map-find helpers use `cbor_value_map_find_value` with an integer key encoded as a CBOR int (TinyCBOR matches by value).
- [ ] **Step 4: Run tests, expect PASS.**
- [ ] **Step 5: Commit** `feat(kerberos): CBOR writer/parser helpers over TinyCBOR`.

---

### Task 2: authenticatorGetInfo + CTAPHID_CBOR routing

**Files:**
- Create: `lib/kerberos_core/ctap2.h` / `.cpp`
- Modify: `lib/kerberos_core/ctaphid_dispatch.cpp` (route CBOR, set capability)
- Modify: `src/features/kerberos.cpp` (register CTAP2 handler)
- Test: `test/test_kerberos_ctap2/test_ctap2.cpp`

**Interfaces:**
- Consumes: `cbor_util`, the `kerb_crypto_t` vtable.
- Produces:
  - `typedef struct { const kerb_crypto_t *cy; const uint8_t *devkey; const uint8_t aaguid[16]; const uint8_t *att_cert; uint16_t att_cert_len; const uint8_t *att_priv; bool (*user_present)(void*); void *ui; struct cred_store *store; } ctap2_cfg_t;`
  - `uint16_t ctap2_handle(const ctap2_cfg_t*, const uint8_t *req, uint16_t len, uint8_t *out, uint16_t cap);` — `out[0]` is the CTAP2 status byte (0x00 success), followed by response CBOR. Returns total length.
  - Command constants: `CTAP2_GET_INFO=0x04, CTAP2_MAKE_CRED=0x01, CTAP2_GET_ASSERT=0x02`.
- Wire byte for CTAPHID CBOR command: `0x90` (init packet CMD = 0x80 | 0x10).

- [ ] **Step 1: Write failing test** — call `ctap2_handle` with a one-byte request `{0x04}` (getInfo), assert `out[0]==0x00` and that the response CBOR contains a `versions` array including `"FIDO_2_0"`. (Parse with cbor_util: enter key 1, iterate array, match string.)
- [ ] **Step 2: Run, expect compile failure.**
- [ ] **Step 3: Implement getInfo** in `ctap2.cpp`: status 0x00, then a canonical map `{1: [versions], 3: aaguid(16 bytes), 4: {options}}` where versions = `["U2F_V2","FIDO_2_0"]` and options = `{"rk": true, "up": true}`. Emit keys in ascending order (1,3,4). Route other commands to `makeCredential`/`getAssertion` (added later) or return `CTAP1_ERR_INVALID_COMMAND` (0x01) for now.
- [ ] **Step 4: Run native test, expect PASS.**
- [ ] **Step 5: Wire the transport.** In `ctaphid_dispatch.cpp`, add a `W_CBOR=0x90` case that calls a registered `ctap2_msg_fn` (same pattern as the U2F `on_msg`) and returns its bytes as a CTAPHID_CBOR response; set the CBOR bit in the INIT capabilities (`resp[16] |= 0x04` already WINK; add `0x04`? use the FIDO CBOR capability flag `0x04` = CBOR, `0x08` = NMSG). Set capabilities to `0x04` (CBOR) plus keep WINK. In `kerberos.cpp`, build `ctap2_cfg_t` and register the handler beside the U2F one.
- [ ] **Step 6: On-device check.** Enter key mode; run the elevated python: `from fido2.ctap2 import Ctap2; info = Ctap2(dev).get_info(); print(info.versions)`. Expected: includes `FIDO_2_0`. (Extends the Phase 1 harness; write results to a file.)
- [ ] **Step 7: Commit** `feat(kerberos): CTAP2 authenticatorGetInfo over CTAPHID_CBOR`.

---

### Task 3: COSE ES256 key + authenticatorData

**Files:**
- Create: `lib/kerberos_core/cose.h` / `.cpp`, `lib/kerberos_core/authdata.h` / `.cpp`
- Test: extend `test_ctap2.cpp`

**Interfaces:**
- Produces:
  - `size_t cose_es256_from_pubkey(const uint8_t pub[65], uint8_t *out, size_t cap);` — encodes the COSE_Key map `{1:2, 3:-7, -1:1, -2:X(32), -3:Y(32)}` in canonical order, from an uncompressed P-256 point (`pub[1..32]`=X, `pub[33..64]`=Y). Returns length.
  - `size_t authdata_build(const uint8_t rpIdHash[32], uint8_t flags, uint32_t signCount, const uint8_t *attCredData, size_t attLen, uint8_t *out, size_t cap);` — concatenates rpIdHash(32) || flags(1) || signCount(4 big-endian) || optional attestedCredentialData. Flags: bit0 UP=0x01, bit6 AT=0x40.
  - `size_t att_cred_data(const uint8_t aaguid[16], const uint8_t *credId, uint8_t credIdLen, const uint8_t *cosePub, size_t coseLen, uint8_t *out, size_t cap);` — aaguid(16) || credIdLen(2 big-endian) || credId || cosePub.

- [ ] **Step 1: Write tests** — COSE encode a known pubkey and assert the leading bytes `A5 01 02 03 26 20 01 21 58 20 <X..>`; authdata_build with AT flag and assert byte 32 == 0x41 (UP|AT) and bytes 33-36 == the counter big-endian.
- [ ] **Step 2: Run, expect fail. Step 3: Implement. Step 4: Run, PASS.**
- [ ] **Step 5: Commit** `feat(kerberos): COSE ES256 key and authenticatorData builders`.

---

### Task 4: authenticatorMakeCredential (non-resident)

**Files:**
- Modify: `lib/kerberos_core/ctap2.cpp`
- Test: extend `test_ctap2.cpp`

**Interfaces:**
- Consumes: cose, authdata, keywrap, crypto vtable, cfg.
- Produces: makeCredential handling inside `ctap2_handle`. Response map `{1: fmt "packed", 2: authData(bytes), 3: attStmt}` where attStmt = `{"alg": -7, "sig": bytes}` (self attestation: sig over authData || clientDataHash with the credential private key), canonical key order (fmt/authData/attStmt map keys are 1,2,3).

- [ ] **Step 1: Write test** — build a makeCredential request CBOR `{1: clientDataHash(32), 2: {id:"example.com"}, 3: {id: h'..', name:"a"}, 4: [{alg:-7, type:"public-key"}]}` (use cbor_util to build the request in the test), call `ctap2_handle`, assert status 0x00, response key 1 == "packed", and that the authData (key 2) has the AT flag set and length ≥ 37+16+2+credIdLen+coseLen. With the mock crypto, verify deterministically.
- [ ] **Step 2: fail. Step 3: implement** — parse clientDataHash (key1), rp.id → SHA-256 = rpIdHash, user (key3), pubKeyCredParams (key4, require -7). Presence via `cfg->user_present`. keygen P-256. Non-resident: credId = wrapped key (keywrap under rpIdHash). Build COSE pub, attCredData, authData (flags UP|AT), self-attestation sig (sign SHA-256 of authData||clientDataHash with the *credential* private key for self attestation; alg -7). Emit response. **Step 4: PASS.**
- [ ] **Step 5: On-device** — `Ctap2` make_credential via python-fido2 with a presence press; verify the returned attestation object parses and the attStmt signature verifies. **Step 6: Commit.**

---

### Task 5: authenticatorGetAssertion (non-resident)

**Files:**
- Modify: `lib/kerberos_core/ctap2.cpp`
- Test: extend `test_ctap2.cpp`

**Interfaces:**
- Produces: getAssertion handling. Response map `{1: credential {id,type}, 2: authData(bytes, no AT flag, UP set), 3: signature}`. Non-resident: unwrap each allowList credId against rpIdHash; first that authenticates is used.

- [ ] **Step 1: Test** — make a credential (Task 4 path), then build a getAssertion request `{1: rpId "example.com", 2: clientDataHash, 3: [{id: credId, type:"public-key"}]}`, call handle, assert status 0x00, authData UP flag set (byte32 bit0), signCount incremented, and the signature verifies against the credential public key from make (use python-fido2-style verify in a helper, or check the signed message layout deterministically with the mock).
- [ ] **Step 2: fail. Step 3: implement** — parse rpId→hash, clientDataHash, allowList. Unwrap credId; presence; authData (flags UP only, signCount++); sign authData||clientDataHash with the credential key. Emit. **Step 4: PASS.**
- [ ] **Step 5: On-device** — full python-fido2 `make_credential` then `get_assertion` round trip, both signatures verified. **Step 6: Commit.**

---

### Task 6: cred_store (in-memory + NVS)

**Files:**
- Create: `lib/kerberos_core/cred_store.h` / `.cpp` (interface + in-memory impl for tests)
- Create: `src/features/kerberos_cred_store_nvs.cpp` (device NVS impl)
- Test: `test/test_kerberos_ctap2/test_cred_store.cpp`

**Interfaces:**
- Produces:
  - `struct cred_record { uint8_t id[32]; uint8_t rpIdHash[32]; uint8_t userId[64]; uint8_t userIdLen; char name[32]; uint8_t wrappedKey[60]; uint32_t signCount; };`
  - `struct cred_store { int (*add)(cred_store*, const cred_record*); int (*find_by_rp)(cred_store*, const uint8_t rpIdHash[32], cred_record *out, int index, int *total); int (*update_counter)(cred_store*, const uint8_t id[32], uint32_t newCount); };`
  - In-memory impl `cred_store *cred_store_mem(void);` for tests; NVS impl `cred_store *cred_store_nvs(void);` on device (Preferences namespace `kerberos`, keys `rk0..rkN` plus a count).

- [ ] **Step 1: Test (in-memory)** — add two records with the same rpIdHash, `find_by_rp` returns total=2 and each by index; `update_counter` changes one. **Step 2: fail. Step 3: implement in-memory. Step 4: PASS. Step 5: Commit.**
- [ ] The NVS impl is exercised on-device in Task 7 (no host test).

---

### Task 7: Resident credentials (rk=true)

**Files:**
- Modify: `lib/kerberos_core/ctap2.cpp` (rk path in make + discoverable get)
- Modify: `src/features/kerberos.cpp` (wire `cred_store_nvs`)
- Test: extend `test_ctap2.cpp` with the in-memory store

**Interfaces:**
- Consumes: cred_store.
- Produces: makeCredential with options.rk=true stores a `cred_record` (random 32-byte credId, wrapped key, rpIdHash, user); getAssertion with an empty allowList looks up by rpIdHash and returns the first match with `numberOfCredentials`.

- [ ] **Step 1: Test** — make with rk=true into the in-memory store; getAssertion with empty allowList returns the credential and a valid signature; store has one record. **Step 2: fail. Step 3: implement. Step 4: PASS.**
- [ ] **Step 5: On-device** — python-fido2 discoverable credential: `make_credential(rk=True)` then `get_assertion` with no allowList; confirm sign in and that the credential survives a reboot (counter persisted, record in NVS). **Step 6: Commit.**

---

### Task 8: AAGUID, capability polish, real site

**Files:**
- Modify: `src/features/kerberos.cpp` (fixed AAGUID), `lib/kerberos_core/ctaphid_dispatch.cpp` (capabilities), `docs/`

- [ ] **Step 1:** Set a stable 16-byte AAGUID constant for KERBEROS (random once, hard-coded). **Step 2:** Confirm INIT advertises CBOR capability and getInfo reports rk. **Step 3:** Register a passkey on a real relying party that allows presence-only keys; sign in. Document the result. **Step 4: Commit.**

---

## Self-Review

**Spec coverage:** getInfo (T2), makeCredential non-resident (T4) + resident (T7), getAssertion non-resident (T5) + discoverable (T7), CBOR via vendored TinyCBOR (T0/T1), COSE + authenticatorData (T3), cred_store NVS (T6/T7), AAGUID + capabilities (T8). Non-goals (PIN, eFuse, credProtect) correctly absent. Multi-packet transport risk exercised first at T4 (attestation object > 64 bytes) and noted.

**Placeholder scan:** No TBD/"handle errors" hand-waving; pure-logic tasks ship real test code and byte-exact expectations. Device-only steps (NVS, enumeration, real site) carry explicit on-device verification, the correct form for firmware.

**Type consistency:** `ctap2_cfg_t`, `cbor_writer`, `cred_store`/`cred_record`, and the command constants are used identically across tasks. `ctap2_handle` signature matches the `ctap2_msg_fn` registered in T2. COSE/authdata builders' signatures match their callers in T4/T5. keywrap and the crypto vtable are the Phase 1 types, unchanged.

## Notes carried forward

- Phase 3 adds clientPIN/UV: getInfo will then advertise `clientPin`, options gain `uv`, and make/get honour the `pinUvAuthParam`. The `ctap2_cfg_t` will gain a PIN state pointer.
- Phase 4 replaces the plain-NVS device key (used by keywrap and cred_store) with the eFuse+PIN derived key and migrates resident records.
- Canonical CBOR is our responsibility on encode; keep the hard-coded key order in each response map correct (ascending integer keys).
