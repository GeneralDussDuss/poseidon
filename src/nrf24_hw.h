/*
 * nrf24_hw — nRF24L01+ driver for the PINGEQUA Hydra RF Cap 424.
 *
 * Hydra hat pinout (shared SPI with SD):
 *   SCK=40  MISO=39  MOSI=14  CS=6  CE=4
 */
#pragma once

#include <Arduino.h>
#include <RF24.h>

#define NRF24_CS  6
#define NRF24_CE  4

bool  nrf24_begin(void);
void  nrf24_end(void);
bool  nrf24_is_up(void);
RF24 &nrf24_radio(void);

void nrf24_park_others(void);
