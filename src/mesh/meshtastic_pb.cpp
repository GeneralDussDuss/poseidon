/*
 * meshtastic_pb — hand-rolled minimal protobuf codec.
 *
 * We only need to encode/decode four Meshtastic protos:
 *   Data     — the encrypted payload
 *   User     — NodeInfo
 *   Position — GPS coordinates
 *   (MeshPacket itself is never serialized on the wire; the envelope fields
 *    come from the 16-byte binary header.)
 *
 * Wire format (standard protobuf 3):
 *   tag         = (field_number << 3) | wire_type
 *   varint      = LEB128 encoded uint64
 *   fixed32     = 4 bytes little-endian
 *   length-dlm  = varint length + bytes
 *
 * All our field numbers are low (<16), so every tag fits in a single byte.
 */
#include "meshtastic.h"
#include "meshtastic_internal.h"
#include <string.h>

/* ==================== encoder primitives ==================== */

static inline bool write_byte(mesh_buf_t *b, uint8_t v)
{
    if (b->len >= b->cap) return false;
    b->data[b->len++] = v;
    return true;
}

static bool write_varint(mesh_buf_t *b, uint64_t v)
{
    while (v >= 0x80) {
        if (!write_byte(b, (uint8_t)(v | 0x80))) return false;
        v >>= 7;
    }
    return write_byte(b, (uint8_t)v);
}

static inline bool write_tag(mesh_buf_t *b, uint8_t field, uint8_t wire)
{
    return write_varint(b, (uint64_t)((field << 3) | wire));
}

static bool write_fixed32(mesh_buf_t *b, uint32_t v)
{
    if (b->len + 4 > b->cap) return false;
    b->data[b->len++] = (uint8_t)(v & 0xFF);
    b->data[b->len++] = (uint8_t)((v >> 8) & 0xFF);
    b->data[b->len++] = (uint8_t)((v >> 16) & 0xFF);
    b->data[b->len++] = (uint8_t)((v >> 24) & 0xFF);
    return true;
}

static bool write_bytes(mesh_buf_t *b, uint8_t field, const uint8_t *bytes, size_t n)
{
    if (!write_tag(b, field, 2)) return false;
    if (!write_varint(b, (uint64_t)n)) return false;
    if (b->len + n > b->cap) return false;
    memcpy(b->data + b->len, bytes, n);
    b->len += n;
    return true;
}

static inline bool write_string(mesh_buf_t *b, uint8_t field, const char *s)
{
    return write_bytes(b, field, (const uint8_t *)s, strlen(s));
}

static inline bool write_varint_field(mesh_buf_t *b, uint8_t field, uint64_t v)
{
    if (!write_tag(b, field, 0)) return false;
    return write_varint(b, v);
}

static inline bool write_fixed32_field(mesh_buf_t *b, uint8_t field, uint32_t v)
{
    if (!write_tag(b, field, 5)) return false;
    return write_fixed32(b, v);
}

/* ==================== decoder primitives ==================== */

static bool read_varint(const uint8_t **p, const uint8_t *end, uint64_t *out)
{
    uint64_t v = 0;
    int shift = 0;
    while (*p < end) {
        uint8_t byte = *(*p)++;
        v |= (uint64_t)(byte & 0x7F) << shift;
        if (!(byte & 0x80)) { *out = v; return true; }
        shift += 7;
        if (shift >= 64) return false;
    }
    return false;
}

static bool read_fixed32(const uint8_t **p, const uint8_t *end, uint32_t *out)
{
    if (*p + 4 > end) return false;
    uint32_t v = (uint32_t)(*p)[0]
               | ((uint32_t)(*p)[1] << 8)
               | ((uint32_t)(*p)[2] << 16)
               | ((uint32_t)(*p)[3] << 24);
    *p += 4;
    *out = v;
    return true;
}

static bool skip_field(const uint8_t **p, const uint8_t *end, uint8_t wire)
{
    switch (wire) {
    case 0: { /* varint */
        uint64_t tmp;
        return read_varint(p, end, &tmp);
    }
    case 5: /* fixed32 */
        if (*p + 4 > end) return false;
        *p += 4;
        return true;
    case 1: /* fixed64 */
        if (*p + 8 > end) return false;
        *p += 8;
        return true;
    case 2: { /* length-delimited */
        uint64_t n;
        if (!read_varint(p, end, &n)) return false;
        if (*p + n > end) return false;
        *p += n;
        return true;
    }
    default:
        return false;
    }
}

static bool read_length(const uint8_t **p, const uint8_t *end,
                        const uint8_t **out_data, size_t *out_len)
{
    uint64_t n;
    if (!read_varint(p, end, &n)) return false;
    if (*p + n > end) return false;
    *out_data = *p;
    *out_len = (size_t)n;
    *p += n;
    return true;
}

/* ==================== Data proto ==================== */

bool mesh_pb_encode_data(mesh_buf_t *b, const mesh_data_t *d)
{
    if (d->portnum && !write_varint_field(b, 1, (uint64_t)d->portnum)) return false;
    if (d->payload_len && !write_bytes(b, 2, d->payload, d->payload_len)) return false;
    if (d->want_response && !write_varint_field(b, 3, 1)) return false;
    if (d->dest && !write_fixed32_field(b, 4, d->dest)) return false;
    if (d->source && !write_fixed32_field(b, 5, d->source)) return false;
    if (d->request_id && !write_fixed32_field(b, 6, d->request_id)) return false;
    if (d->reply_id && !write_fixed32_field(b, 7, d->reply_id)) return false;
    /* Field 8 `emoji` is fixed32 on the wire despite the name. Not used by us. */
    if (d->bitfield && !write_varint_field(b, 9, d->bitfield)) return false;
    return true;
}

bool mesh_pb_decode_data(const uint8_t *buf, size_t len, mesh_data_t *out)
{
    memset(out, 0, sizeof(*out));
    const uint8_t *p = buf;
    const uint8_t *end = buf + len;
    while (p < end) {
        uint64_t tag;
        if (!read_varint(&p, end, &tag)) return false;
        uint8_t field = (uint8_t)(tag >> 3);
        uint8_t wire  = (uint8_t)(tag & 0x07);
        switch (field) {
        case 1: {
            if (wire != 0) return false;
            uint64_t v; if (!read_varint(&p, end, &v)) return false;
            out->portnum = (uint32_t)v;
            break;
        }
        case 2: {
            if (wire != 2) return false;
            const uint8_t *pl; size_t n;
            if (!read_length(&p, end, &pl, &n)) return false;
            if (n > sizeof(out->payload)) n = sizeof(out->payload);
            memcpy(out->payload, pl, n);
            out->payload_len = (uint16_t)n;
            break;
        }
        case 3: {
            if (wire != 0) return false;
            uint64_t v; if (!read_varint(&p, end, &v)) return false;
            out->want_response = (v != 0);
            break;
        }
        case 4: case 5: case 6: case 7: case 8: {
            if (wire != 5) return false;
            uint32_t v; if (!read_fixed32(&p, end, &v)) return false;
            if (field == 4) out->dest = v;
            else if (field == 5) out->source = v;
            else if (field == 6) out->request_id = v;
            else if (field == 7) out->reply_id = v;
            /* field 8 emoji — ignored */
            break;
        }
        case 9: {
            if (wire != 0) return false;
            uint64_t v; if (!read_varint(&p, end, &v)) return false;
            out->bitfield = (uint32_t)v;
            break;
        }
        default:
            if (!skip_field(&p, end, wire)) return false;
        }
    }
    return true;
}

/* ==================== User proto (NodeInfo) ==================== */

bool mesh_pb_encode_user(mesh_buf_t *b, const mesh_user_t *u)
{
    if (u->id[0] && !write_string(b, 1, u->id)) return false;
    if (u->long_name[0] && !write_string(b, 2, u->long_name)) return false;
    if (u->short_name[0] && !write_string(b, 3, u->short_name)) return false;
    /* field 4 macaddr is deprecated; firmware still populates it but
     * modern apps use the node ID. Skipping. */
    if (u->hw_model && !write_varint_field(b, 5, u->hw_model)) return false;
    if (u->is_licensed && !write_varint_field(b, 6, 1)) return false;
    if (u->role && !write_varint_field(b, 7, u->role)) return false;
    return true;
}

bool mesh_pb_decode_user(const uint8_t *buf, size_t len, mesh_user_t *out)
{
    memset(out, 0, sizeof(*out));
    const uint8_t *p = buf;
    const uint8_t *end = buf + len;
    while (p < end) {
        uint64_t tag;
        if (!read_varint(&p, end, &tag)) return false;
        uint8_t field = (uint8_t)(tag >> 3);
        uint8_t wire  = (uint8_t)(tag & 0x07);
        switch (field) {
        case 1: case 2: case 3: {
            if (wire != 2) return false;
            const uint8_t *s; size_t n;
            if (!read_length(&p, end, &s, &n)) return false;
            char *dst = (field == 1) ? out->id :
                        (field == 2) ? out->long_name : out->short_name;
            size_t cap = (field == 1) ? sizeof(out->id) :
                         (field == 2) ? sizeof(out->long_name) : sizeof(out->short_name);
            if (n >= cap) n = cap - 1;
            memcpy(dst, s, n);
            dst[n] = '\0';
            break;
        }
        case 5: {
            if (wire != 0) return false;
            uint64_t v; if (!read_varint(&p, end, &v)) return false;
            out->hw_model = (uint32_t)v;
            break;
        }
        case 6: {
            if (wire != 0) return false;
            uint64_t v; if (!read_varint(&p, end, &v)) return false;
            out->is_licensed = (v != 0);
            break;
        }
        case 7: {
            if (wire != 0) return false;
            uint64_t v; if (!read_varint(&p, end, &v)) return false;
            out->role = (uint32_t)v;
            break;
        }
        default:
            if (!skip_field(&p, end, wire)) return false;
        }
    }
    return true;
}

/* ==================== Position proto ==================== */

bool mesh_pb_encode_position(mesh_buf_t *b, const mesh_position_t *pos)
{
    /* lat/lon are sfixed32 — same wire as fixed32 but signed interpretation. */
    if (pos->latitude_i  && !write_fixed32_field(b, 1, (uint32_t)pos->latitude_i))  return false;
    if (pos->longitude_i && !write_fixed32_field(b, 2, (uint32_t)pos->longitude_i)) return false;
    if (pos->altitude) {
        /* altitude is proto3 int32 = varint with two's-complement sign
         * extension (negatives take 10 bytes). Casting int32 -> int64 ->
         * uint64 does this correctly. */
        if (!write_tag(b, 3, 0)) return false;
        if (!write_varint(b, (uint64_t)(int64_t)pos->altitude)) return false;
    }
    if (pos->time && !write_fixed32_field(b, 4, pos->time)) return false;
    if (pos->location_source && !write_varint_field(b, 5, pos->location_source)) return false;
    if (pos->sats_in_view && !write_varint_field(b, 19, pos->sats_in_view)) return false;
    return true;
}

bool mesh_pb_decode_position(const uint8_t *buf, size_t len, mesh_position_t *out)
{
    memset(out, 0, sizeof(*out));
    const uint8_t *p = buf;
    const uint8_t *end = buf + len;
    while (p < end) {
        uint64_t tag;
        if (!read_varint(&p, end, &tag)) return false;
        uint8_t field = (uint8_t)(tag >> 3);
        uint8_t wire  = (uint8_t)(tag & 0x07);
        switch (field) {
        case 1: case 2: {
            if (wire != 5) return false;
            uint32_t v; if (!read_fixed32(&p, end, &v)) return false;
            int32_t sv = (int32_t)v;
            if (field == 1) out->latitude_i = sv;
            else            out->longitude_i = sv;
            break;
        }
        case 3: {
            if (wire != 0) return false;
            uint64_t v; if (!read_varint(&p, end, &v)) return false;
            out->altitude = (int32_t)v;
            break;
        }
        case 4: {
            if (wire != 5) return false;
            uint32_t v; if (!read_fixed32(&p, end, &v)) return false;
            out->time = v;
            break;
        }
        case 5: {
            if (wire != 0) return false;
            uint64_t v; if (!read_varint(&p, end, &v)) return false;
            out->location_source = (uint32_t)v;
            break;
        }
        case 19: {
            if (wire != 0) return false;
            uint64_t v; if (!read_varint(&p, end, &v)) return false;
            out->sats_in_view = (uint32_t)v;
            break;
        }
        default:
            if (!skip_field(&p, end, wire)) return false;
        }
    }
    return true;
}
