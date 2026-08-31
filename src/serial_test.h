/*
 * serial_test — background task that accepts test-harness commands over
 * USB serial. Lets a host orchestrator drive the device through every
 * feature in sequence and watch for crashes / freezes / regressions.
 *
 * Wire format (newline-terminated ASCII):
 *   K<hex4>   inject keypress code into input_poll queue. e.g. K001B = ESC
 *   S         dump runtime state (heap, uptime, idle, current feat)
 *   R         soft reset (ESP.restart)
 *   T<suite>  run a radio self-test suite (W B C N L A) - see selftest.h
 *   ?         banner with version
 *
 * Replies are prefixed with "[CMD]" or "[STATE]" so the host can grep
 * cleanly past the normal log noise. Cost: one task, 4 KB stack, polls
 * Serial.available every 20 ms.
 */
#pragma once

void serial_test_init(void);
