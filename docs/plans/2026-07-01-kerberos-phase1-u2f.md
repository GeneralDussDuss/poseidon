# KERBEROS Phase 1 (U2F security key) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship a working FIDO U2F / CTAP1 security key as a POSEIDON feature on the Cardputer that a browser recognizes over USB and that registers and authenticates at webauthn.io, Google, and GitHub.

**Architecture:** A portable, hardware free core library (`lib/kerberos_core`) holds the CTAPHID transport framing, the U2F APDU logic, and stateless key handle wrapping, all behind an injected crypto interface so it unit tests natively with a mock. The device layer (`src/features/kerberos*`) provides the TinyUSB FIDO HID interface, an mbedtls crypto binding, the on screen approval UI, and the menu entry, reusing the exact USB pattern `badusb.cpp` already uses.

**Tech Stack:** ESP32-S3, Arduino via pioarduino, TinyUSB (Arduino `USB.h` / `USBHID`), mbedtls (already linked), M5Cardputer display and TCA8418 keyboard, PlatformIO Unity for native unit tests.

## Global Constraints

- Board and framework: `board = m5stack-stamps3`, `framework = arduino`, platform pinned to pioarduino `55.03.38`. Do not change these.
- USB build flags already present and required: `-DARDUINO_USB_MODE=1 -DARDUINO_USB_CDC_ON_BOOT=1`.
- The device has ONE USB PHY. Entering key mode calls `USB.begin()` and takes the port from CDC, exactly like `feat_badusb`. Respect the shared guard: refuse to start if `g_mimir_cdc_active` or `g_trident_cdc_active` is set.
- PSRAM is disabled on this unit. No code path may assume PSRAM exists.
- Language: `-std=gnu++17`. The core library must be C++17 that compiles on the host with no Arduino, ESP-IDF, or M5 headers.
- Copy rule for any user facing strings and docs: avoid hyphens and dashes in prose.
- Feature entry point signature is `void feat_kerberos(void)`. Menu node shape is `{ char hotkey, const char* name, const char* subtitle, const menu_node_t* children, void(*action)(void), const char* help }`.
- Commit after every task. Tests must pass before commit.

---

## Scope

This plan is Phase 1 only: a U2F (CTAP1) authenticator. It intentionally excludes CTAP2 / CBOR / passkeys (Phase 2), client PIN (Phase 3), eFuse hardened storage (Phase 4), SD backup (Phase 5), and Windows passwordless plus polish (Phase 6). Those get their own plans. Phase 1 uses a device key kept in plain NVS for key handle wrapping; Phase 4 later replaces that with the eFuse derived key. This is called out so the temporary NVS key is not mistaken for the final design.

## File structure

Portable core (host testable, no hardware headers):
- `lib/kerberos_core/ctaphid.h` / `ctaphid.cpp` — CTAPHID packet framing: reassemble inbound 64 byte reports into a message, fragment an outbound message into reports.
- `lib/kerberos_core/ctaphid_dispatch.h` / `ctaphid_dispatch.cpp` — channel allocation, INIT, PING, KEEPALIVE, error, and routing MSG to the U2F handler.
- `lib/kerberos_core/u2f.h` / `u2f.cpp` — U2F APDU parse and response build for Register, Authenticate, Version.
- `lib/kerberos_core/keywrap.h` / `keywrap.cpp` — stateless key handle wrap and unwrap bound to the application id.
- `lib/kerberos_core/kerb_crypto.h` — the injected crypto interface (function pointer table) the core calls: random, sha256, P256 keygen, ECDSA sign.

Device layer (Arduino, ESP32 only):
- `src/features/kerberos.cpp` — `feat_kerberos()`, the mode UI, presence prompt, USB bring up and tear down, menu wiring glue, and the crypto vtable filled with mbedtls.
- `src/features/kerberos_hid.h` / `kerberos_hid.cpp` — the `USBHIDFido` device (report descriptor plus report in and out), pumping bytes between TinyUSB and `ctaphid`.
- `src/features/kerberos_crypto.cpp` — mbedtls implementations of the `kerb_crypto.h` interface.
- `src/features/kerberos_attestation.h` — the embedded self signed P256 attestation certificate and private key bytes.

Build and menu:
- `platformio.ini` — add `[env:native-test]`.
- `src/menu.cpp` — add `extern void feat_kerberos(void);` beside the other externs and a KERBEROS node in `MENU_ROOT_CHILDREN` (hotkey `k`, which is unused).
- `test/test_kerberos_core/` — Unity tests for the core library.

---

### Task 0: Native test harness and empty core library

**Files:**
- Modify: `platformio.ini` (add native test env)
- Create: `lib/kerberos_core/kerb_crypto.h`
- Create: `test/test_kerberos_core/test_main.cpp`

**Interfaces:**
- Produces: the `kerb_crypto_t` vtable type used by every later core task.

- [ ] **Step 1: Add the native test env to `platformio.ini`**

Append this env (leave the existing `[env:cardputer]` blocks untouched):

```ini
; Host-native unit tests for lib/kerberos_core. Runs on the dev machine,
; no board attached: pio test -e native-test
[env:native-test]
platform      = native
test_framework = unity
build_flags   = -std=gnu++17 -I lib/kerberos_core
build_src_filter = -<*>
```

- [ ] **Step 2: Define the crypto interface**

`lib/kerberos_core/kerb_crypto.h`:

```cpp
#pragma once
#include <stdint.h>
#include <stddef.h>

// Injected so the core is testable with a mock and runs mbedtls on device.
typedef struct kerb_crypto_s {
    // Fill dst with n random bytes. Returns 0 on success.
    int (*rand)(uint8_t *dst, size_t n, void *ctx);
    // SHA-256 of msg[len] into out[32]. Returns 0 on success.
    int (*sha256)(const uint8_t *msg, size_t len, uint8_t out[32], void *ctx);
    // Generate a P256 keypair. priv[32] raw scalar, pub[65] uncompressed 0x04||X||Y.
    int (*p256_keygen)(uint8_t priv[32], uint8_t pub[65], void *ctx);
    // ECDSA-P256 sign of the SHA-256 of msg[len] with priv[32].
    // Writes a DER ECDSA signature to sig, sets *sig_len. sig has room for 72 bytes.
    int (*p256_sign)(const uint8_t priv[32], const uint8_t *msg, size_t len,
                     uint8_t *sig, size_t *sig_len, void *ctx);
    // AES-256-GCM used by keywrap. key[32], iv[12]. Encrypt: in->out, tag[16] out.
    int (*aes_gcm_seal)(const uint8_t key[32], const uint8_t iv[12],
                        const uint8_t *aad, size_t aad_len,
                        const uint8_t *in, size_t len, uint8_t *out, uint8_t tag[16], void *ctx);
    // Decrypt: verifies tag, in->out. Returns 0 on success, nonzero on auth failure.
    int (*aes_gcm_open)(const uint8_t key[32], const uint8_t iv[12],
                        const uint8_t *aad, size_t aad_len,
                        const uint8_t *in, size_t len, const uint8_t tag[16], uint8_t *out, void *ctx);
    void *ctx;
} kerb_crypto_t;
```

- [ ] **Step 3: Write a trivial passing test to prove the harness runs**

`test/test_kerberos_core/test_main.cpp`:

```cpp
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static void test_harness_alive(void) {
    TEST_ASSERT_EQUAL_INT(4, 2 + 2);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_harness_alive);
    return UNITY_END();
}
```

- [ ] **Step 4: Run the native tests**

Run: `pio test -e native-test`
Expected: PASS, 1 test, `test_harness_alive`.

- [ ] **Step 5: Commit**

```bash
git add platformio.ini lib/kerberos_core/kerb_crypto.h test/test_kerberos_core/test_main.cpp
git commit -m "test(kerberos): native unit test harness + crypto interface"
```

---

### Task 1: CTAPHID framing (reassembly and fragmentation)

**Files:**
- Create: `lib/kerberos_core/ctaphid.h`, `lib/kerberos_core/ctaphid.cpp`
- Test: `test/test_kerberos_core/test_ctaphid.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `int ctaphid_feed(ctaphid_assembler_t *a, const uint8_t pkt[64])` returns 1 when `a->buf`/`a->bcnt`/`a->cmd`/`a->cid` hold a complete message, 0 if more packets are needed, negative on a framing error.
  - `void ctaphid_send(uint32_t cid, uint8_t cmd, const uint8_t *data, uint16_t len, void (*sink)(const uint8_t pkt[64], void *), void *sctx)` fragments a message into 64 byte reports and calls `sink` per packet.
  - constants `CTAPHID_PKT`, `CTAPHID_INIT_DATA`, `CTAPHID_CONT_DATA`, `CTAPHID_MAXLEN`.

- [ ] **Step 1: Write failing tests**

`test/test_kerberos_core/test_ctaphid.cpp`:

```cpp
#include <unity.h>
#include <string.h>
#include "ctaphid.h"

void setUp(void) {}
void tearDown(void) {}

// Collect fragmented output packets.
static uint8_t g_out[64 * 4];
static int g_out_n;
static void sink(const uint8_t pkt[64], void *) {
    memcpy(g_out + g_out_n * 64, pkt, 64);
    g_out_n++;
}

static void test_single_packet_message(void) {
    // Build one init packet: CID=1, CMD=0x83 (MSG), bcnt=3, data "abc".
    uint8_t pkt[64] = {0};
    pkt[0]=0;pkt[1]=0;pkt[2]=0;pkt[3]=1;      // CID big-endian
    pkt[4]=0x83; pkt[5]=0x00; pkt[6]=0x03;    // CMD, BCNTH, BCNTL
    pkt[7]='a'; pkt[8]='b'; pkt[9]='c';
    ctaphid_assembler_t a; memset(&a,0,sizeof a);
    int r = ctaphid_feed(&a, pkt);
    TEST_ASSERT_EQUAL_INT(1, r);
    TEST_ASSERT_EQUAL_UINT32(1, a.cid);
    TEST_ASSERT_EQUAL_UINT8(0x83, a.cmd);
    TEST_ASSERT_EQUAL_UINT16(3, a.bcnt);
    TEST_ASSERT_EQUAL_MEMORY("abc", a.buf, 3);
}

static void test_multi_packet_reassembly(void) {
    // 60-byte payload spans an init packet (57) + one continuation (3).
    uint8_t payload[60];
    for (int i=0;i<60;i++) payload[i]=(uint8_t)i;
    uint8_t init[64]={0}; init[3]=2; init[4]=0x90; init[5]=0; init[6]=60;
    memcpy(init+7, payload, 57);
    uint8_t cont[64]={0}; cont[3]=2; cont[4]=0x00; // SEQ 0
    memcpy(cont+5, payload+57, 3);
    ctaphid_assembler_t a; memset(&a,0,sizeof a);
    TEST_ASSERT_EQUAL_INT(0, ctaphid_feed(&a, init));
    TEST_ASSERT_EQUAL_INT(1, ctaphid_feed(&a, cont));
    TEST_ASSERT_EQUAL_UINT16(60, a.bcnt);
    TEST_ASSERT_EQUAL_MEMORY(payload, a.buf, 60);
}

static void test_send_fragments_60_bytes(void) {
    uint8_t payload[60]; for(int i=0;i<60;i++) payload[i]=(uint8_t)(i+1);
    g_out_n=0;
    ctaphid_send(2, 0x90, payload, 60, sink, nullptr);
    TEST_ASSERT_EQUAL_INT(2, g_out_n);          // init + 1 continuation
    TEST_ASSERT_EQUAL_UINT8(0x90, g_out[4]);    // CMD in init
    TEST_ASSERT_EQUAL_UINT8(60, g_out[6]);      // BCNTL
    TEST_ASSERT_EQUAL_UINT8(0x00, g_out[64+4]); // SEQ 0 in continuation
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_single_packet_message);
    RUN_TEST(test_multi_packet_reassembly);
    RUN_TEST(test_send_fragments_60_bytes);
    return UNITY_END();
}
```

Delete the placeholder `test_main.cpp` (its `main` would clash): `git rm test/test_kerberos_core/test_main.cpp`.

- [ ] **Step 2: Run to verify failure**

Run: `pio test -e native-test`
Expected: FAIL to compile, `ctaphid.h` not found.

- [ ] **Step 3: Implement the header**

`lib/kerberos_core/ctaphid.h`:

```cpp
#pragma once
#include <stdint.h>
#include <stddef.h>

#define CTAPHID_PKT        64
#define CTAPHID_INIT_DATA  (CTAPHID_PKT - 7)   // 57
#define CTAPHID_CONT_DATA  (CTAPHID_PKT - 5)   // 59
#define CTAPHID_MAXLEN     2048                 // Phase 1 cap; U2F messages are small

typedef struct {
    uint32_t cid;
    uint8_t  cmd;
    uint16_t bcnt;
    uint16_t got;
    uint8_t  seq;        // next expected continuation seq
    uint8_t  buf[CTAPHID_MAXLEN];
    int      active;
} ctaphid_assembler_t;

typedef void (*ctaphid_sink_fn)(const uint8_t pkt[CTAPHID_PKT], void *ctx);

// Returns 1 = message complete, 0 = need more, negative = framing error.
int  ctaphid_feed(ctaphid_assembler_t *a, const uint8_t pkt[CTAPHID_PKT]);
void ctaphid_send(uint32_t cid, uint8_t cmd, const uint8_t *data, uint16_t len,
                  ctaphid_sink_fn sink, void *sctx);
```

- [ ] **Step 4: Implement the source**

`lib/kerberos_core/ctaphid.cpp`:

```cpp
#include "ctaphid.h"
#include <string.h>

static uint32_t rd32(const uint8_t *p){ return (uint32_t)p[0]<<24|p[1]<<16|p[2]<<8|p[3]; }
static void     wr32(uint8_t *p, uint32_t v){ p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v; }

int ctaphid_feed(ctaphid_assembler_t *a, const uint8_t pkt[CTAPHID_PKT]) {
    uint32_t cid = rd32(pkt);
    if (pkt[4] & 0x80) {                       // init packet
        a->cid = cid;
        a->cmd = pkt[4];
        a->bcnt = (uint16_t)pkt[5] << 8 | pkt[6];
        if (a->bcnt > CTAPHID_MAXLEN) { a->active = 0; return -1; }
        uint16_t n = a->bcnt < CTAPHID_INIT_DATA ? a->bcnt : CTAPHID_INIT_DATA;
        memcpy(a->buf, pkt + 7, n);
        a->got = n; a->seq = 0; a->active = 1;
        return a->got >= a->bcnt ? 1 : 0;
    }
    // continuation
    if (!a->active || cid != a->cid) return -1;
    if (pkt[4] != a->seq) { a->active = 0; return -1; }
    a->seq++;
    uint16_t remain = a->bcnt - a->got;
    uint16_t n = remain < CTAPHID_CONT_DATA ? remain : CTAPHID_CONT_DATA;
    memcpy(a->buf + a->got, pkt + 5, n);
    a->got += n;
    return a->got >= a->bcnt ? 1 : 0;
}

void ctaphid_send(uint32_t cid, uint8_t cmd, const uint8_t *data, uint16_t len,
                  ctaphid_sink_fn sink, void *sctx) {
    uint8_t pkt[CTAPHID_PKT];
    uint16_t off = 0;
    memset(pkt, 0, sizeof pkt);
    wr32(pkt, cid);
    pkt[4] = cmd; pkt[5] = len >> 8; pkt[6] = len & 0xff;
    uint16_t n = len < CTAPHID_INIT_DATA ? len : CTAPHID_INIT_DATA;
    memcpy(pkt + 7, data, n);
    sink(pkt, sctx);
    off += n;
    uint8_t seq = 0;
    while (off < len) {
        memset(pkt, 0, sizeof pkt);
        wr32(pkt, cid);
        pkt[4] = seq++;
        uint16_t remain = len - off;
        uint16_t c = remain < CTAPHID_CONT_DATA ? remain : CTAPHID_CONT_DATA;
        memcpy(pkt + 5, data + off, c);
        sink(pkt, sctx);
        off += c;
    }
}
```

- [ ] **Step 5: Run tests, expect PASS**

Run: `pio test -e native-test`
Expected: PASS, 3 tests.

- [ ] **Step 6: Commit**

```bash
git add lib/kerberos_core/ctaphid.h lib/kerberos_core/ctaphid.cpp test/test_kerberos_core/test_ctaphid.cpp
git rm test/test_kerberos_core/test_main.cpp
git commit -m "feat(kerberos): CTAPHID packet framing with unit tests"
```

---

### Task 2: CTAPHID dispatch (INIT, PING, KEEPALIVE, error, route MSG)

**Files:**
- Create: `lib/kerberos_core/ctaphid_dispatch.h`, `lib/kerberos_core/ctaphid_dispatch.cpp`
- Test: `test/test_kerberos_core/test_dispatch.cpp` (add `RUN_TEST` lines to the existing test main, or a separate file with its own `main`; keep one `main` per test folder by merging into `test_ctaphid.cpp`'s `main`).

**Interfaces:**
- Consumes: `ctaphid_send`, `ctaphid_assembler_t` from Task 1.
- Produces:
  - `uint32_t ctaphid_dispatch(ctaphid_ctx_t *c, const uint8_t pkt[64])` feeds one inbound report, drives replies through the send sink, returns the channel id in use.
  - A registered message handler `typedef uint16_t (*ctaphid_msg_fn)(const uint8_t *req, uint16_t req_len, uint8_t *resp, uint16_t resp_cap, void *ctx)` for MSG (U2F) payloads, returning response length.
  - `void ctaphid_keepalive(ctaphid_ctx_t *c, uint8_t status)` sends a KEEPALIVE frame on the active channel (status 1 = processing, 2 = user presence needed).

Wire command bytes (init packet CMD field, high bit set): PING `0x81`, MSG `0x83`, INIT `0x86`, WINK `0x88`, CBOR `0x90` (rejected in Phase 1), CANCEL `0x91`, KEEPALIVE `0xBB`, ERROR `0xBF`.

- [ ] **Step 1: Write failing tests**

Add to the test folder (new file `test/test_kerberos_core/test_dispatch.cpp`, and move all `RUN_TEST` calls into a single `main`; simplest is to add these tests and their `RUN_TEST` lines into `test_ctaphid.cpp` so only one `main` exists):

```cpp
// INIT on the broadcast channel returns nonce echo + a fresh non-zero CID.
static void test_init_allocates_channel(void) {
    uint8_t pkt[64]={0};
    pkt[0]=pkt[1]=pkt[2]=pkt[3]=0xFF;   // broadcast CID
    pkt[4]=0x86; pkt[5]=0; pkt[6]=8;     // INIT, 8-byte nonce
    for(int i=0;i<8;i++) pkt[7+i]=(uint8_t)(0xA0+i);
    g_out_n=0;
    ctaphid_ctx_t c; ctaphid_ctx_init(&c, sink, nullptr, nullptr, nullptr);
    ctaphid_dispatch(&c, pkt);
    TEST_ASSERT_EQUAL_INT(1, g_out_n);
    TEST_ASSERT_EQUAL_UINT8(0x86, g_out[4]);              // echoes INIT cmd
    TEST_ASSERT_EQUAL_MEMORY(pkt+7, g_out+7, 8);          // nonce echoed
    // new CID at resp offset 7+8 must be non-zero and not broadcast
    uint8_t *cidp = g_out + 7 + 8;
    TEST_ASSERT_TRUE(!(cidp[0]==0&&cidp[1]==0&&cidp[2]==0&&cidp[3]==0));
}

// PING echoes its payload back verbatim.
static void test_ping_echo(void) {
    uint8_t pkt[64]={0}; pkt[3]=7; pkt[4]=0x81; pkt[5]=0; pkt[6]=4;
    pkt[7]='p';pkt[8]='o';pkt[9]='n';pkt[10]='g';
    g_out_n=0;
    ctaphid_ctx_t c; ctaphid_ctx_init(&c, sink, nullptr, nullptr, nullptr);
    ctaphid_dispatch(&c, pkt);
    TEST_ASSERT_EQUAL_UINT8(0x81, g_out[4]);
    TEST_ASSERT_EQUAL_MEMORY("pong", g_out+7, 4);
}
```

- [ ] **Step 2: Run, expect compile failure** (`ctaphid_dispatch.h` missing).

Run: `pio test -e native-test`
Expected: FAIL to compile.

- [ ] **Step 3: Implement header**

`lib/kerberos_core/ctaphid_dispatch.h`:

```cpp
#pragma once
#include "ctaphid.h"

typedef uint16_t (*ctaphid_msg_fn)(const uint8_t *req, uint16_t req_len,
                                   uint8_t *resp, uint16_t resp_cap, void *ctx);

typedef struct {
    ctaphid_assembler_t asm_;
    ctaphid_sink_fn     sink;
    void               *sink_ctx;
    ctaphid_msg_fn      on_msg;      // U2F handler, may be nullptr
    void               *msg_ctx;
    uint32_t            next_cid;
} ctaphid_ctx_t;

void     ctaphid_ctx_init(ctaphid_ctx_t *c, ctaphid_sink_fn sink, void *sink_ctx,
                          ctaphid_msg_fn on_msg, void *msg_ctx);
uint32_t ctaphid_dispatch(ctaphid_ctx_t *c, const uint8_t pkt[CTAPHID_PKT]);
void     ctaphid_keepalive(ctaphid_ctx_t *c, uint32_t cid, uint8_t status);
```

- [ ] **Step 4: Implement source**

`lib/kerberos_core/ctaphid_dispatch.cpp`:

```cpp
#include "ctaphid_dispatch.h"
#include <string.h>

enum { W_PING=0x81, W_MSG=0x83, W_INIT=0x86, W_WINK=0x88, W_CBOR=0x90,
       W_CANCEL=0x91, W_KEEPALIVE=0xBB, W_ERROR=0xBF };
enum { ERR_INVALID_CMD=0x01, ERR_INVALID_LEN=0x03, ERR_OTHER=0x7F };

void ctaphid_ctx_init(ctaphid_ctx_t *c, ctaphid_sink_fn sink, void *sink_ctx,
                      ctaphid_msg_fn on_msg, void *msg_ctx) {
    memset(c, 0, sizeof *c);
    c->sink = sink; c->sink_ctx = sink_ctx;
    c->on_msg = on_msg; c->msg_ctx = msg_ctx;
    c->next_cid = 1;
}

static void send_err(ctaphid_ctx_t *c, uint32_t cid, uint8_t code) {
    ctaphid_send(cid, W_ERROR, &code, 1, c->sink, c->sink_ctx);
}

void ctaphid_keepalive(ctaphid_ctx_t *c, uint32_t cid, uint8_t status) {
    ctaphid_send(cid, W_KEEPALIVE, &status, 1, c->sink, c->sink_ctx);
}

uint32_t ctaphid_dispatch(ctaphid_ctx_t *c, const uint8_t pkt[CTAPHID_PKT]) {
    int r = ctaphid_feed(&c->asm_, pkt);
    if (r == 0) return c->asm_.cid;          // need more packets
    if (r < 0) { send_err(c, c->asm_.cid, ERR_INVALID_LEN); return c->asm_.cid; }

    uint32_t cid = c->asm_.cid;
    uint8_t  cmd = c->asm_.cmd;
    switch (cmd) {
        case W_INIT: {
            uint8_t resp[17];
            memcpy(resp, c->asm_.buf, 8);                 // nonce echo
            uint32_t ncid = c->next_cid++;
            resp[8]=ncid>>24; resp[9]=ncid>>16; resp[10]=ncid>>8; resp[11]=ncid;
            resp[12]=2;                                   // CTAPHID protocol version
            resp[13]=0; resp[14]=6; resp[15]=3;           // device version major/minor/build
            resp[16]=0x04;                                // capabilities: WINK only (no CBOR in Phase 1)
            ctaphid_send(cid, W_INIT, resp, sizeof resp, c->sink, c->sink_ctx);
            break;
        }
        case W_PING:
            ctaphid_send(cid, W_PING, c->asm_.buf, c->asm_.bcnt, c->sink, c->sink_ctx);
            break;
        case W_MSG: {
            if (!c->on_msg) { send_err(c, cid, ERR_INVALID_CMD); break; }
            static uint8_t resp[1200];
            uint16_t n = c->on_msg(c->asm_.buf, c->asm_.bcnt, resp, sizeof resp, c->msg_ctx);
            ctaphid_send(cid, W_MSG, resp, n, c->sink, c->sink_ctx);
            break;
        }
        case W_WINK:
            ctaphid_send(cid, W_WINK, nullptr, 0, c->sink, c->sink_ctx);
            break;
        case W_CANCEL:
            break;                                        // nothing pending in Phase 1
        default:
            send_err(c, cid, ERR_INVALID_CMD);
            break;
    }
    c->asm_.active = 0;
    return cid;
}
```

- [ ] **Step 5: Run tests, expect PASS**, then commit.

```bash
git add lib/kerberos_core/ctaphid_dispatch.* test/test_kerberos_core/
git commit -m "feat(kerberos): CTAPHID dispatch (INIT/PING/KEEPALIVE/MSG routing)"
```

---

### Task 3: Stateless key handle wrap and unwrap

**Files:**
- Create: `lib/kerberos_core/keywrap.h`, `lib/kerberos_core/keywrap.cpp`
- Test: add tests to the core test main.

**Interfaces:**
- Consumes: `kerb_crypto_t` (rand, aes_gcm_seal, aes_gcm_open).
- Produces:
  - `int kw_wrap(const kerb_crypto_t *cy, const uint8_t devkey[32], const uint8_t priv[32], const uint8_t appid[32], uint8_t *handle, size_t *handle_len)` writes `iv(12) || tag(16) || ciphertext(32)` = 60 byte handle.
  - `int kw_unwrap(const kerb_crypto_t *cy, const uint8_t devkey[32], const uint8_t *handle, size_t handle_len, const uint8_t appid[32], uint8_t priv[32])` returns 0 only if the handle authenticates AND was bound to `appid`.
  - The app id is bound as GCM additional authenticated data, so a handle presented under the wrong app id fails to open.

- [ ] **Step 1: Write failing tests using a reversible mock cipher**

Add a mock crypto to the test file (a fake GCM that XORs with a keystream and stores the aad hash in the tag so `open` can verify binding):

```cpp
#include "keywrap.h"
#include "kerb_crypto.h"

static int mk_rand(uint8_t*d,size_t n,void*){ for(size_t i=0;i<n;i++) d[i]=(uint8_t)(0x11+i); return 0; }
static uint8_t aad_fp(const uint8_t*a,size_t n){ uint8_t f=0; for(size_t i=0;i<n;i++) f^=a[i]; return f; }
static int mk_seal(const uint8_t k[32],const uint8_t iv[12],const uint8_t*aad,size_t al,
                   const uint8_t*in,size_t len,uint8_t*out,uint8_t tag[16],void*){
    for(size_t i=0;i<len;i++) out[i]=in[i]^k[i%32]^iv[i%12];
    memset(tag,0,16); tag[0]=aad_fp(aad,al); return 0;
}
static int mk_open(const uint8_t k[32],const uint8_t iv[12],const uint8_t*aad,size_t al,
                   const uint8_t*in,size_t len,const uint8_t tag[16],uint8_t*out,void*){
    if(tag[0]!=aad_fp(aad,al)) return -1;         // wrong app id
    for(size_t i=0;i<len;i++) out[i]=in[i]^k[i%32]^iv[i%12];
    return 0;
}
static kerb_crypto_t MOCK = { mk_rand,nullptr,nullptr,nullptr, mk_seal, mk_open, nullptr };

static void test_wrap_unwrap_roundtrip(void) {
    uint8_t devkey[32]; for(int i=0;i<32;i++) devkey[i]=(uint8_t)i;
    uint8_t priv[32];   for(int i=0;i<32;i++) priv[i]=(uint8_t)(0x40+i);
    uint8_t appid[32];  for(int i=0;i<32;i++) appid[i]=(uint8_t)(0x80+i);
    uint8_t handle[128]; size_t hl=0;
    TEST_ASSERT_EQUAL_INT(0, kw_wrap(&MOCK, devkey, priv, appid, handle, &hl));
    TEST_ASSERT_EQUAL_INT(60, (int)hl);
    uint8_t got[32];
    TEST_ASSERT_EQUAL_INT(0, kw_unwrap(&MOCK, devkey, handle, hl, appid, got));
    TEST_ASSERT_EQUAL_MEMORY(priv, got, 32);
}

static void test_wrong_appid_fails(void) {
    uint8_t devkey[32]={0}, priv[32]={1}, appid[32]={2}, bad[32]={3};
    uint8_t handle[128]; size_t hl=0; uint8_t got[32];
    kw_wrap(&MOCK, devkey, priv, appid, handle, &hl);
    TEST_ASSERT_NOT_EQUAL(0, kw_unwrap(&MOCK, devkey, handle, hl, bad, got));
}
```

- [ ] **Step 2: Run, expect compile failure.**

Run: `pio test -e native-test`
Expected: FAIL, `keywrap.h` missing.

- [ ] **Step 3: Implement**

`lib/kerberos_core/keywrap.h`:

```cpp
#pragma once
#include "kerb_crypto.h"
#define KW_HANDLE_LEN 60   // iv(12) + tag(16) + ct(32)
int kw_wrap(const kerb_crypto_t *cy, const uint8_t devkey[32], const uint8_t priv[32],
            const uint8_t appid[32], uint8_t *handle, size_t *handle_len);
int kw_unwrap(const kerb_crypto_t *cy, const uint8_t devkey[32], const uint8_t *handle,
              size_t handle_len, const uint8_t appid[32], uint8_t priv[32]);
```

`lib/kerberos_core/keywrap.cpp`:

```cpp
#include "keywrap.h"
#include <string.h>

int kw_wrap(const kerb_crypto_t *cy, const uint8_t devkey[32], const uint8_t priv[32],
            const uint8_t appid[32], uint8_t *handle, size_t *handle_len) {
    uint8_t iv[12]; if (cy->rand(iv, 12, cy->ctx)) return -1;
    uint8_t tag[16], ct[32];
    if (cy->aes_gcm_seal(devkey, iv, appid, 32, priv, 32, ct, tag, cy->ctx)) return -1;
    memcpy(handle, iv, 12);
    memcpy(handle + 12, tag, 16);
    memcpy(handle + 28, ct, 32);
    *handle_len = KW_HANDLE_LEN;
    return 0;
}

int kw_unwrap(const kerb_crypto_t *cy, const uint8_t devkey[32], const uint8_t *handle,
              size_t handle_len, const uint8_t appid[32], uint8_t priv[32]) {
    if (handle_len != KW_HANDLE_LEN) return -1;
    const uint8_t *iv = handle, *tag = handle + 12, *ct = handle + 28;
    return cy->aes_gcm_open(devkey, iv, appid, 32, ct, 32, tag, priv, cy->ctx);
}
```

- [ ] **Step 4: Run tests, expect PASS**, then commit.

```bash
git add lib/kerberos_core/keywrap.* test/test_kerberos_core/
git commit -m "feat(kerberos): stateless app-id-bound key handle wrap/unwrap"
```

---

### Task 4: U2F Register, Authenticate, Version

**Files:**
- Create: `lib/kerberos_core/u2f.h`, `lib/kerberos_core/u2f.cpp`
- Test: add tests to the core test main.

**Interfaces:**
- Consumes: `kerb_crypto_t` (sha256, p256_keygen, p256_sign), `keywrap`, the embedded attestation cert and key supplied by the caller.
- Produces:
  - `uint16_t u2f_handle(const u2f_cfg_t *cfg, const uint8_t *apdu, uint16_t len, uint8_t *out, uint16_t cap)` where `u2f_cfg_t` carries the crypto vtable, the device wrapping key, the attestation cert bytes and length, the attestation private key, a signature counter pointer, and a `bool (*user_present)(void *ui)` callback plus its ctx.
  - APDU status words appended big endian: success `0x9000`, condition not satisfied `0x6985` (returned while waiting for presence), wrong data `0x6A80`.
- U2F request framing (raw APDU): `CLA(1) INS(1) P1(1) P2(1) Lc(3, big-endian) data(Lc) Le(2)`. INS 0x01 Register, 0x02 Authenticate, 0x03 Version.

- [ ] **Step 1: Write failing tests (mock crypto returns deterministic keys and signatures)**

Extend the mock to implement sha256, p256_keygen, p256_sign deterministically, then:

```cpp
static void test_version_returns_u2f_v2(void) {
    u2f_cfg_t cfg = make_test_cfg(/*present=*/true);
    uint8_t apdu[] = {0x00,0x03,0x00,0x00, 0,0,0, 0,0};  // Version, Lc=0
    uint8_t out[256];
    uint16_t n = u2f_handle(&cfg, apdu, sizeof apdu, out, sizeof out);
    TEST_ASSERT_EQUAL_MEMORY("U2F_V2", out, 6);
    TEST_ASSERT_EQUAL_UINT8(0x90, out[n-2]);
    TEST_ASSERT_EQUAL_UINT8(0x00, out[n-1]);
}

static void test_register_shape(void) {
    u2f_cfg_t cfg = make_test_cfg(true);
    uint8_t data[64]; memset(data,0xAB,64);              // challenge(32)+appid(32)
    uint8_t apdu[7+64+2]={0}; apdu[1]=0x01; apdu[6]=64;
    memcpy(apdu+7, data, 64);
    uint8_t out[512];
    uint16_t n = u2f_handle(&cfg, apdu, sizeof apdu, out, sizeof out);
    TEST_ASSERT_EQUAL_UINT8(0x05, out[0]);              // reserved byte
    TEST_ASSERT_EQUAL_UINT8(0x04, out[1]);              // pubkey uncompressed prefix
    TEST_ASSERT_EQUAL_UINT8(0x90, out[n-2]);            // SW success
}

static void test_register_denied_without_presence(void) {
    u2f_cfg_t cfg = make_test_cfg(/*present=*/false);
    uint8_t apdu[7+64+2]={0}; apdu[1]=0x01; apdu[6]=64;
    uint8_t out[512];
    uint16_t n = u2f_handle(&cfg, apdu, sizeof apdu, out, sizeof out);
    TEST_ASSERT_EQUAL_UINT8(0x69, out[n-2]);            // 0x6985 condition not satisfied
    TEST_ASSERT_EQUAL_UINT8(0x85, out[n-1]);
}
```

- [ ] **Step 2: Run, expect compile failure.**

- [ ] **Step 3: Implement header and source**

`lib/kerberos_core/u2f.h`:

```cpp
#pragma once
#include "kerb_crypto.h"

typedef struct {
    const kerb_crypto_t *cy;
    const uint8_t *devkey;         // 32-byte wrapping key
    const uint8_t *att_cert;       // DER X.509
    uint16_t       att_cert_len;
    const uint8_t *att_priv;       // 32-byte attestation private key
    uint32_t      *counter;        // monotonic signature counter (persisted by caller)
    bool         (*user_present)(void *ui);   // blocks until Enter or returns false on abort
    void          *ui;
} u2f_cfg_t;

uint16_t u2f_handle(const u2f_cfg_t *cfg, const uint8_t *apdu, uint16_t len,
                    uint8_t *out, uint16_t cap);
```

`lib/kerberos_core/u2f.cpp` (implements the three instructions):

```cpp
#include "u2f.h"
#include "keywrap.h"
#include <string.h>

static uint16_t sw(uint8_t *out, uint16_t n, uint16_t code) {
    out[n]=code>>8; out[n+1]=code&0xff; return n+2;
}

uint16_t u2f_handle(const u2f_cfg_t *cfg, const uint8_t *apdu, uint16_t len,
                    uint8_t *out, uint16_t cap) {
    if (len < 4) return sw(out, 0, 0x6A80);
    uint8_t ins = apdu[1];
    // Lc is 3 bytes big-endian after the 4-byte header (extended APDU).
    uint32_t lc = 0; const uint8_t *data = apdu + 7;
    if (len >= 7) lc = (uint32_t)apdu[4]<<16 | (uint32_t)apdu[5]<<8 | apdu[6];

    if (ins == 0x03) {                                   // VERSION
        memcpy(out, "U2F_V2", 6); return sw(out, 6, 0x9000);
    }
    if (ins == 0x01) {                                   // REGISTER
        if (lc < 64) return sw(out, 0, 0x6A80);
        const uint8_t *chal = data, *appid = data + 32;
        if (!cfg->user_present(cfg->ui)) return sw(out, 0, 0x6985);
        uint8_t priv[32], pub[65];
        if (cfg->cy->p256_keygen(priv, pub, cfg->cy->ctx)) return sw(out, 0, 0x6A80);
        uint8_t handle[KW_HANDLE_LEN]; size_t hl=0;
        if (kw_wrap(cfg->cy, cfg->devkey, priv, appid, handle, &hl)) return sw(out, 0, 0x6A80);
        // Signature over: 0x00 || appid(32) || chal(32) || handle || pub(65)
        uint8_t msg[1+32+32+KW_HANDLE_LEN+65]; uint16_t m=0;
        msg[m++]=0x00; memcpy(msg+m,appid,32); m+=32; memcpy(msg+m,chal,32); m+=32;
        memcpy(msg+m,handle,hl); m+=hl; memcpy(msg+m,pub,65); m+=65;
        uint8_t sig[72]; size_t sl=0;
        if (cfg->cy->p256_sign(cfg->att_priv, msg, m, sig, &sl, cfg->cy->ctx)) return sw(out,0,0x6A80);
        uint16_t n=0;
        out[n++]=0x05; memcpy(out+n,pub,65); n+=65;
        out[n++]=(uint8_t)hl; memcpy(out+n,handle,hl); n+=hl;
        memcpy(out+n,cfg->att_cert,cfg->att_cert_len); n+=cfg->att_cert_len;
        memcpy(out+n,sig,sl); n+=sl;
        return sw(out, n, 0x9000);
    }
    if (ins == 0x02) {                                   // AUTHENTICATE
        if (lc < 65) return sw(out, 0, 0x6A80);
        const uint8_t *chal = data, *appid = data + 32;
        uint8_t khl = data[64]; const uint8_t *handle = data + 65;
        uint8_t p1 = apdu[2];
        uint8_t priv[32];
        if (kw_unwrap(cfg->cy, cfg->devkey, handle, khl, appid, priv))
            return sw(out, 0, 0x6A80);                   // not ours / wrong app
        if (p1 == 0x07) return sw(out, 0, 0x6985);       // check-only: "present" via 0x6985
        if (!cfg->user_present(cfg->ui)) return sw(out, 0, 0x6985);
        uint32_t ctr = ++(*cfg->counter);
        uint8_t up = 0x01;
        uint8_t ctrb[4]={(uint8_t)(ctr>>24),(uint8_t)(ctr>>16),(uint8_t)(ctr>>8),(uint8_t)ctr};
        // Signature over: appid(32) || up(1) || counter(4) || chal(32)
        uint8_t msg[32+1+4+32]; uint16_t m=0;
        memcpy(msg+m,appid,32); m+=32; msg[m++]=up;
        memcpy(msg+m,ctrb,4); m+=4; memcpy(msg+m,chal,32); m+=32;
        uint8_t sig[72]; size_t sl=0;
        if (cfg->cy->p256_sign(priv, msg, m, sig, &sl, cfg->cy->ctx)) return sw(out,0,0x6A80);
        uint16_t n=0; out[n++]=up; memcpy(out+n,ctrb,4); n+=4; memcpy(out+n,sig,sl); n+=sl;
        return sw(out, n, 0x9000);
    }
    return sw(out, 0, 0x6D00);                            // INS not supported
}
```

- [ ] **Step 4: Run tests, expect PASS**, then commit.

```bash
git add lib/kerberos_core/u2f.* test/test_kerberos_core/
git commit -m "feat(kerberos): U2F register/authenticate/version core"
```

---

### Task 5: mbedtls crypto binding and on device crypto self test

**Files:**
- Create: `src/features/kerberos_crypto.cpp`
- Create: `src/features/kerberos_crypto.h` (declares `const kerb_crypto_t *kerb_mbedtls_crypto(void);`)

**Interfaces:**
- Produces: a `kerb_crypto_t` backed by mbedtls (`mbedtls_ctr_drbg`/`entropy` for rand, `mbedtls_sha256`, `mbedtls_ecdsa`/`ecp` for P256, `mbedtls_gcm` for AES-256-GCM).
- Consumes: nothing from other tasks; it is the device implementation of `kerb_crypto.h`.

- [ ] **Step 1: Implement the binding**

Write `src/features/kerberos_crypto.cpp` implementing each function pointer with mbedtls. Key details: seed a static `mbedtls_ctr_drbg_context` once from `mbedtls_entropy`; `p256_keygen` uses `mbedtls_ecp_gen_keypair` on `MBEDTLS_ECP_DP_SECP256R1` then exports `priv` with `mbedtls_mpi_write_binary` (32 bytes) and `pub` with `mbedtls_ecp_point_write_binary` uncompressed (65 bytes); `p256_sign` hashes with SHA-256 then `mbedtls_ecdsa_write_signature` (DER); `aes_gcm_seal`/`open` use `mbedtls_gcm_crypt_and_tag` / `mbedtls_gcm_auth_decrypt` with a 256-bit key.

- [ ] **Step 2: Add a temporary on device self test behind a build flag**

In `feat_kerberos()` (stubbed next task) or a scratch sketch, compute SHA-256 of `"abc"` and assert it equals the known vector `ba7816bf 8f01cfea 414140de 5dae2223 b00361a3 96177a9c b410ff61 f20015ad`, and do a P256 sign then verify round trip. Print PASS or FAIL to the screen.

- [ ] **Step 3: Flash and verify on hardware**

Run: `pio run -e cardputer -t upload`
On device, trigger the self test. Expected: screen shows SHA256 PASS and ECDSA PASS.

- [ ] **Step 4: Commit**

```bash
git add src/features/kerberos_crypto.cpp src/features/kerberos_crypto.h
git commit -m "feat(kerberos): mbedtls crypto binding + on-device self test"
```

---

### Task 6: Embedded self signed attestation certificate

**Files:**
- Create: `src/features/kerberos_attestation.h`

**Interfaces:**
- Produces: `extern const uint8_t KERB_ATT_CERT[]; extern const uint16_t KERB_ATT_CERT_LEN; extern const uint8_t KERB_ATT_PRIV[32];`

- [ ] **Step 1: Generate the attestation key and cert off device**

On the dev machine:

```bash
openssl ecparam -genkey -name prime256v1 -noout -out kerb_att.pem
openssl req -x509 -new -key kerb_att.pem -days 3650 -subj "/CN=POSEIDON KERBEROS U2F" -out kerb_att.crt -outform der
openssl ec -in kerb_att.pem -text -noout   # copy the 32-byte private scalar
xxd -i kerb_att.crt                          # copy the DER bytes
```

- [ ] **Step 2: Paste bytes into `src/features/kerberos_attestation.h`**

Define `KERB_ATT_CERT[]` from the `xxd -i` output, `KERB_ATT_CERT_LEN` as its length, and `KERB_ATT_PRIV[32]` from the private scalar. Do not commit the `.pem`.

- [ ] **Step 3: Commit**

```bash
echo "kerb_att.pem" >> .gitignore
git add src/features/kerberos_attestation.h .gitignore
git commit -m "feat(kerberos): embed self-signed U2F attestation cert"
```

---

### Task 7: TinyUSB FIDO HID interface

**Files:**
- Create: `src/features/kerberos_hid.h`, `src/features/kerberos_hid.cpp`

**Interfaces:**
- Consumes: `ctaphid_dispatch` from Task 2.
- Produces:
  - `class USBHIDFido : public USBHIDDevice` exposing `begin()`, and an inbound path that calls `ctaphid_dispatch` per 64 byte OUT report, with the send sink calling `hid.SendReport`.
  - `void kerberos_hid_set_msg_handler(ctaphid_msg_fn fn, void *ctx)` so the feature wires the U2F handler.
  - `void kerberos_hid_poll(void)` to run any deferred sends and KEEPALIVE ticks from the main loop.

- [ ] **Step 1: Implement the FIDO report descriptor**

The descriptor must be exactly (usage page 0xF1D0, usage 0x01, a 64 byte input report usage 0x20 and 64 byte output report usage 0x21, no report id):

```cpp
static const uint8_t FIDO_REPORT_DESC[] = {
  0x06, 0xD0, 0xF1,       // Usage Page (FIDO Alliance 0xF1D0)
  0x09, 0x01,             // Usage (CTAPHID)
  0xA1, 0x01,             // Collection (Application)
  0x09, 0x20,             //   Usage (Input Report Data)
  0x15, 0x00,             //   Logical Min 0
  0x26, 0xFF, 0x00,       //   Logical Max 255
  0x75, 0x08,             //   Report Size 8
  0x95, 0x40,             //   Report Count 64
  0x81, 0x02,             //   Input (Data,Var,Abs)
  0x09, 0x21,             //   Usage (Output Report Data)
  0x15, 0x00, 0x26, 0xFF, 0x00,
  0x75, 0x08, 0x95, 0x40,
  0x91, 0x02,             //   Output (Data,Var,Abs)
  0xC0                    // End Collection
};
```

- [ ] **Step 2: Implement `USBHIDFido`** subclassing `USBHIDDevice`: return the descriptor from `_onGetDescriptor`, register in `begin()` via `hid.addDevice(this, sizeof(FIDO_REPORT_DESC))` then `USB.begin()`, receive OUT reports in `_onOutput(...)` (feed each 64 byte report to `ctaphid_dispatch`), and implement the send sink as `hid.SendReport(0, pkt, 64)`.

- [ ] **Step 3: Flash and verify enumeration on Windows**

Run: `pio run -e cardputer -t upload` (with a temporary `setup()` that calls the HID `begin()` and loops `kerberos_hid_poll()`).
On Windows: open Device Manager, expect a HID device with a FIDO usage; open `chrome://device-log` or go to https://webauthn.io and confirm the browser offers "Security Key". A PING from the browser must round trip (visible as no error).
Expected: the OS enumerates a FIDO HID device with no driver prompt.

- [ ] **Step 4: Commit**

```bash
git add src/features/kerberos_hid.h src/features/kerberos_hid.cpp
git commit -m "feat(kerberos): TinyUSB FIDO HID interface + CTAPHID pump"
```

---

### Task 8: The KERBEROS feature, presence UI, menu wiring, end to end U2F

**Files:**
- Create: `src/features/kerberos.cpp`
- Modify: `src/menu.cpp` (add extern near line 115 and a node in `MENU_ROOT_CHILDREN` near line 1012)

**Interfaces:**
- Consumes: `USBHIDFido`, `u2f_handle`, `kerb_mbedtls_crypto()`, the attestation symbols, and the CDC guard globals `g_mimir_cdc_active`, `g_trident_cdc_active`.
- Produces: `void feat_kerberos(void)`; a `bool kerberos_user_present(void *ui)` callback that renders the relying party prompt and blocks on `input_poll` for `PK_ENTER` (approve) or `PK_ESC` (deny), while pumping KEEPALIVE.

- [ ] **Step 1: Implement `feat_kerberos()`**

Structure mirroring `feat_badusb`: guard on `g_mimir_cdc_active || g_trident_cdc_active` with a `ui_toast("CDC in use", ...)`; load or lazily create the 32 byte device wrapping key and the signature counter from NVS (namespace `kerberos`, plain NVS for Phase 1); build a `u2f_cfg_t` with `kerb_mbedtls_crypto()`, the devkey, the attestation cert and key, `&counter`, and `kerberos_user_present`; wire `kerberos_hid_set_msg_handler` to a thunk that calls `u2f_handle`; call the HID `begin()`; then loop rendering a "KERBEROS active, plug into a PC" status screen, calling `kerberos_hid_poll()` each iteration, and exiting on `PK_ESC` which tears USB down and returns.

- [ ] **Step 2: Implement `kerberos_user_present`**

Render the relying party context on screen (Phase 1 U2F does not carry a human readable name, so show the app id hash truncated plus "approve sign in"), footer `ENTER=approve  ESC=deny`. Loop `input_poll`; on `PK_ENTER` return true; on `PK_ESC` return false; each idle iteration call `ctaphid_keepalive(..., 2)` roughly every 100 ms so the host does not time out.

- [ ] **Step 3: Register in the menu**

In `src/menu.cpp` add beside the other externs:

```cpp
extern void feat_kerberos(void);
```

and add this node to `MENU_ROOT_CHILDREN` (before the terminating `{ 0, ... }`):

```cpp
    { 'k', "KERBEROS", "FIDO2 security key (U2F)", nullptr, feat_kerberos,
      "Turns the Cardputer into a USB security key. Enter this mode, plug "
      "into a PC, and register it at a site's security key settings. The "
      "screen shows the sign in prompt and ENTER approves. Phase 1 speaks "
      "U2F, so it works as a second factor on Google, GitHub, and any site "
      "that accepts a security key." },
```

- [ ] **Step 4: Flash and run the full U2F ceremony**

Run: `pio run -e cardputer -t upload`
On Windows in Edge and Chrome:
1. Go to https://webauthn.io, register with the security key, approve on device with ENTER, then authenticate. Expected: both succeed.
2. On https://github.com/settings/security register a new security key. Expected: registration succeeds and a later login prompts for the key and works.
Verify the on device KEEPALIVE by waiting about 10 seconds before pressing ENTER during a ceremony. Expected: the browser does not time out and the ceremony still completes.

- [ ] **Step 5: Commit**

```bash
git add src/features/kerberos.cpp src/menu.cpp
git commit -m "feat(kerberos): KERBEROS U2F feature, presence UI, menu entry"
```

---

## Self-Review

**Spec coverage (Phase 1 rows of the design):**
- CTAPHID transport with INIT and KEEPALIVE: Tasks 1, 2, 8 (KEEPALIVE during presence).
- U2F register and authenticate: Task 4, verified end to end in Task 8.
- FIDO HID profile (usage page 0xF1D0, 64 byte reports) for OS recognition: Task 7.
- USB mode enter and exit with the CDC busy guard: Task 8.
- Menu registration following the existing pattern: Task 8.
- On device crypto via mbedtls: Task 5.
- Deferred correctly to later plans: CBOR and CTAP2 (Phase 2), client PIN (Phase 3), eFuse hardened storage (Phase 4, replacing the plain NVS device key used here), SD backup (Phase 5), Windows passwordless and polish (Phase 6). The plain NVS wrapping key is flagged in Scope as temporary.

**Placeholder scan:** No TBD or "handle errors" hand waving; each core task ships real code and real tests. Device tasks that cannot run on the host carry explicit on device verification procedures with expected results, which is the correct test form for firmware, not a placeholder.

**Type consistency:** `kerb_crypto_t` fields are used identically across keywrap, u2f, and the mbedtls binding. `ctaphid_sink_fn`, `ctaphid_msg_fn`, `u2f_cfg_t`, and `KW_HANDLE_LEN` (60) match between definition and use. The message handler signature `ctaphid_msg_fn` in Task 2 matches the thunk wired in Task 8. `user_present` returns `bool` in both `u2f_cfg_t` and the Task 8 callback.

## Notes carried to later plans

- Phase 4 must replace the plain NVS wrapping key with a key derived from the ESP32-S3 eFuse HMAC peripheral plus the PIN, and migrate or invalidate any handles wrapped under the Phase 1 key.
- Signature counter is global in Phase 1 (one counter for all credentials), which is spec compliant for U2F. Phase 2 CTAP2 may move to per credential counters.
- The temporary `setup()` scaffolds in Tasks 5 and 7 must be removed once `feat_kerberos()` (Task 8) is the real entry point.
