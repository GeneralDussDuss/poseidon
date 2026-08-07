/*
 * pinput - maps raw key codes to abstract UI events.
 *
 * Pure: no Arduino, no M5, no display. This is the seam that lets a board
 * with no keyboard (encoder only) drive the same screens. UI_EV_CHAR keeps
 * POSEIDON's letter-mnemonic navigation intact on boards that have keys.
 */
#pragma once

#include <stdint.h>

enum ui_event_kind_t {
    UI_EV_NONE = 0,
    UI_EV_UP,
    UI_EV_DOWN,
    UI_EV_LEFT,
    UI_EV_RIGHT,
    UI_EV_SELECT,
    UI_EV_BACK,
    UI_EV_BKSP,
    UI_EV_CHAR,
};

struct ui_event_t {
    ui_event_kind_t kind;
    char            ch;   /* valid only when kind == UI_EV_CHAR */
};

ui_event_t pinput_map(uint16_t key);
