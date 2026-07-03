#pragma once
#include <stdint.h>
#include "ctaphid_dispatch.h"

// FIDO USB HID transport for KERBEROS. Brings up a single FIDO HID interface
// (usage page 0xF1D0) over the Arduino TinyUSB stack, buffers inbound 64-byte
// reports, and pumps them through the CTAPHID dispatcher from the main loop so
// the (blocking) user-presence prompt never stalls the USB task.

// Register the U2F message handler before begin().
void kerberos_hid_set_msg_handler(ctaphid_msg_fn fn, void *ctx);

// Add the FIDO device and start USB. Call once on entering KERBEROS mode.
void kerberos_hid_begin(void);

// Drain buffered inbound reports through the dispatcher. Call every main loop.
void kerberos_hid_poll(void);

// True once the host has enumerated the interface.
bool kerberos_hid_ready(void);

// Send a CTAPHID KEEPALIVE on the most recent channel (status 2 = up needed).
// Called repeatedly by the presence UI so the host does not time out.
void kerberos_hid_keepalive(uint8_t status);
