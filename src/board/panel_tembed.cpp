#if defined(POSEIDON_BOARD_TEMBED)

#include "panel_tembed.h"

#include <Arduino.h>
#include <M5Unified.h>

/* M5GFX.h wires up lgfx::Bus_SPI and lgfx::Light_PWM via
 * lgfx/v1/platforms/device.hpp, but lgfx::Panel_ST7789 is only pulled in by
 * M5GFX.cpp itself (for its internal autodetect probe), not exposed through
 * any public header. Include it directly. */
#include <lgfx/v1/panel/Panel_ST7789.hpp>

#include "board_tembed.h"

static lgfx::Panel_ST7789 s_panel;
static lgfx::Bus_SPI      s_bus;
static lgfx::Light_PWM    s_light;

void tembed_display_init(void) {
    /* Power rail first: nothing on this board answers until it is high. */
    pinMode(TE_PIN_POWER_ON, OUTPUT);
    digitalWrite(TE_PIN_POWER_ON, HIGH);
    delay(50);

    {
        auto cfg = s_bus.config();
        cfg.spi_host    = SPI2_HOST;
        cfg.spi_mode    = 0;
        cfg.freq_write  = TE_SPI_FREQ_WRITE;
        cfg.freq_read   = TE_SPI_FREQ_READ;
        cfg.spi_3wire   = false;
        cfg.use_lock    = true;
        cfg.dma_channel = SPI_DMA_CH_AUTO;
        cfg.pin_sclk    = TE_SPI_SCK;
        cfg.pin_mosi    = TE_SPI_MOSI;
        cfg.pin_miso    = TE_SPI_MISO;
        cfg.pin_dc      = TE_TFT_DC;
        s_bus.config(cfg);
        s_panel.setBus(&s_bus);
    }

    {
        auto cfg = s_panel.config();
        cfg.pin_cs         = TE_TFT_CS;
        cfg.pin_rst        = TE_TFT_RST;
        cfg.pin_busy       = -1;
        /* The ST7789 controller has 240x320 of GRAM but this panel is only
         * 170 wide, and the visible window sits centred in it. Without the
         * 35px x offset ((240-170)/2) everything draws into the wrong part
         * of memory: content clipped off one edge, a dead strip at the other.
         * TFT_eSPI hides this in a per-variant offset table, which is why
         * Bruce looks correct without stating it; LovyanGFX requires it. */
        cfg.panel_width    = TE_PANEL_W;   /* 170 visible */
        cfg.panel_height   = TE_PANEL_H;   /* 320 visible */
        cfg.memory_width   = 240;          /* controller GRAM, not panel */
        cfg.memory_height  = 320;
        cfg.offset_x       = 35;           /* (240 - 170) / 2 */
        cfg.offset_y       = 0;
        cfg.offset_rotation = 0;
        cfg.readable       = false;
        cfg.invert         = TE_PANEL_INVERT;
        cfg.rgb_order      = false;
        cfg.dlen_16bit     = false;
        cfg.bus_shared     = true;   /* SD and CC1101 share this bus */
        s_panel.config(cfg);
    }

    {
        auto cfg = s_light.config();
        cfg.pin_bl = TE_TFT_BL;
        s_light.config(cfg);
        s_panel.setLight(&s_light);
    }

    M5.Display.init(&s_panel);
    M5.Display.setRotation(TE_PANEL_ROT);
    M5.Display.setBrightness(160);
    M5.Display.fillScreen(0);
}

#endif /* POSEIDON_BOARD_TEMBED */
