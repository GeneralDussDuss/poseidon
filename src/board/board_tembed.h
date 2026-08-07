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

#endif /* POSEIDON_BOARD_TEMBED */
