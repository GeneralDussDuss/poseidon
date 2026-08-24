/*
 * ir_hw — IR pin map for the combo RF/IR hat (Cardputer ADV).
 * TX moved off the built-in G44 LED onto the hat's emitter (G6);
 * RX (G5) is new. Polarity of the hat emitter is unverified — if TX is
 * dead on-device, set IR_TX_ACTIVE_LOW to 0.
 *
 * T-Embed's G6/G5 are the rotary encoder's back button and B phase, so
 * the Cardputer hat values would fight the encoder there. Use the
 * T-Embed's own IR pins instead (per Bruce's lilygo-t-embed-cc1101
 * pins_arduino.h): TX=2, RX=1.
 */
#pragma once

#if defined(POSEIDON_BOARD_TEMBED)
#define IR_TX_PIN        2
#define IR_RX_PIN        1
#else
#define IR_TX_PIN        6
#define IR_RX_PIN        5
#endif
/* TX drive polarity is a PER-BOARD property and must live inside the board gate.
 *
 * It was hardcoded to active-LOW for both targets. On the T-Embed that is
 * backwards: LilyGO's own pin map (mirrored in Bruce's
 * boards/lilygo-t-embed-cc1101/pins_arduino.h -- TXLED 2, LED_ON HIGH,
 * LED_OFF LOW) makes GPIO2 active-HIGH, and Bruce drives IRsend there with
 * inverted=false. Getting this wrong does not merely invert a bit: the 38 kHz
 * mark is a symmetric square wave so it still carries under either polarity,
 * but every SPACE and the post-transmit park level are flipped, so the emitter
 * sits fully forward-biased through every gap and is left ON after the feature
 * exits -- tens of mA burning on a battery device until the next reboot. */
#if defined(POSEIDON_BOARD_TEMBED)
#define IR_TX_ACTIVE_LOW 0   /* GPIO2: HIGH = emitter ON (LilyGO/Bruce ground truth) */
#else
#define IR_TX_ACTIVE_LOW 1   /* hat emitter: HIGH is off */
#endif

/* Single source of truth for the drive levels. Every IR feature must use these
 * rather than open-coding HIGH/LOW, which is exactly how the polarity bug
 * spread across four files. */
#define IR_TX_ON_LEVEL   (IR_TX_ACTIVE_LOW ? LOW  : HIGH)
#define IR_TX_OFF_LEVEL  (IR_TX_ACTIVE_LOW ? HIGH : LOW)
