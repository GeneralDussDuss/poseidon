/*
 * nrf24_hw.cpp — nRF24L01+ init/teardown for Hydra RF Cap 424.
 */
#include "nrf24_hw.h"
#include "sd_helper.h"
#include <SPI.h>

static RF24 *s_radio = nullptr;
static bool  s_up    = false;

void nrf24_park_others(void)
{
    /* SD CS=12, CC1101 CS=13: hold HIGH so they ignore SPI traffic. */
    pinMode(12, OUTPUT); digitalWrite(12, HIGH);
    pinMode(13, OUTPUT); digitalWrite(13, HIGH);
}

bool nrf24_begin(void)
{
    if (s_up) nrf24_end();
    nrf24_park_others();

    /* Use global SPI (FSPI) with explicit pin config — same approach
     * as CC1101. HSPI (SD) and FSPI share the same GPIO pins; the
     * last one to call SPI.begin() owns the GPIO matrix. */
    SPI.begin(40, 39, 14, NRF24_CS);

    s_radio = new RF24(NRF24_CE, NRF24_CS);
    if (!s_radio->begin(&SPI)) {
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
}

bool  nrf24_is_up(void) { return s_up; }

RF24 &nrf24_radio(void)
{
    if (!s_radio) {
        Serial.println("[nrf24] BUG: nrf24_radio() called when not initialized");
        esp_restart();
    }
    return *s_radio;
}
