/*
 * nrf24_hw — nRF24L01+ driver for the combo RF/IR hat (Cardputer ADV).
 *
 * Hat pinout (shared SPI with SD):
 *   SCK=40  MISO=39  MOSI=14  CS=4  CE=3
 */
#pragma once

#include <Arduino.h>
#include <RF24.h>

#define NRF24_CS  4
#define NRF24_CE  3

bool  nrf24_begin(void);
void  nrf24_end(void);
bool  nrf24_is_up(void);
RF24 &nrf24_radio(void);

