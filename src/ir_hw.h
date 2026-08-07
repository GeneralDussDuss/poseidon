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
#define IR_TX_ACTIVE_LOW 1   /* 1 = HIGH is off (built-in behaviour); flip if hat emitter is active-HIGH */
