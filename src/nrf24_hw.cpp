/*
 * nrf24_hw.cpp — nRF24L01+ init/teardown.
 *
 * Cardputer: Hydra RF Cap 424 hat.
 * T-Embed:   module populated on the CC1101 "Plus" SKU, CE=43 / CS=44.
 */
#include "nrf24_hw.h"
#include "cc1101_hw.h"
#include "sd_helper.h"
#include <SPI.h>

#if defined(POSEIDON_BOARD_TEMBED)
#include "board/board_tembed.h"
#endif

static RF24 *s_radio = nullptr;
static bool  s_up    = false;

void nrf24_park_boot(void)
{
    /* CSN high = deselected, CE low = standby. Bruce parks these in its board
     * interface before anything else runs; POSEIDON never did. With CSN left
     * floating the nRF24 can consider itself selected and execute the display's
     * bus traffic as SPI commands, corrupting its own configuration. The chip
     * keeps that state across an ESP32 reset because board power never drops,
     * so a scrambled part only recovers on a full power cycle. */
    pinMode(NRF24_CS, OUTPUT); digitalWrite(NRF24_CS, HIGH);
    pinMode(NRF24_CE, OUTPUT); digitalWrite(NRF24_CE, LOW);
}

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

    /* Use the SD's SPI instance. Global SPI is the display's on both boards -
     * calling SPI.begin() there steals the GPIO matrix from the panel on every
     * nRF24 op and the screen flickers or freezes. Mirror CC1101's pattern and
     * reuse the already-initialised sd_get_spi() instance instead. */
    SPIClass &bus = sd_get_spi();

    /* QUIESCE THE CC1101 FIRST. This is the actual fix for the long-running
     * "nRF24 detected intermittently" fault on this board.
     *
     * The CC1101's SO pin is also its GDO1, and on the T-Embed it hangs on the
     * SAME MISO net as the nRF24 and the SD card. Unless GDO1 is configured
     * 3-state (IOCFG1 = 0x2E), the CC1101 drives that net even while its own CSn
     * is de-asserted, so it fights the nRF24's replies. That matches the measured
     * corruption exactly: 1-bits get pulled down to 0 and a 0 never rises, longer
     * and slower transactions are hit harder, and the CC1101 itself always reads
     * perfectly because it is the aggressor rather than the victim.
     *
     * cc1101_begin() issues SRES and reconfigures the part, restoring the
     * 3-state default. Measured: with the CC1101 never initialised this session,
     * nRF24 channel read-back scored 3-6/10 and detection failed about a quarter
     * of the time; ONE CC1101 init then made the next EIGHT nRF24 runs a clean
     * 10/10 - and that heal survived the ESP32 reset each of those runs performs,
     * because CC1101 register state only clears on SRES or true loss of power.
     * (This board has a battery and charger, so unplugging USB does NOT drop that
     * rail - which is why power cycling never fixed it.)
     *
     * Once per boot is therefore sufficient.
     */
    {
        static bool s_cc_quiesced = false;
        if (!s_cc_quiesced) {
            s_cc_quiesced = true;
            if (cc1101_begin(433.92f)) cc1101_end();
        }
    }

    pinMode(NRF24_CS, OUTPUT); digitalWrite(NRF24_CS, HIGH);
    pinMode(NRF24_CE, OUTPUT); digitalWrite(NRF24_CE, LOW);
    delay(5);                       /* let the pins settle before SPI traffic */

    s_radio = new RF24(NRF24_CE, NRF24_CS);

    /* Retry detection a few times. isChipConnected() is a single SETUP_AW read,
     * so one bad byte reports the chip absent.
     *
     * NOTE (2026-08-30, measured on hardware): this retry papers over a real
     * fault and cannot fix it. The part intermittently enters a state where
     * roughly half of the 1-bits in a register read come back as 0 (0-bits are
     * never wrong), which fails the seven 1-bits RF24 needs across CONFIG and
     * SETUP_AW. It correlates with flashing - the ROM bootloader drives GPIO43/44,
     * which are this board's CE/CSN - and it SURVIVES an ESP32 reset, because
     * board power never drops. Ordering against WiFi is NOT the trigger:
     * radio_switch() touches neither the nRF24's pins nor the SPI bus, and the
     * WiFi-free path fails at the same rate. Once in that state the chip has
     * been seen to stay there for many consecutive attempts.
     *
     * Root cause was the CC1101 driving GDO1 on the shared MISO net, which the
     * quiesce above fixes; this retry is only belt-and-braces. */
    bool ok = false;
    for (int tries = 0; tries < 4 && !ok; ++tries) {
        nrf24_park_others();
        if (s_radio->begin(&bus) && s_radio->isChipConnected()) ok = true;
        else delay(3);
    }

    if (!ok) {
        Serial.println("[nrf24] chip not detected");
        delete s_radio; s_radio = nullptr;
        /* CRITICAL: re-park before giving up. RF24::begin() leaves CE/CSN in
         * whatever state it reached, and nrf24_end() below cannot help because
         * s_up was never set. Leaving CSN unasserted-but-floating here lets the
         * panel's traffic on the shared SPI2 bus scramble the part exactly as
         * it did before nrf24_park_boot() existed - so ONE failed detection
         * used to poison every later attempt and the radio never came back
         * without a power cycle. Measured: 4 consecutive failures after a
         * single miss, versus clean recovery once this park is in place. */
        nrf24_park_boot();
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
    /* Park FIRST and unconditionally. The old early-return meant that calling
     * nrf24_end() after a failed begin() did nothing at all, leaving the pins
     * exposed - the caller has no way to tell the chip is unprotected. */
    if (!s_up) { nrf24_park_boot(); return; }
    if (s_radio) {
        s_radio->powerDown();
        delete s_radio;
        s_radio = nullptr;
    }
    s_up = false;

#if defined(POSEIDON_BOARD_TEMBED)
    /* Keep the part deselected and in standby. Floating CSN here is what lets
     * panel traffic on the shared bus scramble the chip - see nrf24_park_boot. */
    nrf24_park_boot();
#else
    /* Release CE/CS back to high-Z so the pins don't fight the next
     * hat (LoRa BUSY=G6 / DIO1=G4 overlap these on CAP-LoRa1262). */
    pinMode(NRF24_CS, INPUT);
    pinMode(NRF24_CE, INPUT);
#endif
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
