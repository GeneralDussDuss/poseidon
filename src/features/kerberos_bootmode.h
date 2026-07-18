#pragma once

// KERBEROS USB HID personality, chosen at boot.
//
// The Arduino USB stack exposes a single HID interface shared by every added
// device. A FIDO authenticator must be the SOLE HID device (no report ids,
// raw 64-byte reports) for the OS to recognise it, which collides with the
// BadUSB keyboard. So we pick one personality per boot: normal (keyboard) or
// key mode (FIDO only). The choice lives in RTC memory and survives the soft
// reset used to switch, so entering/leaving KERBEROS reboots into the right
// personality. Both modes keep the hardware serial port, so flashing works in
// either (flash from normal mode for the smoothest auto-reset).

bool kerb_boot_key_mode(void);

// Persist the desired personality and restart into it. Does not return.
void kerb_request_key_mode(bool on);

// Call FIRST in setup(): re-assert key mode from NVS after a cold boot
// (unplug/reinsert), so the OS FIDO reset flow keeps the FIDO device present.
// May reboot once (does not return in that case).
void kerb_boot_sync(void);
