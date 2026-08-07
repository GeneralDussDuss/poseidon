/*
 * encoder_tembed - rotary encoder + two buttons, decoded into the same raw
 * key codes src/input.cpp already produces, so every existing input call site
 * keeps working unmodified.
 */
#pragma once

#include <stdint.h>

#if defined(POSEIDON_BOARD_TEMBED)
void     tembed_input_begin(void);
uint16_t tembed_input_poll(void);
#endif
