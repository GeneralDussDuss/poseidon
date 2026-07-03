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
    CTAP2_ERR_NO_CREDENTIALS    = 0x2E,
    CTAP1_ERR_OTHER             = 0x7F,
};

static uint16_t err(uint8_t *out, uint8_t code) { out[0] = code; return 1; }

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

static uint16_t get_info(const ctap2_cfg_t *cfg, uint8_t *out, uint16_t cap) {
    out[0] = CTAP2_OK;
    cbor_writer w; cw_init(&w, out + 1, cap - 1);
    cw_map(&w, 3);                                    // keys 1,3,4 (ascending)
    // 1: versions
    cw_key(&w, 1);
    CborEncoder arr; cbor_encoder_create_array(cw_enc(&w), &arr, 2);
    cbor_encode_text_stringz(&arr, "U2F_V2");
    cbor_encode_text_stringz(&arr, "FIDO_2_0");
    cbor_encoder_close_container(cw_enc(&w), &arr);
    // 3: aaguid
    cw_key(&w, 3); cw_bytes(&w, cfg->aaguid, 16);
    // 4: options {rk, up}  (canonical: "rk" before "up")
    cw_key(&w, 4);
    CborEncoder opt; cbor_encoder_create_map(cw_enc(&w), &opt, 2);
    cbor_encode_text_stringz(&opt, "rk"); cbor_encode_boolean(&opt, true);
    cbor_encode_text_stringz(&opt, "up"); cbor_encode_boolean(&opt, true);
    cbor_encoder_close_container(cw_enc(&w), &opt);
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
    uint8_t authData[320]; size_t adLen = authdata_build(rpIdHash, AD_FLAG_UP | AD_FLAG_AT, 0,
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
    size_t adLen = authdata_build(rpIdHash, AD_FLAG_UP, ctr, nullptr, 0, authData, sizeof authData);
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

uint16_t ctap2_handle(const ctap2_cfg_t *cfg, const uint8_t *req, uint16_t len,
                      uint8_t *out, uint16_t cap) {
    if (cap < 1) return 0;
    if (len < 1) { out[0] = CTAP1_ERR_INVALID_LENGTH; return 1; }
    switch (req[0]) {
        case CTAP2_GET_INFO:   return get_info(cfg, out, cap);
        case CTAP2_MAKE_CRED:  return make_cred(cfg, req, len, out, cap);
        case CTAP2_GET_ASSERT: return get_assert(cfg, req, len, out, cap);
        default:               out[0] = CTAP1_ERR_INVALID_COMMAND; return 1;
    }
}
