/*
 * cc1101_hw — CC1101 driver for the combo RF/IR hat (Cardputer ADV).
 *
 * Hat pinout (shared SPI with SD):
 *   SCK=40  MISO=39  MOSI=14  CS=15  GDO0=13
 */
#pragma once

#include <Arduino.h>

#if defined(POSEIDON_BOARD_TEMBED)
/* LilyGO T-Embed CC1101 / CC1101 Plus. Values from LilyGO's own
 * utilities.h + docs/pinmap_cn.md, not inferred. SW0/SW1 select the
 * antenna band network and MUST be driven when retuning across bands,
 * or high-band scans get measured through the low-band path. */
#define CC1101_CS   12
#define CC1101_GDO0  3
#define CC1101_GDO2 38
#define CC1101_SW1  47
#define CC1101_SW0  48
#else
#define CC1101_CS   15
#define CC1101_GDO0 13
#endif

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
void cc1101_diag(void);   /* TEMP: chip-ID + live RSSI probe over serial */
