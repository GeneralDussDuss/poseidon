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

    /* Read PARTNUM (0x30) + VERSION (0x31) and verify them STRICTLY. The
     * library's getCC1101() accepts any nonzero VERSION (`if val>0`), so a
     * wrong CS pin — MISO floats high, every read is 0xFF, 0xFF>0 — passes as
     * "detected". The chip then reads 0xFF RSSI forever (a constant ~-74 dBm),
     * which is exactly the "waterfall renders but never varies" symptom. A real
     * CC1101 answers PARTNUM=0x00, VERSION=0x14 (some 0x17). Anything else means
     * the hat isn't talking — fail loudly with the actual bytes so it's obvious. */
    uint8_t part = ELECHOUSE_cc1101.SpiReadStatus(0x30);
    uint8_t ver  = ELECHOUSE_cc1101.SpiReadStatus(0x31);
    if (!(part == 0x00 && (ver == 0x14 || ver == 0x17))) {
        Serial.printf("[cc1101] chip NOT detected: PARTNUM=0x%02X VERSION=0x%02X "
                      "(expect 0x00 / 0x14) — check CC1101 CS pin (GPIO %d) and hat seating\n",
                      part, ver, CC1101_CS);
        return false;
    }
    Serial.printf("[cc1101] chip OK: PARTNUM=0x%02X VERSION=0x%02X\n", part, ver);

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
    /* The spectrum/waterfall modes re-strobe SIDLE->SRX on every frequency bin,
     * and with FS_AUTOCAL each SRX fires a ~720us PLL calibration. They then read
     * RSSI after only ~500us — i.e. WHILE the chip is still calibrating, so the
     * RSSI register holds a stale/constant value and the sweep never sees a real
     * burst (flat waterfall). Wait until the radio has actually reached RX
     * (MARCSTATE 0x0D) before reading, then let the RSSI filter settle briefly.
     * Poll up to ~3ms; a settled radio falls through in one read. */
    for (int i = 0; i < 30; ++i) {
        if ((ELECHOUSE_cc1101.SpiReadStatus(0x35) & 0x1F) == 0x0D) break;  /* RX */
        delayMicroseconds(100);
    }
    /* Fallback: if we never reached RX (e.g. a retune dropped us to IDLE and the
     * caller didn't re-strobe), force RX once so we never read RSSI out of RX. */
    if ((ELECHOUSE_cc1101.SpiReadStatus(0x35) & 0x1F) != 0x0D) {
        ELECHOUSE_cc1101.SetRx();
        for (int i = 0; i < 30; ++i) {
            if ((ELECHOUSE_cc1101.SpiReadStatus(0x35) & 0x1F) == 0x0D) break;
            delayMicroseconds(100);
        }
    }
    delayMicroseconds(300);   /* RSSI valid time at RxBW 650 */
    return ELECHOUSE_cc1101.getRssi();
}

/* TEMP DIAGNOSTIC (Track D): bring the CC1101 up, dump chip ID + MARCSTATE,
 * then stream RSSI for ~3.5 s so the operator can press a 433 fob and see if
 * the receiver actually reacts. PARTNUM should read 0x00, VERSION 0x14/0x17;
 * MARCSTATE 0x0D = RX. Flat RSSI while a fob is pressed = RX front-end dead. */
void cc1101_diag(void)
{
    bool up = cc1101_begin(433.92f);
    Serial.printf("[cc1101] DIAG begin=%d\n", up);
    if (!up) return;
    uint8_t part = ELECHOUSE_cc1101.SpiReadStatus(0x30);
    uint8_t ver  = ELECHOUSE_cc1101.SpiReadStatus(0x31);
    Serial.printf("[cc1101] DIAG PARTNUM=0x%02X VERSION=0x%02X\n", part, ver);
    ELECHOUSE_cc1101.SetRx();
    for (int i = 0; i < 24; ++i) {
        int rssi = ELECHOUSE_cc1101.getRssi();
        uint8_t marc = ELECHOUSE_cc1101.SpiReadStatus(0x35) & 0x1F;
        Serial.printf("[cc1101] DIAG rssi=%d marc=0x%02X\n", rssi, marc);
        delay(150);
    }
    cc1101_end();
}
