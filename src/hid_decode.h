#pragma once
#include <stdint.h>

// Portable HID report-map decoder for BLE keyboard capture (Phase 1).
// No NimBLE / Arduino dependencies: pure byte parsing so it is host-testable.

typedef struct {
    bool is_keyboard;      // true if the map describes a keyboard input report
    int  report_id;        // report ID prefix byte value, 0 if none
    int  modifier_offset;  // byte offset of the modifier bitmap, -1 if absent
    int  key_offset;       // byte offset of the first keycode slot, -1 if absent
    int  key_count;        // number of keycode slots in the input report
} hid_layout_t;

// Parse a HID Report Map into a keyboard input-report layout.
// Returns true and fills *out when the map is a usable keyboard; false otherwise.
bool hid_parse_report_map(const uint8_t *map, int len, hid_layout_t *out);

// Decode one keyboard input report into a human-readable string such as
// "LShift+A" or "Enter". Modifiers come first, then keycodes, joined by '+'.
// Returns the number of active tokens written (0 for an all-zero report).
int hid_decode_keyboard(const hid_layout_t *layout, const uint8_t *report,
                        int rlen, char *out, int out_sz);
