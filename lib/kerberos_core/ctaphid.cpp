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
    if (n) memcpy(pkt + 7, data, n);
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
