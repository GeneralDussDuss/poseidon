#include "ctap2.h"
#include "cbor_util.h"
#include "cose.h"
#include "authdata.h"
#include "keywrap.h"
#include "cred_store.h"
#include <string.h>

// CTAP2 status codes used here.
enum {
    CTAP2_OK                    = 0x00,
    CTAP1_ERR_INVALID_COMMAND   = 0x01,
    CTAP1_ERR_INVALID_LENGTH    = 0x03,
    CTAP2_ERR_INVALID_CBOR      = 0x12,
    CTAP2_ERR_MISSING_PARAMETER = 0x14,
    CTAP2_ERR_UNSUPPORTED_ALGORITHM = 0x26,
    CTAP2_ERR_OPERATION_DENIED  = 0x27,
    CTAP2_ERR_KEY_STORE_FULL    = 0x28,
    CTAP2_ERR_UNSUPPORTED_OPTION = 0x2B,
    CTAP2_ERR_INVALID_OPTION    = 0x2C,
    CTAP2_ERR_NO_CREDENTIALS    = 0x2E,
    CTAP2_ERR_NOT_ALLOWED       = 0x30,
    CTAP2_ERR_PIN_INVALID       = 0x31,
    CTAP2_ERR_PIN_BLOCKED       = 0x32,
    CTAP2_ERR_PIN_AUTH_INVALID  = 0x33,
    CTAP2_ERR_PIN_AUTH_BLOCKED  = 0x34,
    CTAP2_ERR_PIN_NOT_SET       = 0x35,
    CTAP2_ERR_PUAT_REQUIRED     = 0x36,   // "PIN required"
    CTAP2_ERR_PIN_POLICY_VIOLATION = 0x37,
    CTAP1_ERR_OTHER             = 0x7F,
};

#define PIN_MAX_RETRIES 8

static uint16_t err(uint8_t *out, uint8_t code) { out[0] = code; return 1; }

// pinUvAuthProtocol-1 token verify (defined with the clientPIN block below).
static bool pin_auth_ok(const ctap2_cfg_t *cfg, const uint8_t *key, size_t keylen,
                        const uint8_t *msg, size_t msglen, const uint8_t *mac, size_t maclen);
static bool pin_is_set(const ctap2_cfg_t *cfg);

// Read a text field by string key from a sub-map (rp, user).
static int submap_text(CborValue *submap, const char *key, char *dst, size_t *len) {
    CborValue v;
    if (cbor_value_map_find_value(submap, key, &v) != CborNoError) return -1;
    if (!cbor_value_is_text_string(&v)) return -1;
    return cbor_value_copy_text_string(&v, dst, len, nullptr) == CborNoError ? 0 : -1;
}
static int submap_bytes(CborValue *submap, const char *key, uint8_t *dst, size_t *len) {
    CborValue v;
    if (cbor_value_map_find_value(submap, key, &v) != CborNoError) return -1;
    if (!cbor_value_is_byte_string(&v)) return -1;
    return cbor_value_copy_byte_string(&v, dst, len, nullptr) == CborNoError ? 0 : -1;
}

// True if the pubKeyCredParams array contains an ES256 (alg -7) entry.
static bool has_es256(CborValue *array) {
    if (!cbor_value_is_array(array)) return false;
    CborValue it; cbor_value_enter_container(array, &it);
    while (!cbor_value_at_end(&it)) {
        if (cbor_value_is_map(&it)) {
            CborValue algv;
            if (cbor_value_map_find_value(&it, "alg", &algv) == CborNoError &&
                cbor_value_is_integer(&algv)) {
                int alg = 0; cbor_value_get_int_checked(&algv, &alg);
                if (alg == -7) return true;
            }
        }
        cbor_value_advance(&it);
    }
    return false;
}

// Read options.<name> as a boolean from the top-level request map (options is
// key 7 for makeCredential, key 5 for getAssertion).
static bool opt_true(CborValue *map, int optionsKey, const char *name) {
    CborValue opts, v;
    if (cbor_map_enter(map, optionsKey, &opts)) return false;
    if (cbor_value_map_find_value(&opts, name, &v) != CborNoError) return false;
    if (!cbor_value_is_boolean(&v)) return false;
    bool b = false; cbor_value_get_boolean(&v, &b); return b;
}

// Find a resident record for this rp whose id matches. Returns 0 on hit.
static int store_find_by_id(const ctap2_cfg_t *cfg, const uint8_t rp[32],
                            const uint8_t *id, size_t idl, cred_record *out) {
    if (idl != 32 || !cfg->store) return -1;
    int total = 0;
    for (int i = 0; ; i++) {
        cred_record r;
        if (cfg->store->find_by_rp(cfg->store, rp, &r, i, &total)) break;
        if (memcmp(r.id, id, 32) == 0) { *out = r; return 0; }
        if (i + 1 >= total) break;
    }
    return -1;
}

// True if a PIN is currently set (pin support present and load reports one).
static bool pin_is_set(const ctap2_cfg_t *cfg) {
    if (!cfg->pin) return false;
    uint8_t h[16], r; return cfg->pin->load(cfg->pin, h, &r) == 1;
}

static uint16_t get_info(const ctap2_cfg_t *cfg, uint8_t *out, uint16_t cap) {
    bool pinSupport = (cfg->pin && cfg->pin_rt);
    bool pinSet = pinSupport && pin_is_set(cfg);

    out[0] = CTAP2_OK;
    cbor_writer w; cw_init(&w, out + 1, cap - 1);
    cw_map(&w, pinSupport ? 4 : 3);                   // keys 1,3,4[,6] (ascending)
    // 1: versions
    cw_key(&w, 1);
    CborEncoder arr; cbor_encoder_create_array(cw_enc(&w), &arr, pinSupport ? 3 : 2);
    cbor_encode_text_stringz(&arr, "U2F_V2");
    cbor_encode_text_stringz(&arr, "FIDO_2_0");
    if (pinSupport) cbor_encode_text_stringz(&arr, "FIDO_2_1");
    cbor_encoder_close_container(cw_enc(&w), &arr);
    // 3: aaguid
    cw_key(&w, 3); cw_bytes(&w, cfg->aaguid, 16);
    // 4: options — canonical order (major/len/byte): rk, up, clientPin,
    //    pinUvAuthToken, makeCredUvNotRqd. CTAP 2.1 §9 requires that a key
    //    advertising rk:true also advertise clientPin/uv, which is exactly the
    //    conformance gap that blocked Windows passkey enrollment.
    cw_key(&w, 4);
    CborEncoder opt; cbor_encoder_create_map(cw_enc(&w), &opt, pinSupport ? 5 : 2);
    cbor_encode_text_stringz(&opt, "rk"); cbor_encode_boolean(&opt, true);
    cbor_encode_text_stringz(&opt, "up"); cbor_encode_boolean(&opt, true);
    if (pinSupport) {
        cbor_encode_text_stringz(&opt, "clientPin");        cbor_encode_boolean(&opt, pinSet);
        cbor_encode_text_stringz(&opt, "pinUvAuthToken");    cbor_encode_boolean(&opt, true);
        cbor_encode_text_stringz(&opt, "makeCredUvNotRqd");  cbor_encode_boolean(&opt, true);
    }
    cbor_encoder_close_container(cw_enc(&w), &opt);
    // 6: pinUvAuthProtocols [1]
    if (pinSupport) {
        cw_key(&w, 6);
        CborEncoder pr; cbor_encoder_create_array(cw_enc(&w), &pr, 1);
        cbor_encode_int(&pr, 1);
        cbor_encoder_close_container(cw_enc(&w), &pr);
    }
    size_t n = cw_finish(&w);
    return (uint16_t)(1 + n);
}

static uint16_t make_cred(const ctap2_cfg_t *cfg, const uint8_t *req, uint16_t len,
                          uint8_t *out, uint16_t cap) {
    CborParser p; CborValue map;
    if (cbor_get_map(req + 1, len - 1, &p, &map)) return err(out, CTAP2_ERR_INVALID_CBOR);

    // 1: clientDataHash (32)
    uint8_t cdh[32]; size_t cdhl = sizeof cdh;
    if (cbor_map_bytes(&map, 1, cdh, &cdhl) || cdhl != 32) return err(out, CTAP2_ERR_MISSING_PARAMETER);
    // 2: rp {id}
    CborValue rp; if (cbor_map_enter(&map, 2, &rp)) return err(out, CTAP2_ERR_MISSING_PARAMETER);
    char rpid[128]; size_t rpidl = sizeof rpid;
    if (submap_text(&rp, "id", rpid, &rpidl)) return err(out, CTAP2_ERR_MISSING_PARAMETER);
    uint8_t rpIdHash[32];
    if (cfg->cy->sha256((const uint8_t *)rpid, strlen(rpid), rpIdHash, cfg->cy->ctx))
        return err(out, CTAP1_ERR_OTHER);
    // 3: user (present but its id is only needed for resident creds, added later)
    CborValue user; if (cbor_map_enter(&map, 3, &user)) return err(out, CTAP2_ERR_MISSING_PARAMETER);
    // 4: pubKeyCredParams must offer ES256
    CborValue pk; if (cbor_map_enter(&map, 4, &pk)) return err(out, CTAP2_ERR_MISSING_PARAMETER);
    if (!has_es256(&pk)) return err(out, CTAP2_ERR_UNSUPPORTED_ALGORITHM);

    bool rk = opt_true(&map, 7, "rk");
    bool uvOpt = opt_true(&map, 7, "uv");

    // ---- PIN/UV (CTAP 2.1 §6.1) ----
    // A passkey is a discoverable credential requiring user verification. If a
    // PIN is set, the platform must present a pinUvAuthParam over the client
    // data hash; verifying it sets the UV flag. This is the half of the flow
    // Windows drives after setPIN — without it, enrollment silently produces a
    // UV-less credential that Windows rejects.
    uint8_t uvFlag = 0;
    if (cfg->pin && cfg->pin_rt && pin_is_set(cfg)) {
        uint8_t pap[16]; size_t papl = sizeof pap;
        bool havePap = (cbor_map_bytes(&map, 8, pap, &papl) == 0);
        if (havePap) {
            uint64_t proto = 0;
            if (cbor_map_uint(&map, 9, &proto) || proto != 1) return err(out, CTAP2_ERR_PIN_AUTH_INVALID);
            ctap2_pin_rt *rt = cfg->pin_rt;
            if (!rt->token_set || !(rt->token_perms & PIN_PERM_MC)) return err(out, CTAP2_ERR_PIN_AUTH_INVALID);
            if (rt->token_rp_set && memcmp(rt->token_rp, rpIdHash, 32) != 0) return err(out, CTAP2_ERR_PIN_AUTH_INVALID);
            if (!pin_auth_ok(cfg, rt->token, 32, cdh, 32, pap, papl)) return err(out, CTAP2_ERR_PIN_AUTH_INVALID);
            uvFlag = AD_FLAG_UV;
        } else if (rk || uvOpt) {
            return err(out, CTAP2_ERR_PUAT_REQUIRED);   // 0x36 "PIN required"
        }
    }

    if (!cfg->user_present(cfg->ui)) return err(out, CTAP2_ERR_OPERATION_DENIED);

    uint8_t priv[32], pub[65];
    if (cfg->cy->p256_keygen(priv, pub, cfg->cy->ctx)) return err(out, CTAP1_ERR_OTHER);
    uint8_t credId[64]; size_t credIdLen = 0;
    if (rk && cfg->store) {
        // Resident: random 32-byte credential id; the wrapped key lives in a record.
        if (cfg->cy->rand(credId, 32, cfg->cy->ctx)) return err(out, CTAP1_ERR_OTHER);
        credIdLen = 32;
        cred_record rec; memset(&rec, 0, sizeof rec);
        memcpy(rec.id, credId, 32);
        memcpy(rec.rpIdHash, rpIdHash, 32);
        size_t uidl = sizeof rec.userId;
        if (submap_bytes(&user, "id", rec.userId, &uidl) == 0) rec.userIdLen = (uint8_t)uidl;
        size_t nl = sizeof rec.name;
        submap_text(&user, "name", rec.name, &nl);   // optional
        size_t wl = 0;
        if (kw_wrap(cfg->cy, cfg->devkey, priv, rpIdHash, rec.wrappedKey, &wl)) return err(out, CTAP1_ERR_OTHER);
        rec.signCount = 0;
        if (cfg->store->add(cfg->store, &rec)) return err(out, CTAP2_ERR_KEY_STORE_FULL);
    } else {
        // Non-resident: credential id IS the wrapped private key (bound to rpIdHash).
        if (kw_wrap(cfg->cy, cfg->devkey, priv, rpIdHash, credId, &credIdLen)) return err(out, CTAP1_ERR_OTHER);
    }

    uint8_t cose[128]; size_t coseLen = cose_es256_from_pubkey(pub, cose, sizeof cose);
    uint8_t acd[256]; size_t acdLen = att_cred_data(cfg->aaguid, credId, (uint16_t)credIdLen,
                                                    cose, coseLen, acd, sizeof acd);
    uint8_t authData[320]; size_t adLen = authdata_build(rpIdHash, AD_FLAG_UP | AD_FLAG_AT | uvFlag, 0,
                                                         acd, acdLen, authData, sizeof authData);
    // Packed self attestation: sign authData || clientDataHash with the credential key.
    uint8_t tosign[320 + 32]; memcpy(tosign, authData, adLen); memcpy(tosign + adLen, cdh, 32);
    uint8_t sig[72]; size_t sigLen = 0;
    if (cfg->cy->p256_sign(priv, tosign, adLen + 32, sig, &sigLen, cfg->cy->ctx))
        return err(out, CTAP1_ERR_OTHER);

    out[0] = CTAP2_OK;
    cbor_writer w; cw_init(&w, out + 1, cap - 1);
    cw_map(&w, 3);                                    // 1: fmt, 2: authData, 3: attStmt
    cw_key(&w, 1); cw_text(&w, "packed");
    cw_key(&w, 2); cw_bytes(&w, authData, adLen);
    cw_key(&w, 3);
    CborEncoder att; cbor_encoder_create_map(cw_enc(&w), &att, 2);
    cbor_encode_text_stringz(&att, "alg"); cbor_encode_int(&att, -7);
    cbor_encode_text_stringz(&att, "sig"); cbor_encode_byte_string(&att, sig, sigLen);
    cbor_encoder_close_container(cw_enc(&w), &att);
    size_t n = cw_finish(&w);
    return (uint16_t)(1 + n);
}

static uint16_t get_assert(const ctap2_cfg_t *cfg, const uint8_t *req, uint16_t len,
                           uint8_t *out, uint16_t cap) {
    CborParser p; CborValue map;
    if (cbor_get_map(req + 1, len - 1, &p, &map)) return err(out, CTAP2_ERR_INVALID_CBOR);

    // 1: rpId (text) -> hash
    char rpid[128]; size_t rpidl = sizeof rpid;
    if (cbor_map_text(&map, 1, rpid, &rpidl)) return err(out, CTAP2_ERR_MISSING_PARAMETER);
    uint8_t rpIdHash[32];
    if (cfg->cy->sha256((const uint8_t *)rpid, strlen(rpid), rpIdHash, cfg->cy->ctx))
        return err(out, CTAP1_ERR_OTHER);
    // 2: clientDataHash
    uint8_t cdh[32]; size_t cdhl = sizeof cdh;
    if (cbor_map_bytes(&map, 2, cdh, &cdhl) || cdhl != 32) return err(out, CTAP2_ERR_MISSING_PARAMETER);

    // ---- PIN/UV: verify a presented token (getAssertion keys 6/7) and set UV ----
    uint8_t uvFlag = 0;
    if (cfg->pin && cfg->pin_rt && pin_is_set(cfg)) {
        uint8_t pap[16]; size_t papl = sizeof pap;
        if (cbor_map_bytes(&map, 6, pap, &papl) == 0) {
            uint64_t proto = 0;
            if (cbor_map_uint(&map, 7, &proto) || proto != 1) return err(out, CTAP2_ERR_PIN_AUTH_INVALID);
            ctap2_pin_rt *rt = cfg->pin_rt;
            if (!rt->token_set || !(rt->token_perms & PIN_PERM_GA)) return err(out, CTAP2_ERR_PIN_AUTH_INVALID);
            if (rt->token_rp_set && memcmp(rt->token_rp, rpIdHash, 32) != 0) return err(out, CTAP2_ERR_PIN_AUTH_INVALID);
            if (!pin_auth_ok(cfg, rt->token, 32, cdh, 32, pap, papl)) return err(out, CTAP2_ERR_PIN_AUTH_INVALID);
            uvFlag = AD_FLAG_UV;
        }
    }

    // Resolve the credential: allowList (non-resident unwrap, or resident by id),
    // else discoverable lookup by rp when the allowList is empty.
    uint8_t priv[32], credId[64]; size_t credIdLen = 0; bool found = false;
    bool from_store = false; cred_record found_rec;
    CborValue al;
    bool hasAllow = (cbor_map_enter(&map, 3, &al) == 0 && cbor_value_is_array(&al));
    int allowCount = 0;
    if (hasAllow) {
        CborValue it; cbor_value_enter_container(&al, &it);
        while (!cbor_value_at_end(&it)) {
            if (cbor_value_is_map(&it)) {
                allowCount++;
                CborValue idv;
                if (cbor_value_map_find_value(&it, "id", &idv) == CborNoError &&
                    cbor_value_is_byte_string(&idv)) {
                    uint8_t cid[128]; size_t cl = sizeof cid;
                    if (cbor_value_copy_byte_string(&idv, cid, &cl, nullptr) == CborNoError) {
                        if (kw_unwrap(cfg->cy, cfg->devkey, cid, cl, rpIdHash, priv) == 0) {
                            memcpy(credId, cid, cl); credIdLen = cl; found = true; break;
                        }
                        if (store_find_by_id(cfg, rpIdHash, cid, cl, &found_rec) == 0 &&
                            kw_unwrap(cfg->cy, cfg->devkey, found_rec.wrappedKey, KW_HANDLE_LEN,
                                      rpIdHash, priv) == 0) {
                            memcpy(credId, cid, cl); credIdLen = cl; found = true; from_store = true; break;
                        }
                    }
                }
            }
            cbor_value_advance(&it);
        }
    }
    if (!found && allowCount == 0 && cfg->store) {
        int total = 0; cred_record r;
        if (cfg->store->find_by_rp(cfg->store, rpIdHash, &r, 0, &total) == 0 &&
            kw_unwrap(cfg->cy, cfg->devkey, r.wrappedKey, KW_HANDLE_LEN, rpIdHash, priv) == 0) {
            memcpy(credId, r.id, 32); credIdLen = 32; found = true; from_store = true; found_rec = r;
        }
    }
    if (!found) return err(out, CTAP2_ERR_NO_CREDENTIALS);

    if (!cfg->user_present(cfg->ui)) return err(out, CTAP2_ERR_OPERATION_DENIED);
    uint32_t ctr;
    if (from_store) {
        ctr = found_rec.signCount + 1;
        if (cfg->store) cfg->store->update_counter(cfg->store, credId, ctr);
    } else {
        ctr = cfg->counter ? ++(*cfg->counter) : 1;
    }

    uint8_t authData[37];
    size_t adLen = authdata_build(rpIdHash, AD_FLAG_UP | uvFlag, ctr, nullptr, 0, authData, sizeof authData);
    uint8_t tosign[37 + 32]; memcpy(tosign, authData, adLen); memcpy(tosign + adLen, cdh, 32);
    uint8_t sig[72]; size_t sigLen = 0;
    if (cfg->cy->p256_sign(priv, tosign, adLen + 32, sig, &sigLen, cfg->cy->ctx))
        return err(out, CTAP1_ERR_OTHER);

    out[0] = CTAP2_OK;
    cbor_writer w; cw_init(&w, out + 1, cap - 1);
    cw_map(&w, 3);                                    // 1: credential, 2: authData, 3: signature
    cw_key(&w, 1);
    CborEncoder cred; cbor_encoder_create_map(cw_enc(&w), &cred, 2);
    cbor_encode_text_stringz(&cred, "id");   cbor_encode_byte_string(&cred, credId, credIdLen);
    cbor_encode_text_stringz(&cred, "type"); cbor_encode_text_stringz(&cred, "public-key");
    cbor_encoder_close_container(cw_enc(&w), &cred);
    cw_key(&w, 2); cw_bytes(&w, authData, adLen);
    cw_key(&w, 3); cw_bytes(&w, sig, sigLen);
    size_t n = cw_finish(&w);
    return (uint16_t)(1 + n);
}

// ---- CTAP2 clientPIN (pinUvAuthProtocol 1) ----

// Lazily create the authenticator's per-boot key-agreement keypair.
static int ensure_ka(const ctap2_cfg_t *cfg) {
    ctap2_pin_rt *rt = cfg->pin_rt;
    if (rt->ka_ready) return 0;
    if (cfg->cy->p256_keygen(rt->ka_priv, rt->ka_pub, cfg->cy->ctx)) return -1;
    rt->ka_ready = true;
    return 0;
}

// Parse platform keyAgreement (request key 3, a COSE_Key) and derive
// sharedSecret = SHA-256(ECDH(ka_priv, platformPub)). Returns 0 or a CTAP2 error.
static uint8_t pin_shared(const ctap2_cfg_t *cfg, const CborValue *map, uint8_t ss[32]) {
    if (ensure_ka(cfg)) return CTAP1_ERR_OTHER;
    CborValue ka;
    if (cbor_map_enter(map, 3, &ka)) return CTAP2_ERR_MISSING_PARAMETER;
    uint8_t x[32], y[32]; size_t xl = 32, yl = 32;
    if (cbor_map_bytes(&ka, -2, x, &xl) || xl != 32) return CTAP2_ERR_MISSING_PARAMETER;
    if (cbor_map_bytes(&ka, -3, y, &yl) || yl != 32) return CTAP2_ERR_MISSING_PARAMETER;
    uint8_t pub[65]; pub[0] = 0x04; memcpy(pub + 1, x, 32); memcpy(pub + 33, y, 32);
    uint8_t sx[32];
    if (cfg->cy->ecdh_p256(cfg->pin_rt->ka_priv, pub, sx, cfg->cy->ctx))
        return CTAP2_ERR_PIN_AUTH_INVALID;
    if (cfg->cy->sha256(sx, 32, ss, cfg->cy->ctx)) return CTAP1_ERR_OTHER;
    return 0;
}

// pinUvAuthProtocol-1 verify: first 16 bytes of HMAC(key, msg) == mac[16].
static bool pin_auth_ok(const ctap2_cfg_t *cfg, const uint8_t *key, size_t keylen,
                        const uint8_t *msg, size_t msglen, const uint8_t *mac, size_t maclen) {
    if (maclen != 16) return false;
    uint8_t full[32];
    if (cfg->cy->hmac_sha256(key, keylen, msg, msglen, full, cfg->cy->ctx)) return false;
    uint8_t d = 0; for (int i = 0; i < 16; i++) d |= (uint8_t)(full[i] ^ mac[i]);
    return d == 0;
}

// Decrypt pinHashEnc, compare to the stored pinHash, and update retry counter +
// per-boot lockout. Returns 0 on match, or a CTAP2 error on mismatch/lockout.
static uint8_t pin_check(const ctap2_cfg_t *cfg, const uint8_t *ss,
                         const uint8_t pinHashEnc[16], const uint8_t stored[16],
                         uint8_t *retries) {
    ctap2_pin_rt *rt = cfg->pin_rt;
    if (*retries == 0) return CTAP2_ERR_PIN_BLOCKED;
    uint8_t iv[16] = {0}; uint8_t got[16];
    if (cfg->cy->aes_cbc(ss, iv, 0, pinHashEnc, 16, got, cfg->cy->ctx)) return CTAP1_ERR_OTHER;
    // Spec: decrement the persisted retry counter before comparing; restore on success.
    (*retries)--; cfg->pin->save_retries(cfg->pin, *retries);
    uint8_t d = 0; for (int i = 0; i < 16; i++) d |= (uint8_t)(got[i] ^ stored[i]);
    if (d != 0) {
        rt->boot_fails++;
        rt->ka_ready = false;   // invalidate shared secret; platform must re-agree
        if (*retries == 0) return CTAP2_ERR_PIN_BLOCKED;
        if (rt->boot_fails >= 3) return CTAP2_ERR_PIN_AUTH_BLOCKED;
        return CTAP2_ERR_PIN_INVALID;
    }
    *retries = PIN_MAX_RETRIES; cfg->pin->save_retries(cfg->pin, PIN_MAX_RETRIES);
    rt->boot_fails = 0;
    return 0;
}

// Write the getKeyAgreement COSE_Key (EC2 / P-256 / ECDH) as response key 1.
static void write_cose_ka(cbor_writer *w, const uint8_t pub[65]) {
    CborEncoder m; cbor_encoder_create_map(cw_enc(w), &m, 5);
    cbor_encode_int(&m, 1);  cbor_encode_int(&m, 2);    // kty: EC2
    cbor_encode_int(&m, 3);  cbor_encode_int(&m, -25);  // alg: ECDH-ES+HKDF-256
    cbor_encode_int(&m, -1); cbor_encode_int(&m, 1);    // crv: P-256
    cbor_encode_int(&m, -2); cbor_encode_byte_string(&m, pub + 1, 32);   // X
    cbor_encode_int(&m, -3); cbor_encode_byte_string(&m, pub + 33, 32);  // Y
    cbor_encoder_close_container(cw_enc(w), &m);
}

// Decrypt newPinEnc (64B, zero-padded PIN) with the shared secret, validate the
// length policy, and store LEFT-16 of SHA-256(pin). Returns 0 or a CTAP2 error.
static uint8_t pin_set_from_enc(const ctap2_cfg_t *cfg, const uint8_t *ss,
                                const uint8_t newPinEnc[64]) {
    uint8_t iv[16] = {0}; uint8_t pin[64];
    if (cfg->cy->aes_cbc(ss, iv, 0, newPinEnc, 64, pin, cfg->cy->ctx)) return CTAP1_ERR_OTHER;
    size_t plen = 0; while (plen < 64 && pin[plen] != 0) plen++;
    if (plen < 4) return CTAP2_ERR_PIN_POLICY_VIOLATION;
    uint8_t h[32];
    if (cfg->cy->sha256(pin, plen, h, cfg->cy->ctx)) return CTAP1_ERR_OTHER;
    if (cfg->pin->save(cfg->pin, h, PIN_MAX_RETRIES)) return CTAP1_ERR_OTHER;
    return 0;
}

static uint16_t client_pin(const ctap2_cfg_t *cfg, const uint8_t *req, uint16_t len,
                           uint8_t *out, uint16_t cap) {
    if (!cfg->pin || !cfg->pin_rt) { out[0] = CTAP1_ERR_INVALID_COMMAND; return 1; }
    CborParser p; CborValue map;
    if (cbor_get_map(req + 1, len - 1, &p, &map)) return err(out, CTAP2_ERR_INVALID_CBOR);

    uint64_t proto = 0, sub = 0;
    if (cbor_map_uint(&map, 1, &proto) || proto != 1) return err(out, CTAP2_ERR_PIN_AUTH_INVALID);
    if (cbor_map_uint(&map, 2, &sub))                 return err(out, CTAP2_ERR_MISSING_PARAMETER);

    ctap2_pin_rt *rt = cfg->pin_rt;
    uint8_t stored[16], retries = PIN_MAX_RETRIES;
    bool haveStored = (cfg->pin->load(cfg->pin, stored, &retries) == 1);

    switch (sub) {
    case 1: {  // getPINRetries -> {3: retries}
        out[0] = CTAP2_OK; cbor_writer w; cw_init(&w, out + 1, cap - 1);
        cw_map(&w, 1); cw_key(&w, 3); cw_uint(&w, haveStored ? retries : PIN_MAX_RETRIES);
        return (uint16_t)(1 + cw_finish(&w));
    }
    case 2: {  // getKeyAgreement -> {1: COSE_Key}
        if (ensure_ka(cfg)) return err(out, CTAP1_ERR_OTHER);
        out[0] = CTAP2_OK; cbor_writer w; cw_init(&w, out + 1, cap - 1);
        cw_map(&w, 1); cw_key(&w, 1); write_cose_ka(&w, rt->ka_pub);
        return (uint16_t)(1 + cw_finish(&w));
    }
    case 3: {  // setPIN
        if (haveStored) return err(out, CTAP2_ERR_NOT_ALLOWED);
        uint8_t ss[32]; uint8_t e = pin_shared(cfg, &map, ss); if (e) return err(out, e);
        uint8_t newPinEnc[80]; size_t npl = sizeof newPinEnc;
        if (cbor_map_bytes(&map, 5, newPinEnc, &npl) || npl != 64) return err(out, CTAP2_ERR_PIN_POLICY_VIOLATION);
        uint8_t mac[16]; size_t macl = sizeof mac;
        if (cbor_map_bytes(&map, 4, mac, &macl))                    return err(out, CTAP2_ERR_MISSING_PARAMETER);
        if (!pin_auth_ok(cfg, ss, 32, newPinEnc, 64, mac, macl))    return err(out, CTAP2_ERR_PIN_AUTH_INVALID);
        uint8_t e2 = pin_set_from_enc(cfg, ss, newPinEnc); if (e2) return err(out, e2);
        rt->boot_fails = 0;
        out[0] = CTAP2_OK; return 1;
    }
    case 4: {  // changePIN
        if (!haveStored) return err(out, CTAP2_ERR_PIN_NOT_SET);
        if (retries == 0) return err(out, CTAP2_ERR_PIN_BLOCKED);
        if (rt->boot_fails >= 3) return err(out, CTAP2_ERR_PIN_AUTH_BLOCKED);
        uint8_t ss[32]; uint8_t e = pin_shared(cfg, &map, ss); if (e) return err(out, e);
        uint8_t newPinEnc[80]; size_t npl = sizeof newPinEnc;
        uint8_t phe[16]; size_t phel = sizeof phe;
        if (cbor_map_bytes(&map, 5, newPinEnc, &npl) || npl != 64) return err(out, CTAP2_ERR_PIN_POLICY_VIOLATION);
        if (cbor_map_bytes(&map, 6, phe, &phel) || phel != 16)     return err(out, CTAP2_ERR_PIN_INVALID);
        uint8_t mac[16]; size_t macl = sizeof mac;
        if (cbor_map_bytes(&map, 4, mac, &macl))                   return err(out, CTAP2_ERR_MISSING_PARAMETER);
        uint8_t cat[80]; memcpy(cat, newPinEnc, 64); memcpy(cat + 64, phe, 16);   // newPinEnc || pinHashEnc
        if (!pin_auth_ok(cfg, ss, 32, cat, 80, mac, macl))         return err(out, CTAP2_ERR_PIN_AUTH_INVALID);
        uint8_t e2 = pin_check(cfg, ss, phe, stored, &retries); if (e2) return err(out, e2);
        uint8_t e3 = pin_set_from_enc(cfg, ss, newPinEnc); if (e3) return err(out, e3);
        out[0] = CTAP2_OK; return 1;
    }
    case 5:      // getPinToken (legacy, no permissions)
    case 9: {    // getPinUvAuthTokenUsingPinWithPermissions
        if (!haveStored) return err(out, CTAP2_ERR_PIN_NOT_SET);
        if (retries == 0) return err(out, CTAP2_ERR_PIN_BLOCKED);
        if (rt->boot_fails >= 3) return err(out, CTAP2_ERR_PIN_AUTH_BLOCKED);
        uint8_t ss[32]; uint8_t e = pin_shared(cfg, &map, ss); if (e) return err(out, e);
        uint8_t phe[16]; size_t phel = sizeof phe;
        if (cbor_map_bytes(&map, 6, phe, &phel) || phel != 16)     return err(out, CTAP2_ERR_PIN_INVALID);
        uint8_t e2 = pin_check(cfg, ss, phe, stored, &retries); if (e2) return err(out, e2);
        uint8_t perms = PIN_PERM_MC | PIN_PERM_GA;
        rt->token_rp_set = false;
        if (sub == 9) {
            uint64_t pm = 0; if (cbor_map_uint(&map, 9, &pm)) return err(out, CTAP2_ERR_MISSING_PARAMETER);
            perms = (uint8_t)(pm & (PIN_PERM_MC | PIN_PERM_GA));
            if (perms == 0) return err(out, CTAP2_ERR_INVALID_OPTION);
            char rpid[128]; size_t rl = sizeof rpid;
            if (cbor_map_text(&map, 0x0A, rpid, &rl) == 0) {
                if (cfg->cy->sha256((const uint8_t *)rpid, strlen(rpid), rt->token_rp, cfg->cy->ctx))
                    return err(out, CTAP1_ERR_OTHER);
                rt->token_rp_set = true;
            }
        }
        if (cfg->cy->rand(rt->token, 32, cfg->cy->ctx)) return err(out, CTAP1_ERR_OTHER);
        rt->token_set = true; rt->token_perms = perms;
        uint8_t iv[16] = {0}; uint8_t enc[32];
        if (cfg->cy->aes_cbc(ss, iv, 1, rt->token, 32, enc, cfg->cy->ctx)) return err(out, CTAP1_ERR_OTHER);
        out[0] = CTAP2_OK; cbor_writer w; cw_init(&w, out + 1, cap - 1);
        cw_map(&w, 1); cw_key(&w, 2); cw_bytes(&w, enc, 32);
        return (uint16_t)(1 + cw_finish(&w));
    }
    default:
        return err(out, CTAP2_ERR_INVALID_OPTION);
    }
}

// Reset is only accepted within this long after (re)power-up. This is the CTAP
// anti-wipe hardening: a brief-physical-access attacker can't reset a key that's
// been running. It WORKS with real platforms because their reset flow (Windows
// included) makes the user reinsert the key first, which restarts the window.
// boot_ms is stamped at key-mode init, i.e. right after the cold-boot re-sync's
// one extra reboot, so it tracks the reinsert. 30 s (vs the spec's ~10 s)
// generously absorbs that double-reboot + host round-trip while still shutting
// the "already running for minutes" attack.
#define KERB_RESET_WINDOW_MS 30000

static uint16_t reset_cmd(const ctap2_cfg_t *cfg, uint8_t *out, uint16_t cap) {
    (void)cap;
    ctap2_pin_rt *rt = cfg->pin_rt;
    if (rt && rt->boot_ms && cfg->now_ms &&
        (uint32_t)(cfg->now_ms - rt->boot_ms) > KERB_RESET_WINDOW_MS)
        return err(out, CTAP2_ERR_NOT_ALLOWED);   // reinsert the key, then reset
    if (!cfg->user_present(cfg->ui)) return err(out, CTAP2_ERR_OPERATION_DENIED);
    if (cfg->store && cfg->store->wipe) cfg->store->wipe(cfg->store);
    if (cfg->pin) cfg->pin->wipe(cfg->pin);
    if (rt) { rt->ka_ready = false; rt->token_set = false; rt->boot_fails = 0; }
    if (cfg->counter) *cfg->counter = 0;
    out[0] = CTAP2_OK; return 1;
}

uint16_t ctap2_handle(const ctap2_cfg_t *cfg, const uint8_t *req, uint16_t len,
                      uint8_t *out, uint16_t cap) {
    if (cap < 1) return 0;
    if (len < 1) { out[0] = CTAP1_ERR_INVALID_LENGTH; return 1; }
    switch (req[0]) {
        case CTAP2_GET_INFO:   return get_info(cfg, out, cap);
        case CTAP2_MAKE_CRED:  return make_cred(cfg, req, len, out, cap);
        case CTAP2_GET_ASSERT: return get_assert(cfg, req, len, out, cap);
        case CTAP2_CLIENT_PIN: return client_pin(cfg, req, len, out, cap);
        case CTAP2_RESET:      return reset_cmd(cfg, out, cap);
        default:               out[0] = CTAP1_ERR_INVALID_COMMAND; return 1;
    }
}
