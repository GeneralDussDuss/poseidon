/*
 * nrf24_hw — nRF24L01+ driver for the combo RF/IR hat (Cardputer ADV).
 *
 * Hat pinout (shared SPI with SD):
 *   SCK=40  MISO=39  MOSI=14  CS=4  CE=3
 *
 * T-Embed has no built-in nRF24 — CS=4/CE=3 there are the rotary encoder's
 * A phase and the LoRa-hat-park pin respectively, so the Cardputer values
 * would fight the encoder. If/when an nRF24 QWIIC module is attached to a
 * T-Embed, it lands on CE=43 / CS=44 (free GPIOs, unused elsewhere).
 */
#pragma once

#include <Arduino.h>
#include <RF24.h>

#if defined(POSEIDON_BOARD_TEMBED)
#define NRF24_CS  44
#define NRF24_CE  43
#else
#define NRF24_CS  4
#define NRF24_CE  3
#endif

bool  nrf24_begin(void);
void  nrf24_end(void);
bool  nrf24_is_up(void);
RF24 &nrf24_radio(void);

