#pragma once
#include <stdint.h>
#include <stddef.h>
#include "cbor.h"

// Thin conveniences over TinyCBOR for the CTAP2 map patterns. The writer wraps
// one top-level map; nested arrays/maps are built by grabbing cw_enc() and using
// TinyCBOR directly. Keys must be emitted in canonical (ascending) order by the
// caller. The parser side finds values by integer key (CTAP2 uses int keys,
// which TinyCBOR's string-keyed map_find does not cover).

typedef struct {
    CborEncoder enc;
    CborEncoder map;
    uint8_t    *buf;
} cbor_writer;

void   cw_init(cbor_writer *w, uint8_t *buf, size_t cap);
void   cw_map(cbor_writer *w, size_t pairs);      // open the top map
void   cw_key(cbor_writer *w, int key);           // integer key
void   cw_bytes(cbor_writer *w, const uint8_t *p, size_t n);
void   cw_text(cbor_writer *w, const char *s);
void   cw_uint(cbor_writer *w, uint64_t v);
void   cw_bool(cbor_writer *w, bool b);
CborEncoder *cw_enc(cbor_writer *w);              // the map encoder, for nesting
size_t cw_finish(cbor_writer *w);                 // close top map, return length

// Parser: fill `p` and position `map` at the top-level map. Returns 0 on success.
int cbor_get_map(const uint8_t *buf, size_t len, CborParser *p, CborValue *map);
// Integer-keyed getters. Return 0 on success, negative if the key is absent or
// the value has the wrong type. For bytes/text, *len is in/out (buffer cap in,
// actual length out).
int cbor_map_bytes(const CborValue *map, int key, uint8_t *dst, size_t *len);
int cbor_map_text (const CborValue *map, int key, char *dst, size_t *len);
int cbor_map_uint (const CborValue *map, int key, uint64_t *out);
int cbor_map_bool (const CborValue *map, int key, bool *out);
int cbor_map_enter(const CborValue *map, int key, CborValue *out);
