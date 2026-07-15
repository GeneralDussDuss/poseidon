/*
 * cc1101_hw.cpp — CC1101 init/teardown for Hydra RF Cap 424.
 */
#include "cc1101_hw.h"
#include "nrf24_hw.h"   /* NRF24_CS — deselect the nRF24 while CC1101 owns the bus */
#include "sd_helper.h"
#include "gps.h"
#include <ELECHOUSE_CC1101_SRC_DRV.h>
#include <SD.h>

static bool s_up = false;

void cc1101_park_others(void)
{
    /* On the combo hat CC1101 CS=15 and GDO0=13, which are exactly the GPS
     * UART pins (RX=15, TX=13). A running GPS poller fights us for them —
     * symptoms range from "CS never asserts" to "garbage bytes on SPI" — so
     * tear GPS down first. (POS-AUDIT-244 / rf-015: only if actually up;
     * unconditional teardown momentarily backdrove the GPS TX into us.) */
    if (gps_is_up()) gps_end();

    /* Hold every other device on the shared HSPI bus deselected so CC1101
     * owns it. Driven from the canonical macros (SD_CS, NRF24_CS) so this
     * can't drift out of sync with the hat pinout — the old hard-coded 6/5
     * were the Hydra nRF24-CS / LoRa-NSS pins and left the hat's real nRF24
     * CS (4) un-parked (latent MISO contention). CC1101's own CS/GDO0 are
     * set up in cc1101_begin. */
    pinMode(SD_CS,    OUTPUT); digitalWrite(SD_CS,    HIGH);
    pinMode(NRF24_CS, OUTPUT); digitalWrite(NRF24_CS, HIGH);
}

bool cc1101_begin(float freq_mhz)
{
    if (s_up) cc1101_end();
    cc1101_park_others();

    /* Reuse the SD's HSPI instance (pins 40/39/14 are shared). With
     * the bmorcelli fork of the ELECHOUSE lib we can pass that
     * instance in via setSPIinstance so the lib skips its own
     * SPI.begin() — which on Arduino-ESP32 3.x put pins into SPI
     * peripheral mode then immediately digitalWrite/Read them, logging
     * "IO X is not set as GPIO" errors and spinning forever in
     * Reset()'s MISO-wait loop. By handing the library the SD SPI
     * instance (already initialised) the pin-mode conflict disappears. */
    ELECHOUSE_cc1101.setSPIinstance(&sd_get_spi());
    pinMode(CC1101_CS, OUTPUT); digitalWrite(CC1101_CS, HIGH);
    ELECHOUSE_cc1101.setSpiPin(40, 39, 14, CC1101_CS);
    /* Do NOT call setGDO() — it sets GDO0 to OUTPUT which blocks the
     * CC1101's data signal. The official RCSwitch example skips it.
     * GDO0 must be INPUT so the CC1101 drives it and RCSwitch reads. */
    /* bmorcelli fork's Init() returns bool — false means Reset()'s
     * MISO-wait loop bailed (SPI bus likely wedged). HEAD code ignored
     * the return value, so a half-init chip could slip through and
     * silently corrupt subsequent register writes. */
    if (!ELECHOUSE_cc1101.Init()) {
        Serial.println("[cc1101] Init() failed — SPI bus likely wedged");
        return false;
    }
    delay(10);
    pinMode(CC1101_GDO0, INPUT);  /* CC1101 drives this pin, we read it */

    /* Belt-and-suspenders: read PARTNUM/VERSION registers. Catches a
     * working SPI bus + no chip on the other end (wrong hat / loose
     * connector / dead CC1101). */
    if (!ELECHOUSE_cc1101.getCC1101()) {
        Serial.println("[cc1101] chip not detected — wrong hat?");
        return false;
    }

    /* Tuned for car keys / garage remotes — Flipper's "AM650" preset
     * which covers the vast majority of 315/433 MHz OOK fobs. Prior
     * values (RxBW 256, DRate 50) were too narrow + too fast: the
     * demod filter ate the slow (~2-5 kbps) pulses that car remotes
     * emit, so GDO0 never transitioned even though RSSI tracked the
     * burst. RxBW 650 + DRate 3.794 matches Flipper's capture range. */
    ELECHOUSE_cc1101.setModulation(2);          /* ASK/OOK */
    ELECHOUSE_cc1101.setMHZ(freq_mhz);
    ELECHOUSE_cc1101.setRxBW(650);              /* AM650 — wide enough for car fobs */
    ELECHOUSE_cc1101.setClb(1, 13, 15);         /* VCO calibration (Bruce) */
    ELECHOUSE_cc1101.setClb(2, 16, 19);
    ELECHOUSE_cc1101.setDRate(3.794);           /* AM650 data rate */
    ELECHOUSE_cc1101.setPktFormat(3);           /* async serial on GDO0 */
    ELECHOUSE_cc1101.SetRx();

    /* In async serial mode (PKT_FORMAT=3), GDO0 is the raw data line
     * regardless of IOCFG0 setting. Ensure pin stays INPUT. */
    pinMode(CC1101_GDO0, INPUT);

    s_up = true;
    Serial.printf("[cc1101] up @ %.3f MHz\n", freq_mhz);
    return true;
}

void cc1101_end(void)
{
    if (!s_up) return;
    ELECHOUSE_cc1101.setSidle();
    ELECHOUSE_cc1101.goSleep();
    /* POS-AUDIT-012: restore GDO0 to INPUT (was OUTPUT for some TX paths)
     * and float CS so the next HSPI user sees a clean bus. Other parked
     * CS lines (SD=12, nRF24=6, LoRa=5) are released by their owners. */
    pinMode(CC1101_GDO0, INPUT);
    pinMode(CC1101_CS, INPUT);
    s_up = false;
}

bool cc1101_is_up(void)    { return s_up; }
void cc1101_set_freq(float mhz) { ELECHOUSE_cc1101.setMHZ(mhz); }
void cc1101_set_rx(void)   { ELECHOUSE_cc1101.SetRx(); }
void cc1101_set_tx(void)   { ELECHOUSE_cc1101.SetTx(); }
void cc1101_set_idle(void) { ELECHOUSE_cc1101.setSidle(); }

int cc1101_get_rssi(void)
{
    int raw = ELECHOUSE_cc1101.getRssi();
    return raw;
}
