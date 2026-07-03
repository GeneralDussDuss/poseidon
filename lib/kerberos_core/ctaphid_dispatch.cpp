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

void ctaphid_set_cbor(ctaphid_ctx_t *c, ctaphid_msg_fn on_cbor, void *cbor_ctx) {
    c->on_cbor = on_cbor; c->cbor_ctx = cbor_ctx;
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
            resp[16]=0x05;                                // capabilities: CBOR (0x04) | WINK (0x01)
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
        case W_CBOR: {
            if (!c->on_cbor) { send_err(c, cid, ERR_INVALID_CMD); break; }
            static uint8_t cresp[2048];
            uint16_t n = c->on_cbor(c->asm_.buf, c->asm_.bcnt, cresp, sizeof cresp, c->cbor_ctx);
            ctaphid_send(cid, W_CBOR, cresp, n, c->sink, c->sink_ctx);
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
