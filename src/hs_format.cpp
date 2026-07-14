#include "hs_format.h"
#include <string.h>
#include <stdio.h>

static int hexcat(char *out, size_t out_sz, size_t pos,
                  const uint8_t *b, size_t n) {
    if (pos + n * 2 >= out_sz) return -1;
    static const char H[] = "0123456789abcdef";
    for (size_t i = 0; i < n; ++i) {
        out[pos++] = H[b[i] >> 4];
        out[pos++] = H[b[i] & 0x0f];
    }
    return (int)pos;
}

int hs_format_22000(char *out, size_t out_sz,
                    const uint8_t bssid[6], const uint8_t sta[6],
                    const uint8_t anonce[32],
                    const uint8_t *eapol, size_t eapol_len,
                    const char *essid) {
    if (!out || out_sz == 0) return -1;
    if (eapol_len < (size_t)(HS_EAPOL_MIC_OFFSET + 16)) return -1;

    /* MIC copied out before we zero it in the emitted frame. */
    uint8_t mic[16];
    memcpy(mic, eapol + HS_EAPOL_MIC_OFFSET, 16);

    int p = snprintf(out, out_sz, "WPA*02*");
    if (p < 0 || (size_t)p >= out_sz) return -1;

    p = hexcat(out, out_sz, p, mic, 16);             if (p < 0) return -1;
    if (p + 1 >= (int)out_sz) return -1; out[p++] = '*';
    p = hexcat(out, out_sz, p, bssid, 6);            if (p < 0) return -1;
    if (p + 1 >= (int)out_sz) return -1; out[p++] = '*';
    p = hexcat(out, out_sz, p, sta, 6);              if (p < 0) return -1;
    if (p + 1 >= (int)out_sz) return -1; out[p++] = '*';
    p = hexcat(out, out_sz, p, (const uint8_t *)essid, strlen(essid));
    if (p < 0) return -1;
    if (p + 1 >= (int)out_sz) return -1; out[p++] = '*';
    p = hexcat(out, out_sz, p, anonce, 32);          if (p < 0) return -1;
    if (p + 1 >= (int)out_sz) return -1; out[p++] = '*';

    /* EAPOL field: whole frame, MIC zeroed. Hex byte-by-byte so we can zero
     * the MIC region without mutating the caller's buffer. */
    if (eapol_len > 256) eapol_len = 256;
    static const char H[] = "0123456789abcdef";
    for (size_t i = 0; i < eapol_len; ++i) {
        uint8_t b = eapol[i];
        if (i >= (size_t)HS_EAPOL_MIC_OFFSET && i < (size_t)HS_EAPOL_MIC_OFFSET + 16)
            b = 0x00;
        if (p + 2 >= (int)out_sz) return -1;
        out[p++] = H[b >> 4];
        out[p++] = H[b & 0x0f];
    }
    if (p + 3 >= (int)out_sz) return -1;
    out[p++] = '*'; out[p++] = '0'; out[p++] = '0';   /* message-pair 00 */
    out[p] = '\0';
    return p;
}
