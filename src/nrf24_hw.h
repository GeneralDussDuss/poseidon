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

/* Park the nRF24 OWN pins at boot: CSN high (deselected), CE low (standby).
 * Must run before the display or any other master touches the shared SPI bus.
 * Bruce does this in boards/lilygo-t-embed-cc1101/interface.cpp and POSEIDON
 * did not, which left CSN floating while the panel clocked the bus at 40 MHz -
 * the nRF24 then reads that traffic as commands and scrambles its registers. */
void  nrf24_park_boot(void);

/* Park the other chip-selects on the shared SPI bus. */
void  nrf24_park_others(void);

/* DIAGNOSTIC (temporary): raw SETUP_AW reads under three bus conditions, to
 * separate "module absent" from "bus flaky". Runs automatically whenever
 * detection fails; output goes to serial at 115200. See nrf24_hw.cpp. */
void  nrf24_diagnose(void);
