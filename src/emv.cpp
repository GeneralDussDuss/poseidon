/*
 * emv.cpp - see emv.h.
 *
 * Deliberately transport-agnostic: everything here is plain C++ over a
 * function pointer, so the BER-TLV and Track 2 parsing can be unit tested on
 * the host without a PN532 (the parsing is where the bugs live; the APDU
 * plumbing is trivial by comparison).
 */
#include "emv.h"

#include <string.h>
#include <stdio.h>

/* ---------------- BER-TLV ---------------- */

/* Read a BER-TLV tag at *p. Tags are 1-3 bytes: if the low 5 bits of the first
 * byte are all set, the tag continues while the high bit of each next byte is
 * set. Returns the tag as a big-endian packed integer. */
static uint32_t read_tag(const uint8_t **p, const uint8_t *end)
{
    uint32_t tag = *(*p)++;
    if ((tag & 0x1F) == 0x1F) {
        while (*p < end) {
            const uint8_t b = *(*p)++;
            tag = (tag << 8) | b;
            if (!(b & 0x80)) break;
        }
    }
    return tag;
}

/* Read a BER-TLV length. Short form is one byte < 0x80; long form has the low
 * 7 bits as a byte count. Returns false on a malformed/oversized length. */
static bool read_len(const uint8_t **p, const uint8_t *end, uint16_t *out)
{
    if (*p >= end) return false;
    uint8_t b = *(*p)++;
    if (!(b & 0x80)) { *out = b; return true; }
    const uint8_t nbytes = b & 0x7F;
    if (nbytes == 0 || nbytes > 2) return false;   /* >64KB is not a thing here */
    uint32_t v = 0;
    for (uint8_t i = 0; i < nbytes; ++i) {
        if (*p >= end) return false;
        v = (v << 8) | *(*p)++;
    }
    if (v > 0xFFFF) return false;
    *out = (uint16_t)v;
    return true;
}

/* Depth-bounded worker. The TLV being walked comes straight off the card, so
 * nesting depth is ATTACKER-CONTROLLED: a malicious or malfunctioning card can
 * hand back constructed templates nested arbitrarily deep and recurse this
 * function until the task stack is gone. Real EMV data nests ~3 levels
 * (BF0C > 61 > 4F), so 8 is generous and anything beyond it is hostile. */
#define EMV_MAX_TLV_DEPTH 8

static const uint8_t *find_tag_d(const uint8_t *buf, uint16_t len,
                                 uint32_t tag, uint16_t *out_len, uint8_t depth)
{
    if (depth > EMV_MAX_TLV_DEPTH) return nullptr;
    const uint8_t *p = buf, *end = buf + len;
    while (p < end) {
        /* Skip padding bytes that some cards sprinkle between objects. */
        if (*p == 0x00 || *p == 0xFF) { ++p; continue; }

        const uint8_t *tag_start = p;
        const uint32_t t = read_tag(&p, end);
        uint16_t l = 0;
        if (!read_len(&p, end, &l)) return nullptr;
        if (p + l > end) return nullptr;             /* truncated object */

        if (t == tag) { if (out_len) *out_len = l; return p; }

        /* Constructed (bit 6 of the first tag byte) -> recurse into it. */
        if (tag_start[0] & 0x20) {
            uint16_t inner_len = 0;
            const uint8_t *hit = find_tag_d(p, l, tag, &inner_len, (uint8_t)(depth + 1));
            if (hit) { if (out_len) *out_len = inner_len; return hit; }
        }
        p += l;
    }
    return nullptr;
}

const uint8_t *emv_find_tag(const uint8_t *buf, uint16_t len,
                            uint32_t tag, uint16_t *out_len)
{
    return find_tag_d(buf, len, tag, out_len, 0);
}

/* ---------------- Track 2 ---------------- */

bool emv_parse_track2(const uint8_t *t2, uint16_t len,
                      char *pan, size_t pan_sz, char *expiry, size_t exp_sz)
{
    /* Track 2 Equivalent is packed BCD: PAN, then 'D' (nibble 0xD) as the
     * field separator, then YYMM expiry, then service code and discretionary
     * data. Unpack nibbles until the separator. */
    if (!t2 || !len) return false;
    char digits[41];
    size_t n = 0;
    for (uint16_t i = 0; i < len && n < sizeof(digits) - 1; ++i) {
        const uint8_t hi = (uint8_t)(t2[i] >> 4);
        const uint8_t lo = (uint8_t)(t2[i] & 0x0F);
        digits[n++] = (char)(hi <= 9 ? '0' + hi : 'A' + (hi - 10));
        if (n < sizeof(digits) - 1)
            digits[n++] = (char)(lo <= 9 ? '0' + lo : 'A' + (lo - 10));
    }
    digits[n] = '\0';

    const char *sep = strchr(digits, 'D');
    if (!sep) return false;

    const size_t pan_len = (size_t)(sep - digits);
    if (pan_len < 8 || pan_len >= pan_sz) return false;
    memcpy(pan, digits, pan_len);
    pan[pan_len] = '\0';

    /* YYMM immediately after the separator -> render as MM/YY. */
    if (strlen(sep + 1) >= 4 && exp_sz >= 6) {
        expiry[0] = sep[3]; expiry[1] = sep[4];   /* MM */
        expiry[2] = '/';
        expiry[3] = sep[1]; expiry[4] = sep[2];   /* YY */
        expiry[5] = '\0';
    }
    return true;
}

/* ---------------- AID -> scheme ---------------- */

const char *emv_scheme_for_aid(const uint8_t *aid, uint8_t aid_len)
{
    struct { const char *name; uint8_t len; uint8_t p[7]; } K[] = {
        { "Visa",       5, { 0xA0,0x00,0x00,0x00,0x03 } },
        { "Mastercard", 5, { 0xA0,0x00,0x00,0x00,0x04 } },
        { "Amex",       5, { 0xA0,0x00,0x00,0x00,0x25 } },
        { "Discover",   5, { 0xA0,0x00,0x00,0x01,0x52 } },
        { "JCB",        5, { 0xA0,0x00,0x00,0x00,0x65 } },
        { "UnionPay",   5, { 0xA0,0x00,0x00,0x03,0x33 } },
        { "Interac",    5, { 0xA0,0x00,0x00,0x02,0x77 } },
        { "Maestro",    7, { 0xA0,0x00,0x00,0x00,0x04,0x30,0x60 } },
    };
    for (unsigned i = 0; i < sizeof(K) / sizeof(K[0]); ++i) {
        if (aid_len >= K[i].len && memcmp(aid, K[i].p, K[i].len) == 0)
            return K[i].name;
    }
    return "Unknown";
}

/* ---------------- card read flow ---------------- */

#define SW_OK(r, n) ((n) >= 2 && (r)[(n) - 2] == 0x90 && (r)[(n) - 1] == 0x00)

static void set_err(emv_card_t *c, const char *m)
{
    snprintf(c->error, sizeof(c->error), "%s", m);
}

/* Harvest whatever identity fields a record happens to carry. Called for
 * every record; first non-empty value wins. */
static void harvest(const uint8_t *rec, uint16_t len, emv_card_t *out)
{
    uint16_t l = 0;
    const uint8_t *v;

    if (!out->pan[0]) {
        /* Track 2 (57) is the most reliably present source, and carries the
         * expiry too, so try it before the standalone PAN tag. */
        if ((v = emv_find_tag(rec, len, 0x57, &l)) != nullptr)
            emv_parse_track2(v, l, out->pan, sizeof(out->pan),
                             out->expiry, sizeof(out->expiry));
        if (!out->pan[0] && (v = emv_find_tag(rec, len, 0x5A, &l)) != nullptr) {
            size_t n = 0;
            for (uint16_t i = 0; i < l && n < sizeof(out->pan) - 2; ++i) {
                const uint8_t hi = (uint8_t)(v[i] >> 4), lo = (uint8_t)(v[i] & 0x0F);
                if (hi <= 9) out->pan[n++] = (char)('0' + hi);
                if (lo <= 9) out->pan[n++] = (char)('0' + lo);
            }
            out->pan[n] = '\0';
        }
    }
    if (!out->expiry[0] && (v = emv_find_tag(rec, len, 0x5F24, &l)) != nullptr && l >= 2) {
        /* YYMMDD packed BCD -> MM/YY */
        snprintf(out->expiry, sizeof(out->expiry), "%02X/%02X", v[1], v[0]);
    }
    if (!out->holder[0] && (v = emv_find_tag(rec, len, 0x5F20, &l)) != nullptr && l) {
        const uint16_t n = (l < sizeof(out->holder) - 1) ? l : (uint16_t)(sizeof(out->holder) - 1);
        memcpy(out->holder, v, n);
        out->holder[n] = '\0';
        /* Cards pad the name with spaces; trim so the UI can centre it. */
        for (int i = (int)strlen(out->holder) - 1; i >= 0 && out->holder[i] == ' '; --i)
            out->holder[i] = '\0';
    }
}

bool emv_read_card(emv_apdu_fn send, emv_card_t *out)
{
    memset(out, 0, sizeof(*out));
    uint8_t r[264];
    int n;

    /* 1. SELECT PPSE -- the directory of supported payment applications. */
    static const uint8_t PPSE[] = {
        0x00, 0xA4, 0x04, 0x00, 0x0E,
        '2','P','A','Y','.','S','Y','S','.','D','D','F','0','1',
        0x00
    };
    n = send(PPSE, sizeof(PPSE), r, sizeof(r));
    if (n < 2 || !SW_OK(r, n)) { set_err(out, "no PPSE (not a payment card?)"); return false; }

    uint16_t l = 0;
    const uint8_t *aid = emv_find_tag(r, (uint16_t)(n - 2), 0x4F, &l);
    if (!aid || l == 0 || l > sizeof(out->aid)) { set_err(out, "no AID in PPSE"); return false; }
    memcpy(out->aid, aid, l);
    out->aid_len = (uint8_t)l;
    snprintf(out->scheme, sizeof(out->scheme), "%s", emv_scheme_for_aid(out->aid, out->aid_len));

    /* 2. SELECT the application. */
    uint8_t sel[6 + 16];
    sel[0] = 0x00; sel[1] = 0xA4; sel[2] = 0x04; sel[3] = 0x00; sel[4] = out->aid_len;
    memcpy(sel + 5, out->aid, out->aid_len);
    sel[5 + out->aid_len] = 0x00;
    n = send(sel, (uint8_t)(6 + out->aid_len), r, sizeof(r));
    if (n < 2 || !SW_OK(r, n)) { set_err(out, "AID select failed"); return false; }

    /* Does this card advertise a transaction log? Tag 9F4D = [SFI][count].
     * Most issuers omit it -- record that fact so the UI can say so plainly
     * instead of looking broken. */
    uint8_t log_sfi = 0, log_max = 0;
    const uint8_t *le = emv_find_tag(r, (uint16_t)(n - 2), 0x9F4D, &l);
    if (le && l >= 2) { out->log_supported = true; log_sfi = le[0]; log_max = le[1]; }

    /* 3. GET PROCESSING OPTIONS. If the card published a PDOL (9F38) we must
     * send a value for each requested item; sending zeros of the right total
     * length satisfies almost every card for a read-only session. */
    uint8_t gpo[64];
    uint8_t gi = 0;
    gpo[gi++] = 0x80; gpo[gi++] = 0xA8; gpo[gi++] = 0x00; gpo[gi++] = 0x00;
    const uint8_t *pdol = emv_find_tag(r, (uint16_t)(n - 2), 0x9F38, &l);
    if (pdol && l) {
        /* Sum the lengths the PDOL asks for. */
        uint16_t total = 0;
        const uint8_t *p = pdol, *end = pdol + l;
        while (p < end) {
            read_tag(&p, end);
            uint16_t ll = 0;
            if (!read_len(&p, end, &ll)) break;
            total = (uint16_t)(total + ll);
        }
        if (total > 40) total = 40;
        gpo[gi++] = (uint8_t)(2 + total);
        gpo[gi++] = 0x83; gpo[gi++] = (uint8_t)total;
        for (uint16_t i = 0; i < total; ++i) gpo[gi++] = 0x00;
    } else {
        gpo[gi++] = 0x02; gpo[gi++] = 0x83; gpo[gi++] = 0x00;
    }
    gpo[gi++] = 0x00;
    n = send(gpo, gi, r, sizeof(r));
    if (n < 2 || !SW_OK(r, n)) { set_err(out, "GPO refused"); return false; }

    /* 4. Walk the AFL and READ RECORD each entry. The AFL is 4-byte groups:
     *    [SFI<<3 | 0][first record][last record][offline-auth record count]. */
    const uint8_t *afl = emv_find_tag(r, (uint16_t)(n - 2), 0x94, &l);
    uint8_t afl_buf[64];
    uint16_t afl_len = 0;
    if (afl && l && l <= sizeof(afl_buf)) { memcpy(afl_buf, afl, l); afl_len = l; }
    else {
        /* Format 1 response (tag 80): AIP is the first 2 bytes, AFL follows. */
        const uint8_t *f1 = emv_find_tag(r, (uint16_t)(n - 2), 0x80, &l);
        if (f1 && l > 2) {
            afl_len = (uint16_t)(l - 2);
            if (afl_len > sizeof(afl_buf)) afl_len = sizeof(afl_buf);
            memcpy(afl_buf, f1 + 2, afl_len);
        }
    }

    for (uint16_t i = 0; i + 3 < afl_len; i += 4) {
        const uint8_t sfi   = (uint8_t)(afl_buf[i] >> 3);
        const uint8_t first = afl_buf[i + 1];
        const uint8_t last  = afl_buf[i + 2];
        /* first/last come FROM THE CARD. A last of 0xFF makes a uint8_t counter
         * wrap at 255 and loop forever; implausible SFIs waste a round trip each.
         * Bound both the values and the iteration count. */
        if (!sfi || sfi > 30 || first == 0 || last < first || last > 16) continue;
        for (uint16_t rec = first; rec <= last && rec < (uint16_t)first + 16; ++rec) {
            const uint8_t cmd[5] = { 0x00, 0xB2, rec, (uint8_t)((sfi << 3) | 0x04), 0x00 };
            n = send(cmd, sizeof(cmd), r, sizeof(r));
            if (n >= 2 && SW_OK(r, n)) harvest(r, (uint16_t)(n - 2), out);
        }
        if (out->pan[0] && out->expiry[0] && out->holder[0]) break;   /* got everything */
    }

    /* 5. Optional transaction log. Records live in their own SFI and are NOT
     * TLV -- they are raw fields laid out per the Log Format (9F4F), which we
     * do not parse here. Read them so we can report how many exist; decoding
     * amounts correctly needs the per-card format and is a follow-up. */
    if (out->log_supported && log_sfi && log_max) {
        if (log_max > EMV_MAX_LOG) log_max = EMV_MAX_LOG;
        for (uint8_t rec = 1; rec <= log_max; ++rec) {
            const uint8_t cmd[5] = { 0x00, 0xB2, rec, (uint8_t)((log_sfi << 3) | 0x04), 0x00 };
            n = send(cmd, sizeof(cmd), r, sizeof(r));
            if (n < 2 || !SW_OK(r, n)) break;
            if (out->txn_count < EMV_MAX_LOG) out->txn_count++;
        }
    }

    if (!out->pan[0]) { set_err(out, "no PAN in records"); return false; }
    out->valid = true;
    return true;
}
