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

/* No custom SPIClass. M5Stack's CAP-LoRa1262 reference and d4rkmen/plai's
 * Cardputer-Adv Meshtastic port both rely on the global Arduino SPI object
 * that M5Unified sets up during M5.begin() — the hat's bus is already
 * correctly pinned via M5Unified's board pinmap. Creating a second
 * SPIClass(FSPI) hijacks SPI2 from M5GFX (display freezes). HSPI also
 * fails because RadioLib's Module without a BUSY pin spins on startReceive.
 * The fix is to do neither: use RadioLib's default SPI + pass BUSY. */
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

/*
 * Drive the PI4IOE5V6408 antenna switch at I2C 0x43 directly.
 *
 * Previously this called M5.getIOExpander(0) — but nothing in POSEIDON
 * ever registers an IOExpander with M5Unified, so that API returns a
 * reference to an uninitialized slot and the first method call on it
 * LoadProhibited-panics the whole device (Core 1, hard restart via
 * Guru Meditation). Direct I2C is both safer and doesn't depend on
 * M5Unified internals.
 *
 * PI4IOE5V6408 register map (datasheet):
 *   0x01 I/O direction  (bit N: 1=input, 0=output)
 *   0x05 Output register (bit N: 1=high, 0=low)
 * GPIO 0 is the antenna switch enable on the CAP-LoRa1262 hat.
 */
#define PI4IOE_ADDR    0x43
#define PI4IOE_REG_DIR 0x01
#define PI4IOE_REG_OUT 0x05

/* Use M5.In_I2C — the Cardputer's internal I2C already configured by
 * M5.begin(). Raw Wire isn't bound to the right SDA/SCL on this board. */
static bool pi4ioe_write(uint8_t reg, uint8_t val)
{
    return M5.In_I2C.writeRegister8(PI4IOE_ADDR, reg, val, 400000);
}

static bool pi4ioe_present(void)
{
    return M5.In_I2C.scanID(PI4IOE_ADDR);
}

static void lora_rf_switch(bool on)
{
    if (!pi4ioe_present()) {
        Serial.println("[lora] PI4IOE antenna switch not present at 0x43 (I2C scan)");
        return;
    }
    /* Bit 0 = GPIO 0 = antenna switch. Configure as output first. */
    pi4ioe_write(PI4IOE_REG_DIR, 0xFE);  /* GPIO 0 output, rest inputs */
    pi4ioe_write(PI4IOE_REG_OUT, on ? 0x01 : 0x00);
}

int lora_begin(const lora_config_t &cfg)
{
    if (s_up) lora_end();

    /* Make sure the shared HSPI bus is actually up before we hand
     * sd_get_spi() to RadioLib. sd_mount() is idempotent — if SD was
     * already mounted at boot this is a no-op; if boot-time mount was
     * skipped (or failed and left sd_spi in a partial state) this
     * guarantees sd_spi.begin(40,39,14,-1) has been run, which is
     * what RadioLib needs to talk to the SX1262. */
    sd_mount();

    /* Park other CS lines. */
    pinMode(12, OUTPUT); digitalWrite(12, HIGH);  /* SD */
    pinMode(13, OUTPUT); digitalWrite(13, HIGH);  /* CC1101 if present */

    /* Release from reset. */
    pinMode(LORA_NSS, OUTPUT); digitalWrite(LORA_NSS, HIGH);
    pinMode(LORA_RST, OUTPUT); digitalWrite(LORA_RST, HIGH);
    delay(20);

    /* Antenna switch ON before radio.begin — per M5Stack + plai reference,
     * RF front-end enable must be high before the chip is initialised. */
    lora_rf_switch(true);

    /* 5-arg Module: (cs, irq=DIO1, rst, busy, spi). BUSY is MANDATORY —
     * leaving it RADIOLIB_NC makes RadioLib spin on startReceive.
     *
     * SPI bus: share the SD helper's HSPI (SPI3), already initialized
     * during boot on the correct pins 40/39/14. The two peripherals
     * share the bus and are selected by their independent CS lines
     * (SD CS=12, LoRa NSS=5). This avoids two problems we hit:
     *   a) Default global Arduino `SPI` isn't pinned to 40/39/14 on
     *      Cardputer-Adv, so radio.begin() returns CHIP_NOT_FOUND (-2).
     *   b) Creating our own SPIClass(FSPI) hijacks SPI2 from M5GFX and
     *      freezes the TFT on the next draw. */
    s_mod   = new Module(LORA_NSS, LORA_DIO1, LORA_RST, LORA_BUSY, sd_get_spi());
    s_radio = new SX1262(s_mod);

    int st = s_radio->begin(cfg.freq_mhz);
    if (st != RADIOLIB_ERR_NONE) {
        Serial.printf("[lora] begin(%.3f) err %d\n", cfg.freq_mhz, st);
        delete s_radio; s_radio = nullptr;
        delete s_mod;   s_mod   = nullptr;
        lora_rf_switch(false);
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
    /* Current limit per M5 reference — protects PA on the CAP-LoRa1262. */
    log_if_err("setCurrentLimit",    s_radio->setCurrentLimit(140));

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
    /* Release BUSY / DIO1 back to high-Z so the next radio domain
     * (or the Hydra hat's nRF24 which shares G4+G6) doesn't fight
     * an output-driven pin. SPI-claimed NSS (G5) is left; the next
     * begin() reclaims it. */
    pinMode(LORA_BUSY, INPUT);
    pinMode(LORA_DIO1, INPUT);
    s_up = false;
}

bool lora_is_up(void) { return s_up; }

/*
 * Fallback SX1262 instance used when lora_radio() is called before
 * lora_begin(). Never talks to real hardware (module pins are all -1)
 * so any register write is a RadioLib no-op or error return. This is
 * far better than esp_restart() which caused a hard boot-loop if a
 * feature accessed the radio without going through lora_begin() first.
 * Callers that care about correctness should check lora_is_up() first.
 */
static Module *s_dummy_mod = nullptr;
static SX1262 *s_dummy_radio = nullptr;

SX1262 &lora_radio(void)
{
    if (s_radio) return *s_radio;

    if (!s_dummy_radio) {
        s_dummy_mod = new Module(RADIOLIB_NC, RADIOLIB_NC, RADIOLIB_NC, RADIOLIB_NC);
        s_dummy_radio = new SX1262(s_dummy_mod);
    }
    Serial.println("[lora] WARN: lora_radio() called before lora_begin — returning dummy");
    return *s_dummy_radio;
}
