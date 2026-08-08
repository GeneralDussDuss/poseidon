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
#define TE_SPI_FREQ_WRITE 80000000
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
/* 40, not -1. An earlier change set this to -1 on the theory that GPIO 40
 * is the I2S word clock and so could not also be a panel reset. Bruce
 * drives this exact board with TFT_RST 40 and pulses it at init
 * (boards/lilygo-t-embed-cc1101/pins_arduino.h:41), so the pin is genuinely
 * shared and the reset is genuinely needed. With -1 the ST7789 is never
 * reset at boot, which leaves it in whatever state the previous firmware
 * left behind. */
#define TE_TFT_RST        40
#define TE_TFT_BL         21
#define TE_PANEL_W       170
#define TE_PANEL_H       320
#define TE_PANEL_ROT       3
#define TE_PANEL_INVERT true

/* Rotary encoder + buttons, all active LOW. */
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
