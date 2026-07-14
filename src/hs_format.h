#pragma once
#include <stdint.h>
#include <stddef.h>

/* Offset of the 16-byte Key MIC inside a full M2 802.1X frame (measured from
 * the 802.1X version byte). */
#define HS_EAPOL_MIC_OFFSET 81

/* Build a hashcat-22000 WPA*02* line into `out`. Returns the string length
 * (excluding NUL), or -1 if it would not fit `out_sz`. The MIC is read from
 * eapol[HS_EAPOL_MIC_OFFSET..+16] and zeroed in the emitted EAPOL field. */
int hs_format_22000(char *out, size_t out_sz,
                    const uint8_t bssid[6], const uint8_t sta[6],
                    const uint8_t anonce[32],
                    const uint8_t *eapol, size_t eapol_len,
                    const char *essid);
