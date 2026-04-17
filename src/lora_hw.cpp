/*
 * lora_hw.cpp — SX1262 bring-up on the CAP-LoRa1262.
 */
#include "lora_hw.h"
#include "sd_helper.h"
#include <M5Unified.h>

/* Hat pins per M5 docs. */
#define LORA_NSS   5
#define LORA_RST   3
#define LORA_DIO1  4
#define LORA_BUSY  6

static SX1262 *s_radio = nullptr;
static Module *s_mod   = nullptr;
static bool    s_up    = false;

lora_config_t lora_preset(lora_band_t b)
{
    lora_config_t c = { 915.0f, 125.0f, 9, 7, 0x12, 10 };
    switch (b) {
    case LORA_BAND_433: c.freq_mhz = 433.0f; break;
    case LORA_BAND_868: c.freq_mhz = 868.0f; break;
    case LORA_BAND_915: c.freq_mhz = 915.0f; break;
    case LORA_BAND_MESHTASTIC_US:
        /* Meshtastic LongFast, US channel 20 default. */
        c.freq_mhz = 906.875f;
        c.bw_khz   = 250.0f;
        c.sf       = 11;
        c.cr       = 5;
        c.sync     = 0x2B;
        c.power    = 17;
        break;
    default: break;
    }
    return c;
}

const char *lora_band_name(lora_band_t b)
{
    switch (b) {
    case LORA_BAND_433:            return "433 MHz";
    case LORA_BAND_868:            return "868 MHz";
    case LORA_BAND_915:            return "915 MHz";
    case LORA_BAND_MESHTASTIC_US:  return "Mesh LF US";
    default:                       return "?";
    }
}

/* Toggle P0 on the PI4IOE5V6408 @ 0x43 that drives the hat's RF
 * antenna switch. M5Unified already configures this chip as a bulk
 * output during M5.begin(); we only need to flip P0. */
static void lora_rf_switch(bool on)
{
    auto &io = M5.getIOExpander(0);
    /* Guard: if M5Unified didn't detect the Adv board, the expander
     * object may be a dummy that crashes on register writes. Check
     * isEnabled() (I2C address != 0) before touching it. */
    if (!io.isEnabled()) {
        Serial.println("[lora] PI4IOE not available, skipping RF switch");
        return;
    }
    io.setDirection(0, true);
    io.digitalWrite(0, on);
}

int lora_begin(const lora_config_t &cfg)
{
    if (s_up) lora_end();

    /* Park ALL other CS lines HIGH so only the SX1262 responds on the
     * shared SPI bus. SD=12, and if Hydra hat were present: CC1101=13, nRF24=6. */
    pinMode(12, OUTPUT); digitalWrite(12, HIGH);
    pinMode(13, OUTPUT); digitalWrite(13, HIGH);
    pinMode(6, OUTPUT);  digitalWrite(6, HIGH);

    /* Release LoRa from reset (held LOW since boot) and park NSS. */
    pinMode(LORA_NSS, OUTPUT);
    digitalWrite(LORA_NSS, HIGH);
    pinMode(LORA_RST, OUTPUT);
    digitalWrite(LORA_RST, HIGH);
    delay(20);  /* SX1262 needs ~10ms after reset release */

    /* Pass RADIOLIB_NC for BUSY — Bruce firmware doesn't use it on
     * Cardputer either. RadioLib falls back to timing-based delays. */
    s_mod   = new Module(LORA_NSS, LORA_DIO1, LORA_RST, RADIOLIB_NC, sd_get_spi());
    s_radio = new SX1262(s_mod);

    /* Bruce firmware doesn't configure TCXO at all for the CAP-LoRa1262,
     * meaning the hat uses a plain crystal. Pass 0.0 (XTAL, no TCXO). */
    int st = s_radio->begin(cfg.freq_mhz, cfg.bw_khz, cfg.sf, cfg.cr,
                            cfg.sync, cfg.power, 8, 0.0f);
    if (st != RADIOLIB_ERR_NONE) {
        Serial.printf("[lora] begin %.3fMHz failed: %d\n", cfg.freq_mhz, st);
        delete s_radio; s_radio = nullptr;
        delete s_mod;   s_mod   = nullptr;
        return st;
    }

    /* Route DIO2 to the RF switch so TX/RX antenna path toggles
     * automatically. Also enable the hat's external antenna switch
     * via the PI4IOE I/O expander. */
    s_radio->setDio2AsRfSwitch(true);
    lora_rf_switch(true);

    s_up = true;
    Serial.printf("[lora] up @ %.3f MHz SF%u BW%.0f power=%d\n",
                  cfg.freq_mhz, cfg.sf, cfg.bw_khz, cfg.power);
    return RADIOLIB_ERR_NONE;
}

void lora_end(void)
{
    if (!s_up) return;
    if (s_radio) s_radio->sleep();
    lora_rf_switch(false);
    /* Re-assert reset so the chip is guaranteed quiet on the shared
     * SPI bus when other features use SD. */
    pinMode(LORA_RST, OUTPUT);
    digitalWrite(LORA_RST, LOW);
    delete s_radio; s_radio = nullptr;
    delete s_mod;   s_mod   = nullptr;
    s_up = false;
}

bool   lora_is_up(void) { return s_up; }

SX1262 &lora_radio(void)
{
    if (!s_radio) {
        Serial.println("[lora] BUG: lora_radio() called when not initialized");
        esp_restart();
    }
    return *s_radio;
}
