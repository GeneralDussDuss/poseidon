#pragma once
#include <stdint.h>
#include <stddef.h>

/* One RMT symbol: two (level,duration) halves, mirrors rmt_symbol_word_t but
 * IDF-free so the flattener is host-testable. */
struct ir_edge_pair_t { uint16_t d0; uint8_t l0; uint16_t d1; uint8_t l1; };

/* Flatten symbol pairs into an alternating mark/space duration array (µs).
 * Stops at the first zero duration (RMT end-of-frame) or when out_max is hit
 * (sets *truncated). Returns the number of durations written. */
uint16_t ir_symbols_to_us(const ir_edge_pair_t *syms, size_t nsyms,
                          uint16_t *out, uint16_t out_max, bool *truncated);
