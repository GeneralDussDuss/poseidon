/*
 * nrf24_hw.cpp — nRF24L01+ init/teardown for Hydra RF Cap 424.
 */
#include "nrf24_hw.h"
#include "cc1101_hw.h"
#include "sd_helper.h"
#include <SPI.h>

static RF24 *s_radio = nullptr;
static bool  s_up    = false;

void nrf24_park_others(void)
{
    /* Park every other CS on the shared SPI bus HIGH so only the nRF24
     * answers. SD CS=12; CC1101 CS is CC1101_CS (combo hat: 15). Use the
     * macro so this can't drift out of sync with the hat pinout again — the
     * old hard-coded 13 was the Hydra CC1101 CS and left the hat's real
     * CC1101 CS (15) un-parked, so CC1101 contended on MISO and the nRF24
     * probe read garbage ("chip not detected"). */
    pinMode(SD_CS,     OUTPUT); digitalWrite(SD_CS,     HIGH);
    pinMode(CC1101_CS, OUTPUT); digitalWrite(CC1101_CS, HIGH);
}

bool nrf24_begin(void)
{
    if (s_up) nrf24_end();
    nrf24_park_others();

    /* Use the SD's HSPI instance (pins 40/39/14). Global SPI is FSPI,
     * which M5GFX claims for the TFT — calling SPI.begin() there
     * stole the GPIO matrix from the display every nRF24 op and the
     * screen would flicker / freeze. Mirror CC1101's pattern instead:
     * reuse the already-initialised sd_get_spi() instance. */
    SPIClass &bus = sd_get_spi();
    pinMode(NRF24_CS, OUTPUT); digitalWrite(NRF24_CS, HIGH);

    s_radio = new RF24(NRF24_CE, NRF24_CS);
    if (!s_radio->begin(&bus)) {
        Serial.println("[nrf24] begin failed");
        delete s_radio; s_radio = nullptr;
        return false;
    }

    if (!s_radio->isChipConnected()) {
        Serial.println("[nrf24] chip not detected");
        delete s_radio; s_radio = nullptr;
        return false;
    }

    s_radio->setPALevel(RF24_PA_MAX);
    s_radio->setDataRate(RF24_1MBPS);
    s_radio->stopListening();

    s_up = true;
    Serial.println("[nrf24] up");
    return true;
}

void nrf24_end(void)
{
    if (!s_up) return;
    if (s_radio) {
        s_radio->powerDown();
        delete s_radio;
        s_radio = nullptr;
    }
    s_up = false;

    /* Release CE/CS back to high-Z so the pins don't fight the next
     * hat (LoRa BUSY=G6 / DIO1=G4 overlap these on CAP-LoRa1262). */
    pinMode(NRF24_CS, INPUT);
    pinMode(NRF24_CE, INPUT);
}

bool  nrf24_is_up(void) { return s_up; }

RF24 &nrf24_radio(void)
{
    if (!s_radio) {
        /* Soft fallback: return a never-begin()'d dummy instead of
         * esp_restart(). Callers are expected to check nrf24_is_up()
         * first, but if a latent path misses the check we log and
         * return a harmless-but-nonfunctional radio so the device
         * stays responsive. Same pattern as lora_radio(). */
        static RF24 dummy(NRF24_CE, NRF24_CS);
        static uint32_t last_warn = 0;
        if (millis() - last_warn > 2000) {
            Serial.println("[nrf24] nrf24_radio() called without nrf24_begin() — returning dummy");
            last_warn = millis();
        }
        return dummy;
    }
    return *s_radio;
}
