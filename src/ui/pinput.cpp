#include "pinput.h"

/* Raw codes mirrored from src/input.h. Redeclared here so this unit stays
 * free of Arduino headers and can compile under the native test env. */
enum : uint16_t {
    RAW_NONE  = 0,
    RAW_ENTER = 0x0D,
    RAW_ESC   = 0x1B,
    RAW_BKSP  = 0x08,
    RAW_UP    = 0x100,
    RAW_DOWN  = 0x101,
    RAW_LEFT  = 0x102,
    RAW_RIGHT = 0x103,
};

ui_event_t pinput_map(uint16_t key) {
    ui_event_t e{UI_EV_NONE, 0};
    switch (key) {
    case RAW_UP:    e.kind = UI_EV_UP;     return e;
    case RAW_DOWN:  e.kind = UI_EV_DOWN;   return e;
    case RAW_LEFT:  e.kind = UI_EV_LEFT;   return e;
    case RAW_RIGHT: e.kind = UI_EV_RIGHT;  return e;
    case RAW_ENTER: e.kind = UI_EV_SELECT; return e;
    case RAW_ESC:   e.kind = UI_EV_BACK;   return e;
    case RAW_BKSP:  e.kind = UI_EV_BKSP;   return e;
    default: break;
    }
    if (key >= 0x20 && key <= 0x7E) {
        e.kind = UI_EV_CHAR;
        e.ch   = static_cast<char>(key);
    }
    return e;
}
