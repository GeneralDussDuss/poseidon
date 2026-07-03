#include "ctap2.h"
#include "cbor_util.h"
#include <string.h>

// CTAP2 status codes used here.
enum {
    CTAP2_OK                    = 0x00,
    CTAP1_ERR_INVALID_COMMAND   = 0x01,
    CTAP1_ERR_INVALID_LENGTH    = 0x03,
    CTAP2_ERR_UNSUPPORTED_ALGORITHM = 0x26,
    CTAP2_ERR_OPERATION_DENIED  = 0x27,
    CTAP2_ERR_NO_CREDENTIALS    = 0x2E,
};

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

uint16_t ctap2_handle(const ctap2_cfg_t *cfg, const uint8_t *req, uint16_t len,
                      uint8_t *out, uint16_t cap) {
    if (cap < 1) return 0;
    if (len < 1) { out[0] = CTAP1_ERR_INVALID_LENGTH; return 1; }
    switch (req[0]) {
        case CTAP2_GET_INFO: return get_info(cfg, out, cap);
        default:             out[0] = CTAP1_ERR_INVALID_COMMAND; return 1;
    }
}
