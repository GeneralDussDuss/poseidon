#include "cose.h"
#include "cbor.h"

size_t cose_es256_from_pubkey(const uint8_t pub[65], uint8_t *out, size_t cap) {
    CborEncoder enc, map;
    cbor_encoder_init(&enc, out, cap, 0);
    cbor_encoder_create_map(&enc, &map, 5);
    cbor_encode_int(&map, 1);  cbor_encode_int(&map, 2);        // kty: EC2
    cbor_encode_int(&map, 3);  cbor_encode_int(&map, -7);       // alg: ES256
    cbor_encode_int(&map, -1); cbor_encode_int(&map, 1);        // crv: P-256
    cbor_encode_int(&map, -2); cbor_encode_byte_string(&map, pub + 1, 32);   // x
    cbor_encode_int(&map, -3); cbor_encode_byte_string(&map, pub + 33, 32);  // y
    cbor_encoder_close_container(&enc, &map);
    return cbor_encoder_get_buffer_size(&enc, out);
}
