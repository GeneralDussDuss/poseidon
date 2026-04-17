/*
 * subghz_decode.h — OOK protocol decoder for raw pulse timings.
 *
 * Decodes: Princeton, CAME, NICE, Linear, Chamberlain, Holtek,
 * GateTX, Dooya, and generic fixed-code remotes.
 */
#pragma once

#include <Arduino.h>

struct subghz_decoded_t {
    const char *protocol;
    uint32_t    value;
    uint8_t     bits;
    int         repeat;
    bool        valid;
};

subghz_decoded_t subghz_decode(const int16_t *pulses, int count);
