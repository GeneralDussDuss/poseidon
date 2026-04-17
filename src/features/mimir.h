/*
 * mimir.h — MIMIR pentest drop-box control client.
 *
 * Drives a Banana Pi BPI-M4 Zero over USB-C to USB-C using
 * newline-delimited JSON over USB-CDC.
 */
#pragma once

void feat_mimir(void);

/* Set true while MIMIR owns the USB-CDC link. Other features that
 * need USB (BadUSB HID) should check this and bail. */
extern bool g_mimir_cdc_active;
