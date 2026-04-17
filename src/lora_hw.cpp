/*
 * lora_hw.cpp — SX1262 bring-up on the CAP-LoRa1262.
 *
 * Uses a dedicated SPIClass(HSPI) for RadioLib — NOT the SD card's
 * SPIClass, NOT the global SPI. This avoids CS pin confusion where
 * the SD's SPIClass has CS=12 stored internally and asserts it
 * during RadioLib transactions.
 */
#include "lora_hw.h"
#include "sd_helper.h"
#include <M5Unified.h>
#include <SD.h>
#include <SPI.h>

#define LORA_NSS   5
#define LORA_RST   3
#define LORA_DIO1  4
#define LORA_BUSY  6

static SPIClass loraSpi(FSPI);  /* dedicated FSPI instance for RadioLib */
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
        c.freq_mhz = 906.875f; c.bw_khz = 250.0f;
        c.sf = 11; c.cr = 5; c.sync = 0x2B; c.power = 17;
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

static void lora_rf_switch(bool on)
{
    auto &io = M5.getIOExpander(0);
    if (!io.isEnabled()) return;
    io.setDirection(0, true);
    io.digitalWrite(0, on);
}

int lora_begin(const lora_config_t &cfg)
{
    if (s_up) lora_end();

    /* Park other CS lines. */
    pinMode(12, OUTPUT); digitalWrite(12, HIGH);  /* SD */
    pinMode(13, OUTPUT); digitalWrite(13, HIGH);  /* CC1101 if present */

    /* Release from reset. */
    pinMode(LORA_NSS, OUTPUT); digitalWrite(LORA_NSS, HIGH);
    pinMode(LORA_RST, OUTPUT); digitalWrite(LORA_RST, HIGH);
    delay(20);

    /* Init FSPI for RadioLib. Don't touch SD's HSPI — let both
     * coexist on the same pins. RadioLib manages CS via digitalWrite. */
    loraSpi.begin(40, 39, 14, -1);
    delay(10);

    s_mod   = new Module(LORA_NSS, LORA_DIO1, LORA_RST, RADIOLIB_NC, loraSpi);
    s_radio = new SX1262(s_mod);

    /* Single-param begin — RadioLib handles oscillator auto-detect. */
    int st = s_radio->begin(cfg.freq_mhz);
    if (st != RADIOLIB_ERR_NONE) {
        Serial.printf("[lora] begin(%.3f) err %d\n", cfg.freq_mhz, st);
        delete s_radio; s_radio = nullptr;
        delete s_mod;   s_mod   = nullptr;
        loraSpi.end();
        return st;
    }

    /* RadioLib's SX1262 setBandwidth takes kHz directly. cfg.bw_khz is already
     * in kHz (125.0, 250.0, 500.0), so pass it unchanged. Previously it was
     * divided by 1000, passing 0.125 which isn't a valid SX1262 bandwidth —
     * config silently failed and the radio was left in a bad state that
     * crashed on the next setFrequency + startReceive cycle.
     *
     * Setter return values are logged for diagnostics but we don't tear down
     * the radio on a single setter failure — some return non-OK on certain
     * RadioLib versions (e.g. setDio2AsRfSwitch on older builds) even though
     * the hardware is configured correctly. Fail-fast here would cause
     * lora_radio() to esp_restart() once the caller dereferences the null. */
    auto log_if_err = [](const char *name, int st) {
        if (st != RADIOLIB_ERR_NONE) Serial.printf("[lora] %s -> %d\n", name, st);
    };
    log_if_err("setSpreadingFactor", s_radio->setSpreadingFactor(cfg.sf));
    log_if_err("setBandwidth",       s_radio->setBandwidth(cfg.bw_khz));
    log_if_err("setCodingRate",      s_radio->setCodingRate(cfg.cr));
    log_if_err("setSyncWord",        s_radio->setSyncWord(cfg.sync));
    log_if_err("setOutputPower",     s_radio->setOutputPower(cfg.power));
    log_if_err("setPreambleLength",  s_radio->setPreambleLength(8));
    log_if_err("setDio2AsRfSwitch",  s_radio->setDio2AsRfSwitch(true));
    lora_rf_switch(true);

    s_up = true;
    Serial.printf("[lora] up @ %.3f MHz SF%u BW%.0f\n",
                  cfg.freq_mhz, cfg.sf, cfg.bw_khz);
    return RADIOLIB_ERR_NONE;
}

void lora_end(void)
{
    if (!s_up) return;
    if (s_radio) s_radio->sleep();
    lora_rf_switch(false);
    pinMode(LORA_RST, OUTPUT);
    digitalWrite(LORA_RST, LOW);
    delete s_radio; s_radio = nullptr;
    delete s_mod;   s_mod   = nullptr;
    loraSpi.end();
    s_up = false;
}

bool lora_is_up(void) { return s_up; }

SX1262 &lora_radio(void)
{
    if (!s_radio) {
        Serial.println("[lora] BUG: lora_radio() not init");
        esp_restart();
    }
    return *s_radio;
}
