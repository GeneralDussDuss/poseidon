/*
 * nfc_emv - contactless EMV bank card reader (T-Embed PN532).
 *
 * Demonstrates what a contactless payment card hands out with NO
 * authentication at all: the PAN and expiry are readable by anyone with a
 * reader in range. That is the whole point of the screen.
 *
 * What it deliberately does NOT do: there is no CVV here and no way to derive
 * one -- the printed code is not on the chip and the contactless cryptogram is
 * dynamic per transaction. Nothing this screen produces can clone a card.
 *
 * Read your own cards.
 */
#include "app.h"
#include "../theme.h"
#include "ui.h"
#include "input.h"
#include "../nfc_hw.h"
#include "../emv.h"
#include "../board/leds_tembed.h"
#include <string.h>
#include <stdio.h>

/* Adapter: emv.cpp speaks through a function pointer so its parsing can be
 * unit tested off-device; here we bind it to the PN532 transport. */
static int emv_send(const uint8_t *apdu, uint8_t len, uint8_t *resp, uint16_t max)
{
    return nfc_apdu(apdu, len, resp, max);
}

/* Render the PAN in 4-digit groups, the way it is printed on the card. */
static void draw_pan(int x, int y, const char *pan)
{
    auto &d = M5Cardputer.Display;
    char grouped[32];
    size_t g = 0;
    for (size_t i = 0; pan[i] && g < sizeof(grouped) - 2; ++i) {
        if (i && (i % 4) == 0) grouped[g++] = ' ';
        grouped[g++] = pan[i];
    }
    grouped[g] = '\0';
    d.setTextColor(T_FG, T_BG);
    d.setCursor(x, y);
    d.print(grouped);
}

static void show_card(const emv_card_t &c)
{
    auto &d = M5Cardputer.Display;
    d.fillRect(0, BODY_Y + 14, SCR_W, BODY_H - 16, T_BG);

    int y = BODY_Y + 20;

    d.setTextColor(T_ACCENT2, T_BG);
    d.setCursor(4, y); d.print(c.scheme);
    y += 16;

    draw_pan(4, y, c.pan);
    y += 16;

    d.setTextColor(T_DIM, T_BG);
    d.setCursor(4, y);
    d.printf("EXP %s", c.expiry[0] ? c.expiry : "--/--");
    y += 14;

    /* Say plainly when a field is absent rather than leaving a blank line --
     * "card omits it" is the accurate finding, not a read failure. */
    d.setTextColor(c.holder[0] ? T_FG : T_DIM, T_BG);
    d.setCursor(4, y);
    if (c.holder[0]) d.printf("%.28s", c.holder);
    else             d.print("name: not on card");
    y += 14;

    d.setTextColor(T_DIM, T_BG);
    d.setCursor(4, y);
    if (!c.log_supported)      d.print("txn log: not supported");
    else if (!c.txn_count)     d.print("txn log: empty");
    else                       d.printf("txn log: %u entries", c.txn_count);
}

void feat_nfc_emv(void)
{
    auto &d = M5Cardputer.Display;
    ui_clear_body();
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2); d.print("EMV CARD READ");
    ui_draw_footer("tap card   ESC=exit");
    leds_set_mode(LED_MODE_SCAN);

    if (!nfc_begin()) {
        d.setTextColor(T_BAD, T_BG);
        d.setCursor(4, BODY_Y + 24); d.print("PN532 not responding");
        d.setTextColor(T_DIM, T_BG);
        d.setCursor(4, BODY_Y + 38); d.print("see serial for I2C scan");
        while (input_poll() != PK_ESC) delay(20);
        nfc_end();
        return;
    }

    d.setTextColor(T_FG, T_BG);
    d.setCursor(4, BODY_Y + 24); d.print("Tap a contactless card...");

    bool shown = false;
    for (;;) {
        const uint16_t k = input_poll();
        if (k == PK_ESC) break;

        NfcTag t;
        if (!nfc_poll_tag(&t, 150)) { shown = false; delay(40); continue; }
        if (shown) { delay(60); continue; }   /* same card still in the field */
        shown = true;

        if (!nfc_tag_is_iso14443_4(&t)) {
            /* MIFARE Classic and friends are not APDU cards -- explain rather
             * than failing silently, since a transit card will land here. */
            d.fillRect(0, BODY_Y + 14, SCR_W, BODY_H - 16, T_BG);
            d.setTextColor(T_WARN, T_BG);
            d.setCursor(4, BODY_Y + 24); d.print("not a payment card");
            d.setTextColor(T_DIM, T_BG);
            d.setCursor(4, BODY_Y + 40); d.printf("SAK %02X (no ISO14443-4)", t.sak);
            ui_toast("wrong card type", T_WARN, 900);
            continue;
        }

        d.fillRect(0, BODY_Y + 14, SCR_W, BODY_H - 16, T_BG);
        d.setTextColor(T_ACCENT, T_BG);
        d.setCursor(4, BODY_Y + 24); d.print("reading...");

        emv_card_t card;
        if (emv_read_card(emv_send, &card)) {
            show_card(card);
            leds_event(LED_EVENT_HIT);
            ui_toast("card read", T_GOOD, 800);
        } else {
            d.fillRect(0, BODY_Y + 14, SCR_W, BODY_H - 16, T_BG);
            d.setTextColor(T_BAD, T_BG);
            d.setCursor(4, BODY_Y + 24); d.print("read failed");
            d.setTextColor(T_DIM, T_BG);
            d.setCursor(4, BODY_Y + 40); d.printf("%.36s", card.error);
            ui_toast("failed", T_BAD, 900);
        }
    }

    leds_set_mode(LED_MODE_IDLE);
    nfc_end();
}
