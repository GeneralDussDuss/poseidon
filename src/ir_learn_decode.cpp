#include "ir_learn_decode.h"

uint16_t ir_symbols_to_us(const ir_edge_pair_t *syms, size_t nsyms,
                          uint16_t *out, uint16_t out_max, bool *truncated) {
    if (truncated) *truncated = false;
    uint16_t n = 0;
    for (size_t i = 0; i < nsyms; ++i) {
        if (syms[i].d0 == 0) return n;                 /* end-of-frame */
        if (n >= out_max) { if (truncated) *truncated = true; return n; }
        out[n++] = syms[i].d0;
        if (syms[i].d1 == 0) return n;                 /* end-of-frame */
        if (n >= out_max) { if (truncated) *truncated = true; return n; }
        out[n++] = syms[i].d1;
    }
    return n;
}
