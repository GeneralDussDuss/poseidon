#pragma once
#include <stdint.h>
#include <stddef.h>

#define CTAPHID_PKT        64
#define CTAPHID_INIT_DATA  (CTAPHID_PKT - 7)   // 57
#define CTAPHID_CONT_DATA  (CTAPHID_PKT - 5)   // 59
#define CTAPHID_MAXLEN     2048                 // Phase 1 cap; U2F messages are small

typedef struct {
    uint32_t cid;
    uint8_t  cmd;
    uint16_t bcnt;
    uint16_t got;
    uint8_t  seq;        // next expected continuation seq
    uint8_t  buf[CTAPHID_MAXLEN];
    int      active;
} ctaphid_assembler_t;

typedef void (*ctaphid_sink_fn)(const uint8_t pkt[CTAPHID_PKT], void *ctx);

// Returns 1 = message complete, 0 = need more, negative = framing error.
int  ctaphid_feed(ctaphid_assembler_t *a, const uint8_t pkt[CTAPHID_PKT]);
void ctaphid_send(uint32_t cid, uint8_t cmd, const uint8_t *data, uint16_t len,
                  ctaphid_sink_fn sink, void *sctx);
