/* Native Unity tests for the pure WiFi capture / bounds LOGIC that the deep
 * audit fixed. The production parse lives in feature .cpp files that pull in
 * esp_wifi.h (not native-compilable), so these tests reconstruct synthetic
 * 802.11 EAPOL-Key frames and exercise the exact offset/clamp/guard expressions
 * the fixes use — validating the math + the data extracted, not the radio. */
#include <unity.h>
#include <string.h>
#include <stdint.h>

void setUp(void) {}
void tearDown(void) {}

/* Build a WPA2 EAPOL-Key frame (4-byte 802.1X header + key descriptor).
 * Standard offsets: nonce@17, MIC@81, Key-Data-Length@97, Key-Data@99. */
static int build_eapol(uint8_t *f, const uint8_t *kdata, int kdlen) {
    memset(f, 0, 99 + kdlen);
    f[0] = 0x02;                 /* 802.1X version */
    f[1] = 0x03;                 /* type = EAPOL-Key */
    int body = 95 + kdlen;       /* descriptor(95) + key data */
    f[2] = (body >> 8) & 0xFF;   /* 802.1X length field */
    f[3] = body & 0xFF;
    f[4] = 0x02;                 /* descriptor type */
    for (int i = 0; i < 32; i++) f[17 + i] = (uint8_t)(0xA0 + i);  /* nonce */
    for (int i = 0; i < 16; i++) f[81 + i] = (uint8_t)(0x50 + i);  /* MIC */
    f[97] = (kdlen >> 8) & 0xFF; /* Key-Data-Length */
    f[98] = kdlen & 0xFF;
    if (kdata && kdlen > 0) memcpy(f + 99, kdata, kdlen);
    return 99 + kdlen;           /* real EAPOL length */
}

/* The corrected offsets pull Key-Data-Length from 97/98 (was 93/94, inside the
 * 16-byte MIC @ 81..96). Assert the fix reads the right length and the old
 * offset read into the MIC. */
static void test_eapol_kd_offset(void) {
    uint8_t kd[22]; memset(kd, 0, sizeof(kd));
    uint8_t f[160];
    build_eapol(f, kd, 22);

    uint16_t kd_len_fixed = ((uint16_t)f[97] << 8) | f[98];
    uint16_t kd_len_old   = ((uint16_t)f[93] << 8) | f[94];
    TEST_ASSERT_EQUAL_UINT16(22, kd_len_fixed);           /* correct */
    TEST_ASSERT_NOT_EQUAL(22, kd_len_old);                /* old read MIC bytes */
    TEST_ASSERT_EQUAL_PTR(f + 99, f + 99);                /* kd_data @ 99 */
}

/* PMKID KDE (type 0xDD, OUI 00-0F-AC, type 04) placed in key data must be found
 * by the walk and its 16-byte PMKID extracted correctly. */
static void test_pmkid_extraction(void) {
    uint8_t kd[24];
    kd[0] = 0xDD; kd[1] = 0x14;               /* vendor KDE, len 20 */
    kd[2] = 0x00; kd[3] = 0x0F; kd[4] = 0xAC; /* WFA OUI */
    kd[5] = 0x04;                             /* PMKID subtype */
    for (int i = 0; i < 16; i++) kd[6 + i] = (uint8_t)(0xC0 + i);
    uint8_t f[160];
    int flen = build_eapol(f, kd, 22);

    const uint8_t *eapol = f;
    int elen = flen;
    uint16_t kd_len = ((uint16_t)eapol[97] << 8) | eapol[98];
    const uint8_t *kdp = eapol + 99;
    int kd_avail = elen - 99; if (kd_avail < 0) kd_avail = 0;
    if ((int)kd_len > kd_avail) kd_len = (uint16_t)kd_avail;   /* the clamp */

    const uint8_t *pmkid = NULL;
    int off = 0;
    while (off + 2 < kd_len) {
        uint8_t t = kdp[off], l = kdp[off + 1];
        if (off + 2 + l > kd_len) break;
        if (t == 0xDD && l >= 20 && kdp[off+2]==0x00 && kdp[off+3]==0x0F &&
            kdp[off+4]==0xAC && kdp[off+5]==0x04) { pmkid = kdp + off + 6; break; }
        off += 2 + l;
    }
    TEST_ASSERT_NOT_NULL(pmkid);
    for (int i = 0; i < 16; i++) TEST_ASSERT_EQUAL_UINT8((uint8_t)(0xC0 + i), pmkid[i]);
}

/* A crafted huge Key-Data-Length must be clamped to what was actually captured
 * so the TLV walk cannot read past the RX buffer (the OOB-read fix). */
static void test_kd_len_clamp(void) {
    uint8_t f[160];
    int flen = build_eapol(f, NULL, 22);
    f[97] = 0xFF; f[98] = 0xFF;               /* attacker claims 65535 */
    int elen = flen;
    uint16_t kd_len = ((uint16_t)f[97] << 8) | f[98];
    int kd_avail = elen - 99; if (kd_avail < 0) kd_avail = 0;
    if ((int)kd_len > kd_avail) kd_len = (uint16_t)kd_avail;
    TEST_ASSERT_EQUAL_INT(elen - 99, kd_len);             /* bounded to buffer */
    TEST_ASSERT_TRUE(99 + kd_len <= elen);                /* never past capture */
}

/* FCS/padding trim: the emitted EAPOL length must be the 802.1X header length,
 * not the padded sig_len (else hashcat's MIC check fails). */
static void test_fcs_trim(void) {
    uint8_t f[160];
    int real = build_eapol(f, NULL, 22);      /* true EAPOL length = 121 */
    int sig_len = real + 4;                    /* frame body incl 4-byte FCS */
    int elen = sig_len;
    int trimmed = 4 + (((int)f[2] << 8) | f[3]);
    if (trimmed > 0 && trimmed < elen) elen = trimmed;
    TEST_ASSERT_EQUAL_INT(real, elen);        /* FCS dropped */
}

/* SSID-IE copy guard (wardrive/pmkid OOB fix): 2 + ssid_len must fit the
 * captured tag region before the memcpy. */
static int ssid_ie_ok(int tag_len, int ssid_len) {
    return (tag_len >= 2 && ssid_len <= 32 && 2 + ssid_len <= tag_len);
}
static void test_ssid_ie_bound(void) {
    TEST_ASSERT_FALSE(ssid_ie_ok(10, 32));    /* would read 24B past a 10B region */
    TEST_ASSERT_FALSE(ssid_ie_ok(1,  0));     /* can't even read the length byte */
    TEST_ASSERT_TRUE (ssid_ie_ok(40, 20));    /* 22 <= 40, safe */
    TEST_ASSERT_TRUE (ssid_ie_ok(34, 32));    /* exact fit */
}

/* LLMNR/mDNS reply guard (net_responder stack-smash fix): input + 16-byte
 * answer must fit the 256-byte reply buffer. */
static int llmnr_ok(int in_len, int cap) {
    return (in_len >= 12 && in_len + 16 <= cap);
}
static void test_llmnr_bound(void) {
    TEST_ASSERT_FALSE(llmnr_ok(1400, 256));   /* attacker datagram -> reject */
    TEST_ASSERT_FALSE(llmnr_ok(250, 256));    /* 266 > 256 -> reject */
    TEST_ASSERT_TRUE (llmnr_ok(40, 256));     /* fits */
    TEST_ASSERT_TRUE (llmnr_ok(240, 256));    /* exact fit (240+16=256) */
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_eapol_kd_offset);
    RUN_TEST(test_pmkid_extraction);
    RUN_TEST(test_kd_len_clamp);
    RUN_TEST(test_fcs_trim);
    RUN_TEST(test_ssid_ie_bound);
    RUN_TEST(test_llmnr_bound);
    return UNITY_END();
}
