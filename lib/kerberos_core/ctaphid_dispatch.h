#pragma once
#include "ctaphid.h"

typedef uint16_t (*ctaphid_msg_fn)(const uint8_t *req, uint16_t req_len,
                                   uint8_t *resp, uint16_t resp_cap, void *ctx);

typedef struct {
    ctaphid_assembler_t asm_;
    ctaphid_sink_fn     sink;
    void               *sink_ctx;
    ctaphid_msg_fn      on_msg;      // U2F (CTAPHID_MSG) handler, may be nullptr
    void               *msg_ctx;
    ctaphid_msg_fn      on_cbor;     // CTAP2 (CTAPHID_CBOR) handler, may be nullptr
    void               *cbor_ctx;
    uint32_t            next_cid;
} ctaphid_ctx_t;

void     ctaphid_ctx_init(ctaphid_ctx_t *c, ctaphid_sink_fn sink, void *sink_ctx,
                          ctaphid_msg_fn on_msg, void *msg_ctx);
void     ctaphid_set_cbor(ctaphid_ctx_t *c, ctaphid_msg_fn on_cbor, void *cbor_ctx);
uint32_t ctaphid_dispatch(ctaphid_ctx_t *c, const uint8_t pkt[CTAPHID_PKT]);
void     ctaphid_keepalive(ctaphid_ctx_t *c, uint32_t cid, uint8_t status);
