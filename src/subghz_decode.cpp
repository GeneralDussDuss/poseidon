/*
 * subghz_decode.cpp — OOK protocol decoder.
 *
 * Matches pulse patterns against known fixed-code protocols.
 * Tolerance: ±30% on pulse widths.
 */
#include "subghz_decode.h"

static bool match(int actual, int expected)
{
    int lo = expected * 7 / 10;
    int hi = expected * 13 / 10;
    int a = abs(actual);
    return a >= lo && a <= hi;
}

/* Try to decode Princeton / PT2262 (24-bit, protocol 1).
 * Encoding: short=1T, long=3T. Sync=1T high + 31T low.
 * Bit 0: 1T high, 3T low. Bit 1: 3T high, 1T low. */
static bool try_princeton(const int16_t *p, int n, subghz_decoded_t *out)
{
    if (n < 50) return false;
    /* Find sync: positive pulse followed by long negative. */
    for (int i = 0; i < n - 50; i++) {
        if (p[i] <= 0) continue;
        int t = abs(p[i]);  /* base timing unit */
        if (t < 100 || t > 1000) continue;
        if (!match(p[i + 1], -(int)(t * 31))) continue;
        /* Found sync. Decode 24 bits. */
        uint32_t val = 0;
        int bits = 0;
        int j = i + 2;
        while (bits < 24 && j + 1 < n) {
            int hi = abs(p[j]), lo = abs(p[j + 1]);
            if (match(hi, t) && match(lo, t * 3)) {
                val = (val << 1) | 0; bits++;
            } else if (match(hi, t * 3) && match(lo, t)) {
                val = (val << 1) | 1; bits++;
            } else break;
            j += 2;
        }
        if (bits == 24) {
            out->protocol = "Princeton";
            out->value = val;
            out->bits = 24;
            out->valid = true;
            return true;
        }
    }
    return false;
}

/* CAME 12-bit: short=320us, long=640us. Sync=320 high + 11520 low. */
static bool try_came(const int16_t *p, int n, subghz_decoded_t *out)
{
    if (n < 26) return false;
    for (int i = 0; i < n - 26; i++) {
        if (p[i] <= 0) continue;
        if (!match(p[i], 320)) continue;
        if (!match(p[i + 1], -11520)) continue;
        uint32_t val = 0;
        int bits = 0, j = i + 2;
        while (bits < 12 && j + 1 < n) {
            int hi = abs(p[j]), lo = abs(p[j + 1]);
            if (match(hi, 320) && match(lo, 640)) {
                val = (val << 1) | 0; bits++;
            } else if (match(hi, 640) && match(lo, 320)) {
                val = (val << 1) | 1; bits++;
            } else break;
            j += 2;
        }
        if (bits == 12) {
            out->protocol = "CAME 12bit";
            out->value = val;
            out->bits = 12;
            out->valid = true;
            return true;
        }
    }
    return false;
}

/* NICE 12-bit: short=700us, long=1400us. Sync=700 high + 25200 low. */
static bool try_nice(const int16_t *p, int n, subghz_decoded_t *out)
{
    if (n < 26) return false;
    for (int i = 0; i < n - 26; i++) {
        if (p[i] <= 0) continue;
        if (!match(p[i], 700)) continue;
        if (!match(p[i + 1], -25200)) continue;
        uint32_t val = 0;
        int bits = 0, j = i + 2;
        while (bits < 12 && j + 1 < n) {
            int hi = abs(p[j]), lo = abs(p[j + 1]);
            if (match(hi, 700) && match(lo, 1400)) {
                val = (val << 1) | 0; bits++;
            } else if (match(hi, 1400) && match(lo, 700)) {
                val = (val << 1) | 1; bits++;
            } else break;
            j += 2;
        }
        if (bits == 12) {
            out->protocol = "NICE 12bit";
            out->value = val;
            out->bits = 12;
            out->valid = true;
            return true;
        }
    }
    return false;
}

/* Linear 10-bit: short=500us, long=1000us. */
static bool try_linear(const int16_t *p, int n, subghz_decoded_t *out)
{
    if (n < 22) return false;
    for (int i = 0; i < n - 22; i++) {
        if (p[i] <= 0) continue;
        if (!match(p[i], 500)) continue;
        if (abs(p[i + 1]) < 5000) continue;
        uint32_t val = 0;
        int bits = 0, j = i + 2;
        while (bits < 10 && j + 1 < n) {
            int hi = abs(p[j]), lo = abs(p[j + 1]);
            if (match(hi, 500) && match(lo, 1000)) {
                val = (val << 1) | 0; bits++;
            } else if (match(hi, 1000) && match(lo, 500)) {
                val = (val << 1) | 1; bits++;
            } else break;
            j += 2;
        }
        if (bits == 10) {
            out->protocol = "Linear 10bit";
            out->value = val;
            out->bits = 10;
            out->valid = true;
            return true;
        }
    }
    return false;
}

/* Generic fixed-code: try common base timings and decode as many bits as possible. */
static bool try_generic(const int16_t *p, int n, subghz_decoded_t *out)
{
    if (n < 10) return false;
    /* Find the most common short pulse width. */
    int hist[10] = {0};
    int buckets[] = {200, 300, 400, 500, 600, 700, 800, 1000, 1200, 1500};
    for (int i = 0; i < n; i++) {
        int a = abs(p[i]);
        for (int b = 0; b < 10; b++) {
            if (a >= buckets[b] * 7 / 10 && a <= buckets[b] * 13 / 10) {
                hist[b]++;
                break;
            }
        }
    }
    int best = 0;
    for (int b = 1; b < 10; b++) if (hist[b] > hist[best]) best = b;
    int t = buckets[best];
    if (hist[best] < 6) return false;

    /* Decode with 1T/3T encoding. */
    uint32_t val = 0;
    int bits = 0;
    for (int i = 0; i + 1 < n && bits < 32; i += 2) {
        int hi = abs(p[i]), lo = abs(p[i + 1]);
        if (match(hi, t) && match(lo, t * 3)) {
            val = (val << 1) | 0; bits++;
        } else if (match(hi, t * 3) && match(lo, t)) {
            val = (val << 1) | 1; bits++;
        } else if (match(hi, t) && match(lo, t)) {
            val = (val << 1) | 0; bits++;
        }
    }
    if (bits >= 8) {
        out->protocol = "Generic OOK";
        out->value = val;
        out->bits = bits;
        out->valid = true;
        return true;
    }
    return false;
}

subghz_decoded_t subghz_decode(const int16_t *pulses, int count)
{
    subghz_decoded_t r = { "Unknown", 0, 0, 0, false };
    if (try_princeton(pulses, count, &r)) return r;
    if (try_came(pulses, count, &r)) return r;
    if (try_nice(pulses, count, &r)) return r;
    if (try_linear(pulses, count, &r)) return r;
    if (try_generic(pulses, count, &r)) return r;
    return r;
}
