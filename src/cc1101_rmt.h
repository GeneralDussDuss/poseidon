/*
 * cc1101_rmt — new-API RMT helper for CC1101 OOK TX/RX.
 *
 * Replaces the legacy driver/rmt.h path that conflicts with IDF 5.5's
 * driver_ng at boot. Uses rmt_tx.h / rmt_rx.h directly.
 *
 * Pulse convention (matches the existing Flipper .sub RAW_Data format):
 *   int16_t values, microseconds per pulse, sign = line level.
 *     positive = HIGH  (carrier ON  / mark)
 *     negative = LOW   (carrier OFF / space)
 *
 * The CC1101 must already be configured for async OOK serial TX
 * (ELECHOUSE_cc1101 library does this by default) with GDO0 wired to
 * the chip's async data line. We just drive GDO0 precisely from RMT.
 *
 * Both calls own channel/encoder lifecycle internally — fine for
 * one-shot TX or a single recording window. Call repeatedly if needed.
 */
#pragma once

#include <Arduino.h>

/* Transmit a pulse array over GDO0.
 * Returns true on success, false on hardware failure. Blocks until the
 * full train has been transmitted. */
bool cc1101_rmt_tx(const int16_t *pulses, int n_pulses);

/* Capture pulses from GDO0 into an output buffer.
 *   timeout_ms — total receive window (how long to wait for a signal)
 *   gap_us     — any silence longer than this ends the capture (typ 10000)
 * Returns number of pulses captured, or 0 on timeout / hw failure. */
int cc1101_rmt_rx(int16_t *out_pulses, int max_pulses,
                  uint32_t timeout_ms, uint32_t gap_us);
