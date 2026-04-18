/*
 * meshtastic_crypto — AES-CTR for default-channel packets.
 *
 * Uses mbedtls (ESP-IDF built-in) for AES-128. Counter block layout
 * matches firmware/src/mesh/CryptoEngine.cpp exactly:
 *
 *   bytes  0.. 7  packet_id (64-bit LE, upper 4 always 0 since id is fixed32)
 *   bytes  8..11  from_node (32-bit LE)
 *   bytes 12..15  block counter (starts at 0, increments per block)
 *
 * setCounterSize(4) in firmware puts the counter in the TRAILING 4 bytes.
 * mbedtls doesn't expose counter-size; hand-roll CTR via AES-ECB on the
 * counter block and XOR into the stream, incrementing only bytes 12..15.
 */
#include "meshtastic_internal.h"
#include <string.h>
#include "mbedtls/aes.h"

void mesh_crypto_ctr(const uint8_t key[16],
                     uint32_t packet_id,
                     uint32_t from_node,
                     uint8_t *data,
                     size_t len)
{
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    mbedtls_aes_setkey_enc(&ctx, key, 128);

    uint8_t counter[16] = {0};
    /* bytes 0..7 = packet_id LE (upper 32 bits always 0) */
    counter[0] = (uint8_t)(packet_id);
    counter[1] = (uint8_t)(packet_id >> 8);
    counter[2] = (uint8_t)(packet_id >> 16);
    counter[3] = (uint8_t)(packet_id >> 24);
    /* bytes 4..7 remain zero (upper 32 bits of 64-bit packet_id) */
    /* bytes 8..11 = from_node LE */
    counter[8]  = (uint8_t)(from_node);
    counter[9]  = (uint8_t)(from_node >> 8);
    counter[10] = (uint8_t)(from_node >> 16);
    counter[11] = (uint8_t)(from_node >> 24);
    /* bytes 12..15 = block counter, starts at zero */

    uint8_t stream[16];
    size_t off = 0;
    while (off < len) {
        mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_ENCRYPT, counter, stream);
        size_t chunk = (len - off) < 16 ? (len - off) : 16;
        for (size_t i = 0; i < chunk; i++) {
            data[off + i] ^= stream[i];
        }
        off += chunk;

        /* Increment ONLY bytes 12..15 (LE), leaving the IV prefix intact. */
        for (int i = 12; i < 16; i++) {
            if (++counter[i] != 0) break;
        }
    }

    mbedtls_aes_free(&ctx);
}
