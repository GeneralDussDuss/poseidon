#pragma once

#include <stdint.h>
#include <stddef.h>

/*
 * emv - contactless EMV (ISO14443-4) card reader.
 *
 * WHAT THIS CAN AND CANNOT GET (be honest about this in the UI):
 *
 *   PAN (card number)   YES. Tag 5A, or parsed out of Track 2 (tag 57).
 *                       Sent unauthenticated by every contactless card. This
 *                       is the actual security finding worth demonstrating.
 *   Expiry              YES. Tag 5F24, or the Track 2 YYMM field.
 *   Cardholder name     RARELY. Tag 5F20. Visa and Mastercard stripped it
 *                       from contactless years ago; expect it to be absent on
 *                       most modern cards. Amex sometimes still carries it.
 *   Transaction log     RARELY. Optional EMV feature: the AID's FCI may carry
 *                       tag 9F4D (Log Entry = SFI + record count). Most
 *                       issuers disable it entirely. When absent we say so
 *                       rather than implying a failure.
 *   CVV / CVC           NEVER. The printed code is not on the chip, and the
 *                       contactless cryptogram is dynamic per-transaction.
 *                       Nothing here can clone a card.
 *
 * The flow is the standard one:
 *   SELECT PPSE ("2PAY.SYS.DDF01") -> read candidate AIDs (tag 4F)
 *   SELECT AID                     -> FCI, maybe PDOL (9F38), maybe 9F4D
 *   GET PROCESSING OPTIONS         -> AIP + AFL
 *   READ RECORD over the AFL       -> the records holding 57 / 5A / 5F24 / 5F20
 *
 * Transport is injected so the parsing logic is testable off-device: pass a
 * function that sends a C-APDU and returns the R-APDU length (including SW1
 * SW2), or negative on failure.
 */

typedef int (*emv_apdu_fn)(const uint8_t *apdu, uint8_t apdu_len,
                           uint8_t *resp, uint16_t resp_max);

#define EMV_MAX_LOG 5

typedef struct {
    char    amount[16];      /* decimal, may be empty if not in the log format */
    char    date[12];        /* YYYY-MM-DD if present */
    char    currency[8];     /* ISO4217 alpha if resolvable, else numeric */
} emv_txn_t;

typedef struct {
    bool     valid;
    char     pan[24];        /* digits, NUL-terminated */
    char     expiry[8];      /* "MM/YY" */
    char     holder[28];     /* cardholder name, empty if the card omits it */
    char     scheme[16];     /* "Visa", "Mastercard", ... from the AID */
    uint8_t  aid[16];
    uint8_t  aid_len;

    bool     log_supported;  /* card advertised a transaction log (tag 9F4D) */
    uint8_t  txn_count;
    emv_txn_t txn[EMV_MAX_LOG];

    char     error[40];      /* human-readable failure reason when !valid */
} emv_card_t;

/* Run the full read against a card already selected as ISO14443-4.
 * Returns true if at least a PAN was recovered. Fills out->error otherwise. */
bool emv_read_card(emv_apdu_fn send, emv_card_t *out);

/* ---- exposed for host tests ---- */

/* Locate a BER-TLV tag inside a buffer, recursing into constructed templates.
 * Returns a pointer to the VALUE and writes its length, or nullptr. */
const uint8_t *emv_find_tag(const uint8_t *buf, uint16_t len,
                            uint32_t tag, uint16_t *out_len);

/* Parse a Track 2 Equivalent (tag 57) into PAN + "MM/YY". */
bool emv_parse_track2(const uint8_t *t2, uint16_t len,
                      char *pan, size_t pan_sz, char *expiry, size_t exp_sz);

/* Map an AID prefix to a scheme name. Returns "Unknown" if unrecognised. */
const char *emv_scheme_for_aid(const uint8_t *aid, uint8_t aid_len);
