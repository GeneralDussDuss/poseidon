/*
 * lora_hw — thin wrapper over RadioLib's SX1262 for the CAP-LoRa1262.
 *
 * Cardputer-Adv pin map (per M5 docs):
 *   SX1262: NSS=G5 SCK=G40 MOSI=G14 MISO=G39 RST=G3 DIO1=G4 BUSY=G6
 *   Antenna switch: PI4IOE5V6408 @ 0x43 on internal I2C, P0 HIGH enables.
 *
 * Shares the SD SPI bus via sd_get_spi() — features run one at a time
 * so no mutex is needed.
 */
#pragma once

#include <RadioLib.h>

/* Preset bands. The SX1262 itself spans 150-960 MHz, but hat antenna
 * is tuned 868-923. 433 MHz included for opportunistic RX even though
 * the antenna is off-band. */
enum lora_band_t {
    LORA_BAND_433 = 0,
    LORA_BAND_868,
    LORA_BAND_915,
    LORA_BAND_MESHTASTIC_US,  /* 906.875 MHz, Meshtastic LongFast US */
    LORA_BAND__COUNT
};

struct lora_config_t {
    float   freq_mhz;
    float   bw_khz;
    uint8_t sf;
    uint8_t cr;     /* coding rate 4/X, X in 5..8 */
    uint8_t sync;   /* sync word */
    int8_t  power;  /* dBm, -9..+22 */
};

lora_config_t lora_preset(lora_band_t b);
const char *  lora_band_name(lora_band_t b);

/* Bring SX1262 out of reset, init RadioLib, apply config, enable the
 * PI4IOE antenna switch. Returns 0 on success, RadioLib status on fail. */
int  lora_begin(const lora_config_t &cfg);
void lora_end(void);
bool lora_is_up(void);

/* Raw access for features that want to call into RadioLib directly. */
SX1262 &lora_radio(void);
