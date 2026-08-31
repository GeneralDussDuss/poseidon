/*
 * selftest — automated on-device radio verification, driven over USB serial.
 *
 * Purpose: catch the class of bug that actually breaks this firmware - a radio
 * that no longer initialises, a bus that reads garbage, a stack that regresses
 * after a refactor. Each test brings one radio up, asserts something only real
 * working hardware can produce, and tears it down.
 *
 * Wire protocol (extends serial_test.h, newline-terminated ASCII):
 *   TW   WiFi     raw-IDF scan, assert AP count > 0
 *   TB   BLE      NimBLE scan, assert advertiser count > 0
 *   TC   CC1101   PARTNUM/VERSION, RSSI floor sane, MARCSTATE transitions
 *   TN   nRF24    CSN parked, 5-byte register read-back, RX chain via RPD
 *   TZ   BLE re-init regression (BLE -> WiFi -> BLE in one boot)
 *   TL   loopback OTA: ESP32 transmits, nRF24's Received Power Detector
 *                 witnesses it. Real over-the-air proof for the 2.4 GHz group.
 *   TA   run everything in the safe order and print a summary
 *
 * Every result is one machine-readable line so a host script can parse without
 * guessing at log noise:
 *
 *   [TEST] name=wifi_scan status=PASS ms=1840 detail=aps=14
 *   [TEST] name=cc1101_id status=FAIL ms=12 detail=version=0x00 want=0x14
 *   [TESTSUM] pass=5 fail=0 skip=1
 *
 * status is PASS, FAIL or SKIP. SKIP means the radio is not present on this
 * board, never "it did not work" - a missing radio must not read as success.
 *
 * NOT covered, deliberately: CC1101 gets no over-the-air verification because
 * nothing else on this board receives sub-GHz. Its test proves the chip and SPI
 * path are healthy and the state machine moves, not that energy left the
 * antenna. Claiming otherwise would be a fake test.
 */
#pragma once

#include <stdint.h>

/* Run one suite. `which` is the protocol letter: W B C N L A. */
void selftest_run(char which);
