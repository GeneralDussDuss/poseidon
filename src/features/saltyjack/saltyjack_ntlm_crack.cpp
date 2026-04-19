/*
 * SaltyJack — on-device NTLMv2 wordlist cracker.
 *
 * Direct port of @7h30th3r0n3's `crackNTLMv2()` from Evil-Cardputer
 * (Evil-M5Project). https://github.com/7h30th3r0n3/Evil-M5Project
 *
 * Reads hashcat-format NTLMv2 captures from
 *   /poseidon/saltyjack/ntlm_hashes.txt
 * and brute-forces each line against a wordlist at
 *   /poseidon/saltyjack/ntlm_wordlist.txt
 *
 * Algorithm (standard NTLMv2 verify):
 *   nt_hash  = MD4(UTF-16LE(password))
 *   v2_key   = HMAC_MD5(nt_hash,  UTF-16LE(UPPER(user)) || UTF-16LE(domain))
 *   resp     = HMAC_MD5(v2_key,   server_challenge || blob)
 *   match if resp == nt_proof_str
 *
 * MD4 is implemented inline (mbedtls 3.x dropped it). HMAC-MD5 goes
 * through mbedtls. Hits are appended to
 *   /poseidon/saltyjack/ntlm_found.txt
 *
 * Keys:
 *   ESC        — abort
 *   ENTER      — skip current user, move to next hash line
 */
#include "../../app.h"
#include "../../ui.h"
#include "../../input.h"
#include "saltyjack.h"
#include "saltyjack_style.h"
#include <Arduino.h>
#include <SD.h>
#include "mbedtls/md.h"

#define NTLM_HASH_PATH     "/poseidon/saltyjack/ntlm_hashes.txt"
#define NTLM_WORDLIST_PATH "/poseidon/saltyjack/ntlm_wordlist.txt"
#define NTLM_FOUND_PATH    "/poseidon/saltyjack/ntlm_found.txt"

/* ================================================================
 * MD4 — lifted from Evil-Cardputer (crackNTLMv2 section).
 * mbedtls 3.x removed MD4 so we keep a compact self-contained copy.
 * ================================================================ */
typedef struct {
    uint32_t state[4];
    uint32_t count[2];
    uint8_t  buffer[64];
} md4_ctx_t;

#define ROL32(x, n) ((uint32_t)(((uint32_t)(x) << (n)) | ((uint32_t)(x) >> (32 - (n)))))
#define MD4_F(x, y, z) (((x) & (y)) | ((~x) & (z)))
#define MD4_G(x, y, z) (((x) & (y)) | ((x) & (z)) | ((y) & (z)))
#define MD4_H(x, y, z) ((x) ^ (y) ^ (z))

static void md4_encode(uint8_t *out, const uint32_t *in, size_t len)
{
    for (size_t i = 0, j = 0; j < len; i++, j += 4) {
        out[j    ] = (uint8_t)( in[i]        & 0xff);
        out[j + 1] = (uint8_t)((in[i] >>  8) & 0xff);
        out[j + 2] = (uint8_t)((in[i] >> 16) & 0xff);
        out[j + 3] = (uint8_t)((in[i] >> 24) & 0xff);
    }
}

static void md4_transform(uint32_t state[4], const uint8_t block[64])
{
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t X[16];
    for (int i = 0, j = 0; j < 64; i++, j += 4) {
        X[i] =  (uint32_t)block[j]
             | ((uint32_t)block[j + 1] <<  8)
             | ((uint32_t)block[j + 2] << 16)
             | ((uint32_t)block[j + 3] << 24);
    }

    #define R1(a,b,c,d,k,s) a = ROL32(a + MD4_F(b,c,d) + X[k], s)
    #define R2(a,b,c,d,k,s) a = ROL32(a + MD4_G(b,c,d) + X[k] + 0x5a827999u, s)
    #define R3(a,b,c,d,k,s) a = ROL32(a + MD4_H(b,c,d) + X[k] + 0x6ed9eba1u, s)

    R1(a,b,c,d, 0, 3);  R1(d,a,b,c, 1, 7);  R1(c,d,a,b, 2,11);  R1(b,c,d,a, 3,19);
    R1(a,b,c,d, 4, 3);  R1(d,a,b,c, 5, 7);  R1(c,d,a,b, 6,11);  R1(b,c,d,a, 7,19);
    R1(a,b,c,d, 8, 3);  R1(d,a,b,c, 9, 7);  R1(c,d,a,b,10,11);  R1(b,c,d,a,11,19);
    R1(a,b,c,d,12, 3);  R1(d,a,b,c,13, 7);  R1(c,d,a,b,14,11);  R1(b,c,d,a,15,19);

    R2(a,b,c,d, 0, 3);  R2(d,a,b,c, 4, 5);  R2(c,d,a,b, 8, 9);  R2(b,c,d,a,12,13);
    R2(a,b,c,d, 1, 3);  R2(d,a,b,c, 5, 5);  R2(c,d,a,b, 9, 9);  R2(b,c,d,a,13,13);
    R2(a,b,c,d, 2, 3);  R2(d,a,b,c, 6, 5);  R2(c,d,a,b,10, 9);  R2(b,c,d,a,14,13);
    R2(a,b,c,d, 3, 3);  R2(d,a,b,c, 7, 5);  R2(c,d,a,b,11, 9);  R2(b,c,d,a,15,13);

    R3(a,b,c,d, 0, 3);  R3(d,a,b,c, 8, 9);  R3(c,d,a,b, 4,11);  R3(b,c,d,a,12,15);
    R3(a,b,c,d, 2, 3);  R3(d,a,b,c,10, 9);  R3(c,d,a,b, 6,11);  R3(b,c,d,a,14,15);
    R3(a,b,c,d, 1, 3);  R3(d,a,b,c, 9, 9);  R3(c,d,a,b, 5,11);  R3(b,c,d,a,13,15);
    R3(a,b,c,d, 3, 3);  R3(d,a,b,c,11, 9);  R3(c,d,a,b, 7,11);  R3(b,c,d,a,15,15);

    #undef R1
    #undef R2
    #undef R3

    state[0] += a;  state[1] += b;  state[2] += c;  state[3] += d;
}

static void md4_init(md4_ctx_t *ctx)
{
    ctx->count[0] = ctx->count[1] = 0;
    ctx->state[0] = 0x67452301u;
    ctx->state[1] = 0xefcdab89u;
    ctx->state[2] = 0x98badcfeu;
    ctx->state[3] = 0x10325476u;
}

static void md4_update(md4_ctx_t *ctx, const uint8_t *in, size_t len)
{
    size_t idx = (ctx->count[0] >> 3) & 0x3f;
    uint32_t bits = (uint32_t)len << 3;
    if ((ctx->count[0] += bits) < bits) ctx->count[1]++;
    ctx->count[1] += (uint32_t)len >> 29;

    size_t partLen = 64 - idx;
    size_t i = 0;
    if (len >= partLen) {
        memcpy(&ctx->buffer[idx], in, partLen);
        md4_transform(ctx->state, ctx->buffer);
        for (i = partLen; i + 63 < len; i += 64)
            md4_transform(ctx->state, &in[i]);
        idx = 0;
    }
    memcpy(&ctx->buffer[idx], &in[i], len - i);
}

static void md4_final(md4_ctx_t *ctx, uint8_t digest[16])
{
    static const uint8_t PADDING[64] = { 0x80 };
    uint8_t bits[8];
    md4_encode(bits, ctx->count, 8);
    size_t idx    = (ctx->count[0] >> 3) & 0x3f;
    size_t padLen = (idx < 56) ? (56 - idx) : (120 - idx);
    md4_update(ctx, PADDING, padLen);
    md4_update(ctx, bits, 8);
    md4_encode(digest, ctx->state, 16);
}

/* nt_hash = MD4(UTF-16LE(password)). Password is ASCII here; surrogate
 * handling doesn't matter for wordlist cracking in practice. */
static void nt_hash(const char *password, uint8_t out[16])
{
    size_t len = strlen(password);
    /* UTF-16LE buffer on stack; cap at 128-char passwords (256 bytes). */
    uint8_t buf[256];
    if (len > 128) len = 128;
    for (size_t i = 0; i < len; i++) {
        buf[2 * i    ] = (uint8_t)password[i];
        buf[2 * i + 1] = 0x00;
    }
    md4_ctx_t ctx;
    md4_init(&ctx);
    md4_update(&ctx, buf, len * 2);
    md4_final(&ctx, out);
}

/* ===== HMAC-MD5 via mbedtls ===== */
static bool hmac_md5(const uint8_t *key, size_t keylen,
                     const uint8_t *msg, size_t msglen,
                     uint8_t out[16])
{
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_MD5);
    if (!info) return false;
    return mbedtls_md_hmac(info, key, keylen, msg, msglen, out) == 0;
}

/* ===== hex helpers ===== */

static inline uint8_t hex_nib(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c |= 0x20;
    return 10 + (c - 'a');
}

static bool hex_to_bytes(const String &hex, uint8_t *out, size_t out_len)
{
    if (hex.length() < out_len * 2) return false;
    for (size_t i = 0; i < out_len; i++) {
        char a = hex[2 * i];
        char b = hex[2 * i + 1];
        if (!isxdigit((unsigned char)a) || !isxdigit((unsigned char)b)) return false;
        out[i] = (uint8_t)((hex_nib(a) << 4) | hex_nib(b));
    }
    return true;
}

/* ===== wordlist bootstrap ===== */

static void ensure_paths(void)
{
    if (!SD.exists("/poseidon")) SD.mkdir("/poseidon");
    if (!SD.exists("/poseidon/saltyjack")) SD.mkdir("/poseidon/saltyjack");

    if (!SD.exists(NTLM_WORDLIST_PATH)) {
        File wf = SD.open(NTLM_WORDLIST_PATH, FILE_WRITE);
        if (wf) {
            static const char *defaults[] = {
                "admin", "root", "123456", "qwerty", "secret",
                "password", "password1", "Password1", "qwerty123",
                "iloveyou", "654321", "a123456", "letmein", "welcome",
                "Welcome1", "P@ssw0rd", "changeme", "administrator"
            };
            for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++)
                wf.println(defaults[i]);
            wf.close();
        }
    }
}

/* ===== UI draw helpers ===== */

static void draw_frame(const char *phase)
{
    sj_frame("NTLMv2 CRACK");
    auto &d = M5Cardputer.Display;
    d.setTextColor(SJ_FG_DIM, SJ_BG);
    d.setCursor(SJ_CONTENT_X, BODY_Y + 16);
    d.print(phase);
    sj_footer("`=stop  ret=skip");
}

static void draw_user_line(const String &user)
{
    auto &d = M5Cardputer.Display;
    /* Clear the user row + dynamic rows below */
    d.fillRect(0, BODY_Y + 26, SCR_W, BODY_H - 26 - 12, SJ_BG);
    d.setTextColor(SJ_ACCENT, SJ_BG);
    d.setCursor(4, BODY_Y + 26);
    String u = user;
    if (u.length() > 30) u = u.substring(0, 27) + "...";
    d.printf("user: %s", u.c_str());
}

static void draw_progress(uint32_t tried, uint32_t pos, uint32_t total, uint32_t hps)
{
    auto &d = M5Cardputer.Display;
    /* Row 1: tried count */
    d.fillRect(0, BODY_Y + 38, SCR_W, 10, SJ_BG);
    d.setTextColor(SJ_FG, SJ_BG);
    d.setCursor(4, BODY_Y + 38);
    d.printf("tried: ");
    d.setTextColor(SJ_ACCENT, SJ_BG);
    d.printf("%lu", (unsigned long)tried);

    /* Row 2: hashes per second */
    d.fillRect(0, BODY_Y + 48, SCR_W, 10, SJ_BG);
    d.setTextColor(SJ_FG, SJ_BG);
    d.setCursor(4, BODY_Y + 48);
    d.print("speed: ");
    d.setTextColor(SJ_WARN, SJ_BG);
    d.printf("%lu H/s", (unsigned long)hps);

    /* Progress bar */
    int bx = 4, by = BODY_Y + 62, bw = SCR_W - 8, bh = 8;
    d.drawRect(bx, by, bw, bh, SJ_ACCENT_DIM);
    int fill = 0;
    if (total > 0) fill = (int)((uint64_t)(bw - 2) * pos / total);
    if (fill < 0) fill = 0;
    if (fill > bw - 2) fill = bw - 2;
    d.fillRect(bx + 1, by + 1, fill, bh - 2, SJ_GOOD);
    d.fillRect(bx + 1 + fill, by + 1, bw - 2 - fill, bh - 2, SJ_BG);
}

static void draw_result(const String &msg, bool success)
{
    auto &d = M5Cardputer.Display;
    d.fillRect(0, BODY_Y + 76, SCR_W, 30, SJ_BG);
    d.setTextColor(success ? SJ_GOOD : SJ_BAD, SJ_BG);
    d.setCursor(4, BODY_Y + 78);
    String m = msg;
    if (m.length() > 36) m = m.substring(0, 33) + "...";
    d.print(m);
}

/* ===== core: try one hash line against the wordlist ===== */

enum crack_result { CR_FOUND, CR_NOT_FOUND, CR_SKIP, CR_ABORT, CR_INVALID };

static crack_result crack_one(const String &line, String &out_password)
{
    /* Parse hashcat format: user::domain:challenge:ntproof:blob */
    int i1 = line.indexOf("::");
    int i2 = line.indexOf(':', i1 + 2);
    int i3 = line.indexOf(':', i2 + 1);
    int i4 = line.indexOf(':', i3 + 1);
    if (i1 < 0 || i2 < 0 || i3 < 0 || i4 < 0) return CR_INVALID;

    String user      = line.substring(0, i1);
    String domain    = line.substring(i1 + 2, i2);
    String chall_hex = line.substring(i2 + 1, i3);
    String ntp_hex   = line.substring(i3 + 1, i4);
    String blob_hex  = line.substring(i4 + 1);
    blob_hex.trim();

    uint8_t challenge[8], nt_proof[16];
    if (!hex_to_bytes(chall_hex, challenge, 8)) return CR_INVALID;
    if (!hex_to_bytes(ntp_hex,   nt_proof,  16)) return CR_INVALID;

    size_t blob_len = blob_hex.length() / 2;
    if (blob_len == 0) return CR_INVALID;
    uint8_t *blob = (uint8_t *)malloc(blob_len);
    if (!blob) return CR_INVALID;
    if (!hex_to_bytes(blob_hex, blob, blob_len)) { free(blob); return CR_INVALID; }

    /* Pre-build challenge || blob for the resp HMAC. */
    size_t   msg_len = 8 + blob_len;
    uint8_t *msg = (uint8_t *)malloc(msg_len);
    if (!msg) { free(blob); return CR_INVALID; }
    memcpy(msg, challenge, 8);
    memcpy(msg + 8, blob, blob_len);

    /* Pre-build UTF-16LE(UPPER(user)) || UTF-16LE(domain) — the v2 key ident. */
    String upper_user = user;
    upper_user.toUpperCase();
    size_t u_len = upper_user.length() * 2;
    size_t d_len = domain.length()     * 2;
    uint8_t *idbuf = (uint8_t *)malloc(u_len + d_len);
    if (!idbuf) { free(msg); free(blob); return CR_INVALID; }
    for (size_t k = 0; k < upper_user.length(); k++) {
        idbuf[2 * k    ] = (uint8_t)upper_user[k];
        idbuf[2 * k + 1] = 0x00;
    }
    for (size_t k = 0; k < domain.length(); k++) {
        idbuf[u_len + 2 * k    ] = (uint8_t)domain[k];
        idbuf[u_len + 2 * k + 1] = 0x00;
    }

    draw_user_line(user);

    File wf = SD.open(NTLM_WORDLIST_PATH, FILE_READ);
    if (!wf) {
        free(idbuf); free(msg); free(blob);
        return CR_INVALID;
    }
    const uint32_t total_bytes = wf.size();

    bool found = false, abort = false, skip = false;
    uint32_t tried = 0;
    uint32_t last_tried = 0;
    uint32_t last_t     = millis();
    uint32_t hps        = 0;

    draw_progress(0, 0, total_bytes, 0);

    while (wf.available() && !found) {
        /* Poll input every iteration — cheap vs. HMAC. */
        uint16_t k = input_poll();
        if (k == PK_ESC)   { abort = true; break; }
        if (k == PK_ENTER) { skip  = true; break; }

        String pwd = wf.readStringUntil('\n');
        pwd.trim();
        if (pwd.length() == 0) continue;
        if (pwd[0] == '#' || pwd[0] == ';') continue;

        uint8_t nthash[16];
        nt_hash(pwd.c_str(), nthash);

        uint8_t v2key[16];
        if (!hmac_md5(nthash, 16, idbuf, u_len + d_len, v2key)) continue;

        uint8_t resp[16];
        if (!hmac_md5(v2key, 16, msg, msg_len, resp)) continue;

        tried++;

        if (memcmp(resp, nt_proof, 16) == 0) {
            out_password = pwd;
            found = true;
            break;
        }

        if ((tried & 0x1ff) == 0) {
            uint32_t now = millis();
            uint32_t elapsed = now - last_t;
            if (elapsed > 0) {
                hps = ((tried - last_tried) * 1000UL) / elapsed;
                last_t = now; last_tried = tried;
            }
            draw_progress(tried, (uint32_t)wf.position(), total_bytes, hps);
            delay(0);  /* yield to watchdog / WiFi stack */
        }
    }

    wf.close();
    free(idbuf); free(msg); free(blob);

    if (found) return CR_FOUND;
    if (abort) return CR_ABORT;
    if (skip)  return CR_SKIP;
    return CR_NOT_FOUND;
}

/* ===== entry point ===== */

void feat_saltyjack_ntlm_crack(void)
{
    draw_frame("init...");

    ensure_paths();

    File hf = SD.open(NTLM_HASH_PATH, FILE_READ);
    if (!hf) {
        draw_frame("no hashes file");
        draw_result("open " NTLM_HASH_PATH " FAIL", false);
        while (true) { if (input_poll() == PK_ESC) return; delay(30); }
    }
    if (hf.size() == 0) {
        hf.close();
        draw_frame("hashes file empty");
        draw_result("capture some first", false);
        while (true) { if (input_poll() == PK_ESC) return; delay(30); }
    }

    uint32_t total_lines = 0, cracked = 0;
    bool abort = false;

    while (hf.available() && !abort) {
        String line = hf.readStringUntil('\n');
        line.trim();
        if (line.length() < 10 || line.startsWith("-") || line.startsWith("#")) continue;
        total_lines++;

        draw_frame("cracking...");

        String pwd;
        crack_result r = crack_one(line, pwd);

        /* Extract user for display (before ::). */
        int i1 = line.indexOf("::");
        String user = (i1 > 0) ? line.substring(0, i1) : String("?");
        String domain;
        {
            int i2 = line.indexOf(':', i1 + 2);
            if (i1 > 0 && i2 > i1)
                domain = line.substring(i1 + 2, i2);
        }

        switch (r) {
        case CR_FOUND: {
            cracked++;
            String m = user + ":" + pwd;
            draw_result(m, true);
            File f = SD.open(NTLM_FOUND_PATH, FILE_APPEND);
            if (f) {
                f.print(user);
                f.print("::");
                f.print(domain);
                f.print(": ");
                f.println(pwd);
                f.close();
            }
            delay(1500);
            break;
        }
        case CR_NOT_FOUND:
            draw_result("no match: " + user, false);
            delay(900);
            break;
        case CR_SKIP:
            draw_result("skipped: " + user, false);
            delay(500);
            break;
        case CR_INVALID:
            draw_result("bad line fmt", false);
            delay(500);
            break;
        case CR_ABORT:
            abort = true;
            break;
        }
    }
    hf.close();

    /* Summary screen. */
    draw_frame(abort ? "aborted." : "done.");
    auto &d = M5Cardputer.Display;
    d.setTextColor(SJ_FG, SJ_BG);
    d.setCursor(4, BODY_Y + 30);
    d.printf("hashes tried: %lu", (unsigned long)total_lines);
    d.setTextColor(SJ_GOOD, SJ_BG);
    d.setCursor(4, BODY_Y + 44);
    d.printf("cracked:      %lu", (unsigned long)cracked);
    d.setTextColor(SJ_FG_DIM, SJ_BG);
    d.setCursor(4, BODY_Y + 62);
    d.print("hits: ntlm_found.txt");

    while (true) {
        uint16_t k = input_poll();
        if (k == PK_ESC) break;
        delay(30);
    }
}
