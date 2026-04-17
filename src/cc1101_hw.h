/*
 * cc1101_hw — CC1101 driver for the PINGEQUA Hydra RF Cap 424.
 *
 * Hydra hat pinout (shared SPI with SD):
 *   SCK=40  MISO=39  MOSI=14  CS=13  GDO0=5
 */
#pragma once

#include <Arduino.h>

#define CC1101_CS   13
#define CC1101_GDO0  5

bool cc1101_begin(float freq_mhz = 433.92f);
void cc1101_end(void);
bool cc1101_is_up(void);
void cc1101_set_freq(float mhz);
void cc1101_set_rx(void);
void cc1101_set_tx(void);
void cc1101_set_idle(void);
int  cc1101_get_rssi(void);

/* Park CS lines for other SPI peripherals HIGH before CC1101 SPI ops. */
void cc1101_park_others(void);
