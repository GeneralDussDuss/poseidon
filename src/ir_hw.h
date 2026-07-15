/*
 * ir_hw — IR pin map for the combo RF/IR hat (Cardputer ADV).
 * TX moved off the built-in G44 LED onto the hat's emitter (G6);
 * RX (G5) is new. Polarity of the hat emitter is unverified — if TX is
 * dead on-device, set IR_TX_ACTIVE_LOW to 0.
 */
#pragma once

#define IR_TX_PIN        6
#define IR_RX_PIN        5
#define IR_TX_ACTIVE_LOW 1   /* 1 = HIGH is off (built-in behaviour); flip if hat emitter is active-HIGH */
