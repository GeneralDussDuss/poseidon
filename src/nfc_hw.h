#pragma once
#include <stdint.h>

/*
 * nfc_hw — PN532 NFC reader on the LilyGO T-Embed CC1101 (Plus).
 *
 * The PN532 hangs off a dedicated I2C bus (SDA=8, SCL=18, addr 0x24) per
 * board_tembed.h / LilyGO's Xinyuan-LilyGO/T-Embed-CC1101 repo. Self-contained
 * PN532 I2C driver (no external NFC lib), run on Wire1 so it never fights the
 * main bus. On non-T-Embed builds these are inert stubs.
 *
 * FIRST SLICE: passive ISO14443A tag read (UID + ATQA/SAK + type). Mifare block
 * read/auth and emulation are follow-ups.
 *
 * NOTE: written from the PN532 I2C spec; not yet bring-up-validated on hardware.
 */

struct NfcTag {
    uint8_t  uid[10];
    uint8_t  uid_len;
    uint8_t  sak;   // SEL_RES
    uint16_t atqa;  // SENS_RES
};

/* Bring up the I2C bus + PN532 (RF reset, GetFirmwareVersion, SAMConfig).
 * Returns false if the PN532 does not answer. */
bool nfc_begin(void);

/* Packed IC/Ver/Rev/Support from GetFirmwareVersion; 0 on failure. */
uint32_t nfc_firmware_version(void);

/* Poll once for a 106 kbps ISO14443A tag. Returns true and fills *out if a tag
 * is in the field within timeout_ms; false otherwise. */
bool nfc_poll_tag(NfcTag *out, uint16_t timeout_ms);

/* Human-readable tag family from SAK. */
const char *nfc_tag_type(const NfcTag *t);

/* MIFARE Classic. keyType: 0 = KEY A, 1 = KEY B. Authenticates one block's sector
 * with the given 6-byte key. Returns true on success.
 * GOTCHA: a FAILED auth deactivates the tag on the PN532 — the caller must
 * re-run nfc_poll_tag() to re-select before the next auth attempt. */
bool nfc_mifare_auth(uint8_t block, uint8_t keyType, const uint8_t key[6],
                     const uint8_t *uid, uint8_t uid_len);

/* Read one authenticated 16-byte MIFARE Classic block into out[16]. */
bool nfc_mifare_read(uint8_t block, uint8_t out[16]);

/* ---- ISO14443-4 (T=CL) APDU exchange ----
 *
 * Sends a full C-APDU to a selected ISO14443-4 card and returns the R-APDU
 * (response data + SW1 SW2). The PN532 handles T=CL framing/chaining itself
 * via InDataExchange, so callers work purely in APDU terms.
 *
 * A card supports this when its SAK has bit 5 set (0x20) -- see
 * nfc_tag_is_iso14443_4(). MIFARE Classic does NOT; it is not an APDU card.
 *
 * Returns the number of bytes written to `resp` (including the 2 status
 * bytes), or -1 on transport failure. The caller checks SW1SW2 == 0x9000.
 * `resp_max` should be at least 258 for a full 256-byte record + SW. */
int nfc_apdu(const uint8_t *apdu, uint8_t apdu_len,
             uint8_t *resp, uint16_t resp_max);

/* True if this tag speaks ISO14443-4 APDUs (EMV cards, passports, DESFire,
 * JCOP). Derived from SAK bit 5. */
bool nfc_tag_is_iso14443_4(const NfcTag *t);

void nfc_end(void);
