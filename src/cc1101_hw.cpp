/*
 * cc1101_hw.cpp — CC1101 init/teardown for Hydra RF Cap 424.
 */
#include "cc1101_hw.h"
#include "nrf24_hw.h"   /* NRF24_CS — deselect the nRF24 while CC1101 owns the bus */
#include "sd_helper.h"
#include "gps.h"
#include <ELECHOUSE_CC1101_SRC_DRV.h>
#include <SD.h>

#if defined(POSEIDON_BOARD_TEMBED)
#include "board/board_tembed.h"
#endif

static bool s_up = false;

#if defined(POSEIDON_BOARD_TEMBED)
/*
 * The T-Embed CC1101 does NOT have a single wideband antenna. It has a
 * band-switched matching network in front of the chip, steered by SW1 (GPIO47)
 * and SW0 (GPIO48). Truth table, verified against LilyGO's hardware via Bruce's
 * boards/lilygo-t-embed-cc1101 (rf_utils.cpp setMHZ) which drives this exact
 * board -- our pin map (CS 12 / GDO0 3 / GDO2 38 / SW1 47 / SW0 48) matches its
 * primary variant one for one:
 *
 *     SW1=1 SW0=0  ->  315 MHz path
 *     SW1=1 SW0=1  ->  434 MHz path
 *     SW1=0 SW0=1  ->  868/915 MHz path
 *
 * These pins were declared in board_tembed.h but NEVER driven anywhere in the
 * firmware, so the switch sat in whatever state it powered up in. The symptom
 * is not a clean failure: the chip configures fine and reports a healthy
 * PARTNUM/VERSION, but the RF path is routed through the wrong filter, so
 * sensitivity collapses and captures look dead or extremely weak.
 *
 * Cached so retuning within a band does not re-toggle the switch (each change
 * needs a settle delay). The 468..778 MHz gap has no defined path; leave the
 * switch alone there rather than guessing.
 */
static void cc1101_select_antenna(float mhz)
{
    static int cur_band = -1;

    int band;
    if      (mhz <= 350.0f)                 band = 0;   /* 315      */
    else if (mhz > 350.0f && mhz < 468.0f)  band = 1;   /* 434      */
    else if (mhz > 778.0f)                  band = 2;   /* 868/915  */
    else                                    return;     /* undefined gap */

    if (band == cur_band) return;

    pinMode(TE_CC1101_SW1, OUTPUT);
    pinMode(TE_CC1101_SW0, OUTPUT);
    switch (band) {
        case 0: digitalWrite(TE_CC1101_SW1, HIGH); digitalWrite(TE_CC1101_SW0, LOW);  break;
        case 1: digitalWrite(TE_CC1101_SW1, HIGH); digitalWrite(TE_CC1101_SW0, HIGH); break;
        default:digitalWrite(TE_CC1101_SW1, LOW);  digitalWrite(TE_CC1101_SW0, HIGH); break;
    }
    cur_band = band;
    delay(10);   /* let the switch settle before any TX/RX */
    Serial.printf("[cc1101] antenna band %d selected for %.3f MHz\n", band, mhz);
}
#endif /* POSEIDON_BOARD_TEMBED */

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
    /* Board-gated to match the SD SPI instance we reuse (see sd_helper.cpp). On
     * the T-Embed the bus is TE_SPI SCK=11/MISO=10/MOSI=9, not the Cardputer pads. */
#if defined(POSEIDON_BOARD_TEMBED)
    ELECHOUSE_cc1101.setSpiPin(11, 10, 9, CC1101_CS);
#else
    ELECHOUSE_cc1101.setSpiPin(40, 39, 14, CC1101_CS);
#endif
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
#if defined(POSEIDON_BOARD_TEMBED)
    /* Route the antenna network to this frequency's band BEFORE tuning. */
    cc1101_select_antenna(freq_mhz);
#endif
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
    /* Restore GDO0 to INPUT (some TX paths drive it OUTPUT).
     *
     * CS must be parked HIGH, not floated. Releasing it to high-Z on a bus
     * shared with the display, SD and nRF24 leaves the pin free to couple low
     * while the UI hammers SCK/MOSI to repaint -- and CSn going low is exactly
     * the documented wake condition out of SPWD, so the sleeping CC1101 can
     * wake mid-repaint, drive MISO against the SD card, and latch framebuffer
     * bytes as register writes. Every other driver here parks foreign CS lines
     * OUTPUT/HIGH (see cc1101_park_others and nrf24_hw.cpp); the CC1101 was the
     * only one releasing its own.
     * (Pin numbers deliberately not repeated in this comment -- they differ per
     * board and the old note here had all three wrong.) */
    pinMode(CC1101_GDO0, INPUT);
    pinMode(CC1101_CS, OUTPUT);
    digitalWrite(CC1101_CS, HIGH);
    s_up = false;
}

bool cc1101_is_up(void)    { return s_up; }
void cc1101_set_freq(float mhz)
{
    /* Drop to IDLE before retuning. The CC1101 latches FREQ registers on the
     * next state transition, so writing them while the chip is in RX can leave
     * the synthesiser on the previous channel until something else strobes it.
     * The caller re-strobes RX/TX itself (every sweep does), so IDLE here is
     * safe for both directions -- unconditionally forcing RX would break the
     * TX paths that retune through this same helper. */
    ELECHOUSE_cc1101.setSidle();
#if defined(POSEIDON_BOARD_TEMBED)
    /* Every retune must re-check the band switch: the spectrum sweep and the
     * frequency scanner walk across band boundaries, and staying on the old
     * filter is exactly what makes far-band bins read as dead air. */
    cc1101_select_antenna(mhz);
#endif
    ELECHOUSE_cc1101.setMHZ(mhz);
}
void cc1101_set_rx(void)   { ELECHOUSE_cc1101.SetRx(); }
void cc1101_set_tx(void)   { ELECHOUSE_cc1101.SetTx(); }
void cc1101_set_idle(void) { ELECHOUSE_cc1101.setSidle(); }

/* CC1101 status registers (0x30-0x3D) can glitch on SO mid-read — the register
 * value is correct but the byte clocked out over SPI is occasionally corrupt
 * (the well-known SWRS061 status-read errata). Live capture showed exactly this:
 * valid RSSI (-30..-107) interleaved with garbage (-138 = raw 0x80) and bogus
 * MARCSTATE (0x14/0x17/0x1F). The standard workaround is to read until two
 * consecutive reads agree. Without it, any waterfall bin that lands on a corrupt
 * sample reads floor — including a real fob's bin, so it looked unresponsive. */
static uint8_t cc1101_status_stable(uint8_t addr)
{
    uint8_t a = ELECHOUSE_cc1101.SpiReadStatus(addr);
    for (int i = 0; i < 8; ++i) {
        uint8_t b = ELECHOUSE_cc1101.SpiReadStatus(addr);
        if (a == b) return a;
        a = b;
    }
    return a;
}

int cc1101_get_rssi(void)
{
    /* Wait until the radio actually reached RX (MARCSTATE 0x0D) — the sweep
     * modes re-strobe SRX per bin and each SRX fires a ~720us PLL calibration,
     * so a read too soon returns a stale value. Reliable (stable) reads. */
    for (int i = 0; i < 30; ++i) {
        if ((cc1101_status_stable(0x35) & 0x1F) == 0x0D) break;
        delayMicroseconds(100);
    }
    if ((cc1101_status_stable(0x35) & 0x1F) != 0x0D) {   /* stuck IDLE -> force RX */
        ELECHOUSE_cc1101.SetRx();
        for (int i = 0; i < 30; ++i) {
            if ((cc1101_status_stable(0x35) & 0x1F) == 0x0D) break;
            delayMicroseconds(100);
        }
    }
    delayMicroseconds(300);   /* RSSI valid time at RxBW 650 */
    /* Read RSSI (reg 0x34) with the same stable-read workaround and convert per
     * the datasheet (RSSI offset 74 dBm at 433 MHz). */
    uint8_t raw = cc1101_status_stable(0x34);
    return (raw >= 128) ? ((int)raw - 256) / 2 - 74 : (int)raw / 2 - 74;
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
