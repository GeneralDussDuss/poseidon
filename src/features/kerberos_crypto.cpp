/*
 * kerberos_crypto — mbedtls (3.6.x) binding for the KERBEROS core crypto vtable.
 * Provides random, SHA-256, P-256 keygen, ECDSA-P256 sign, and AES-256-GCM.
 */
#define MBEDTLS_ALLOW_PRIVATE_ACCESS
#include "kerberos_crypto.h"
#include <string.h>
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/sha256.h"
#include "mbedtls/ecp.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/ecdh.h"
#include "mbedtls/gcm.h"
#include "mbedtls/md.h"
#include "mbedtls/aes.h"

static mbedtls_entropy_context  s_entropy;
static mbedtls_ctr_drbg_context s_drbg;
static bool s_seeded = false;

static int ensure_drbg(void) {
    if (s_seeded) return 0;
    mbedtls_entropy_init(&s_entropy);
    mbedtls_ctr_drbg_init(&s_drbg);
    const char *pers = "kerberos";
    if (mbedtls_ctr_drbg_seed(&s_drbg, mbedtls_entropy_func, &s_entropy,
                              (const unsigned char *)pers, strlen(pers)) != 0)
        return -1;
    s_seeded = true;
    return 0;
}

static int c_rand(uint8_t *dst, size_t n, void *) {
    if (ensure_drbg()) return -1;
    return mbedtls_ctr_drbg_random(&s_drbg, dst, n);
}

static int c_sha256(const uint8_t *m, size_t len, uint8_t out[32], void *) {
    return mbedtls_sha256(m, len, out, 0);
}

static int c_keygen(uint8_t priv[32], uint8_t pub[65], void *) {
    if (ensure_drbg()) return -1;
    mbedtls_ecp_keypair kp; mbedtls_ecp_keypair_init(&kp);
    int rc = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, &kp,
                                 mbedtls_ctr_drbg_random, &s_drbg);
    if (rc == 0) rc = mbedtls_mpi_write_binary(&kp.d, priv, 32);
    size_t olen = 0;
    if (rc == 0) rc = mbedtls_ecp_point_write_binary(&kp.grp, &kp.Q,
                          MBEDTLS_ECP_PF_UNCOMPRESSED, &olen, pub, 65);
    if (rc == 0 && olen != 65) rc = -1;
    mbedtls_ecp_keypair_free(&kp);
    return rc;
}

static int c_sign(const uint8_t priv[32], const uint8_t *m, size_t len,
                  uint8_t *sig, size_t *sl, void *) {
    if (ensure_drbg()) return -1;
    uint8_t hash[32];
    int rc = mbedtls_sha256(m, len, hash, 0);
    if (rc) return rc;
    mbedtls_ecp_keypair kp; mbedtls_ecp_keypair_init(&kp);
    mbedtls_ecdsa_context ec; mbedtls_ecdsa_init(&ec);
    rc = mbedtls_ecp_read_key(MBEDTLS_ECP_DP_SECP256R1, &kp, priv, 32);
    if (rc == 0) rc = mbedtls_ecdsa_from_keypair(&ec, &kp);
    if (rc == 0) rc = mbedtls_ecdsa_write_signature(&ec, MBEDTLS_MD_SHA256, hash, 32,
                          sig, 72, sl, mbedtls_ctr_drbg_random, &s_drbg);
    mbedtls_ecdsa_free(&ec);
    mbedtls_ecp_keypair_free(&kp);
    return rc;
}

static int c_seal(const uint8_t key[32], const uint8_t iv[12], const uint8_t *aad, size_t al,
                  const uint8_t *in, size_t len, uint8_t *out, uint8_t tag[16], void *) {
    mbedtls_gcm_context g; mbedtls_gcm_init(&g);
    int rc = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (rc == 0) rc = mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, len,
                          iv, 12, aad, al, in, out, 16, tag);
    mbedtls_gcm_free(&g);
    return rc;
}

static int c_open(const uint8_t key[32], const uint8_t iv[12], const uint8_t *aad, size_t al,
                  const uint8_t *in, size_t len, const uint8_t tag[16], uint8_t *out, void *) {
    mbedtls_gcm_context g; mbedtls_gcm_init(&g);
    int rc = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (rc == 0) rc = mbedtls_gcm_auth_decrypt(&g, len, iv, 12, aad, al, tag, 16, in, out);
    mbedtls_gcm_free(&g);
    return rc;
}

// ---- CTAP2 clientPIN primitives (pinUvAuthProtocol 1) ----

static int c_ecdh(const uint8_t priv[32], const uint8_t peer_pub[65],
                  uint8_t shared_x[32], void *) {
    if (ensure_drbg()) return -1;
    mbedtls_ecp_group grp; mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point Q;   mbedtls_ecp_point_init(&Q);
    mbedtls_mpi d, z;      mbedtls_mpi_init(&d); mbedtls_mpi_init(&z);
    int rc = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);
    if (rc == 0) rc = mbedtls_ecp_point_read_binary(&grp, &Q, peer_pub, 65);
    if (rc == 0) rc = mbedtls_ecp_check_pubkey(&grp, &Q);   // reject invalid points
    if (rc == 0) rc = mbedtls_mpi_read_binary(&d, priv, 32);
    if (rc == 0) rc = mbedtls_ecdh_compute_shared(&grp, &z, &Q, &d,
                          mbedtls_ctr_drbg_random, &s_drbg);
    if (rc == 0) rc = mbedtls_mpi_write_binary(&z, shared_x, 32);
    mbedtls_mpi_free(&d); mbedtls_mpi_free(&z);
    mbedtls_ecp_point_free(&Q); mbedtls_ecp_group_free(&grp);
    return rc;
}

static int c_hmac(const uint8_t *key, size_t kl, const uint8_t *msg, size_t len,
                  uint8_t out[32], void *) {
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info) return -1;
    return mbedtls_md_hmac(info, key, kl, msg, len, out);
}

static int c_aes_cbc(const uint8_t key[32], const uint8_t iv[16], int enc,
                     const uint8_t *in, size_t len, uint8_t *out, void *) {
    if (len % 16) return -1;
    mbedtls_aes_context a; mbedtls_aes_init(&a);
    uint8_t ivc[16]; memcpy(ivc, iv, 16);   // mbedtls mutates the IV; use a copy
    int rc = enc ? mbedtls_aes_setkey_enc(&a, key, 256)
                 : mbedtls_aes_setkey_dec(&a, key, 256);
    if (rc == 0) rc = mbedtls_aes_crypt_cbc(&a, enc ? MBEDTLS_AES_ENCRYPT : MBEDTLS_AES_DECRYPT,
                          len, ivc, in, out);
    mbedtls_aes_free(&a);
    return rc;
}

static const kerb_crypto_t S_CRYPTO = {
    c_rand, c_sha256, c_keygen, c_sign, c_seal, c_open,
    c_ecdh, c_hmac, c_aes_cbc,
    nullptr
};

const kerb_crypto_t *kerb_mbedtls_crypto(void) { return &S_CRYPTO; }
