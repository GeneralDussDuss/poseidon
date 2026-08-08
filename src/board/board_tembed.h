/*
 * board_tembed - pin map for the LilyGO T-Embed CC1101.
 *
 * Values verified against Bruce's boards/lilygo-t-embed-cc1101/pins_arduino.h,
 * which drives this exact board. Do not guess these; a wrong pin here is hours
 * of silent bring-up failure.
 */
#pragma once

#if defined(POSEIDON_BOARD_TEMBED)

/* Power gate: must be HIGH before panel, CC1101 or LEDs respond. */
#define TE_PIN_POWER_ON   15

/* Shared SPI bus. */
#define TE_SPI_MOSI        9
#define TE_SPI_MISO       10
#define TE_SPI_SCK        11
/* 40MHz is LilyGO's own shipped value (Setup214...h:53). Bruce runs this
 * board at 80MHz and POSEIDON copied that; on a bus shared with SD, CC1101
 * and nRF24 the faster clock is a plausible source of display corruption,
 * so prefer the vendor's number. */
#define TE_SPI_FREQ_WRITE 40000000
#define TE_SPI_FREQ_READ  20000000

/* ST7789 panel. Physical portrait 170x320; we run rotation 3 for landscape. */
#define TE_TFT_CS         41
#define TE_TFT_DC         16
/* This panel has no reset line — LilyGO's own utilities.h sets
 * DISPLAY_RST -1 for this board; the panel reset is tied to the power
 * rail (TE_PIN_POWER_ON), not a GPIO. GPIO 40 is the I2S word clock
 * (see TE_I2S_WS below) — it was wrongly reused here as a "reset" pin,
 * which fights the speaker for the same pin the moment audio runs and
 * corrupts the display. -1 tells LovyanGFX there is no reset pin. */
/* -1, per LilyGO's OWN sources: examples/utilities.h:40, the shipped
 * Setup214_LilyGo_T_Embed_PN532.h:27, and README.md:217 all specify no
 * reset pin. The schematic does tie LCD reset to GPIO40, but GPIO40 is
 * also the I2S word clock, and LilyGO deliberately never drives it as a
 * reset because of that conflict. This flip-flopped once: an audit
 * comparing against Bruce recommended 40, but Bruce is a third party and
 * LilyGO is not. */
#define TE_TFT_RST        -1
#define TE_TFT_BL         21
#define TE_PANEL_W       170
#define TE_PANEL_H       320
#define TE_PANEL_ROT       3
#define TE_PANEL_INVERT true

/* ---- radios and peripherals ----
 * All values from LilyGO's Xinyuan-LilyGO/T-Embed-CC1101 repo (commit
 * 6d9df88): examples/utilities.h and docs/pinmap_cn.md, cross-referenced
 * against the schematic. The Plus SKU differs from the plain CC1101 only
 * by the nRF24 module being populated; the pinout is identical. */

/* CC1101 sub-GHz. SW0/SW1 drive the antenna band-select network. */
#define TE_CC1101_CS      12
#define TE_CC1101_GDO0     3
#define TE_CC1101_GDO2    38
#define TE_CC1101_SW1     47
#define TE_CC1101_SW0     48

/* nRF24L01. Populated on the Plus. NOTE: 43/44 are triple-booked with
 * UART0 and the external UART header, so serial logging and nRF traffic
 * can interfere. LilyGO issue #79 also reports Plus units losing nRF range
 * over time. */
#define TE_NRF24_CS       44
#define TE_NRF24_CE       43
#define TE_NRF24_IRQ      -1   /* not wired on this board */

/* PN532 NFC, on I2C (not SPI). */
#define TE_NFC_SDA         8
#define TE_NFC_SCL        18
#define TE_NFC_RF_RST     45
#define TE_NFC_IRQ        17
#define TE_NFC_I2C_ADDR 0x24

/* IR transmit / receive. */
#define TE_IR_TX           2
#define TE_IR_RX           1

/* SD card, sharing the main SPI bus. */
#define TE_SD_CS          13

/* PDM microphone. */
#define TE_MIC_DATA       42
#define TE_MIC_CLK        39

/* Power management over I2C. */
#define TE_BQ27220_ADDR 0x55   /* fuel gauge */
#define TE_BQ25896_ADDR 0x6B   /* charger */

/* Rotary encoder + buttons, all active LOW.
 * NOTE: TE_BTN_SELECT is GPIO0, which is also the BOOT strap pin. Holding
 * it during reset enters the bootloader rather than registering a press. */
#define TE_ENC_A           4
#define TE_ENC_B           5
#define TE_BTN_SELECT      0
#define TE_BTN_BACK        6

/* WS2812B RGB LED ring around the rotary encoder. */
#define TE_LED_PIN        14
#define TE_LED_COUNT       8

/* NS4168 I2S speaker (mono, class-D). */
#define TE_I2S_BCLK       46   /* bit clock */
#define TE_I2S_WS         40   /* word/select clock — NOT a display pin, see TE_TFT_RST above */
#define TE_I2S_DOUT        7   /* data out */

#endif /* POSEIDON_BOARD_TEMBED */
