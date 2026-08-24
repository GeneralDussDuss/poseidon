#include "nfc_hw.h"
#include <Arduino.h>
#include <string.h>

#if defined(POSEIDON_BOARD_TEMBED)
#include <Wire.h>
#include "board/board_tembed.h"

/* PN532 sits on the T-Embed's MAIN/system I2C bus (SDA 8 / SCL 18) -- the same
 * bus as the BQ25896 charger and BQ27220 fuel gauge. It is NOT a private bus.
 *
 * This originally used Wire1 "so it never collides with the primary bus", which
 * was exactly backwards: binding a SECOND I2C peripheral to the same physical
 * pads re-matrixes those GPIOs, so whichever controller initialises last owns
 * the pins and the other talks to nothing. That is the same class of bug as the
 * SPI2/SPI3 collision that blanked the display, and it is why the PN532 never
 * answered. LilyGO's own stack and Bruce both drive this chip from the global
 * `Wire` (Bruce's PN532.cpp notes verbatim that the driver "always talks to the
 * global Wire - can't work if bus_HAL remapped i2c_bus to Wire1"). */
static TwoWire &W = Wire;
static const uint8_t PN532_ADDR = TE_NFC_I2C_ADDR; /* 0x24, 7-bit */

/* PN532 normal-mode frame markers. */
static const uint8_t HOSTTOPN532 = 0xD4;
static const uint8_t ACK_FRAME[6] = { 0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00 };

/* --- low-level frame I/O (PN532 UM §6.2.5, I2C variant prepends a status byte) --- */

static void writeCommand(const uint8_t *cmd, uint8_t len)
{
    const uint8_t length = (uint8_t)(len + 1); /* TFI + cmd/data */
    W.beginTransmission(PN532_ADDR);
    W.write((uint8_t)0x00);        /* preamble  */
    W.write((uint8_t)0x00);        /* start 1   */
    W.write((uint8_t)0xFF);        /* start 2   */
    W.write(length);               /* LEN       */
    W.write((uint8_t)(~length + 1)); /* LCS     */
    W.write(HOSTTOPN532);          /* TFI       */
    uint8_t sum = HOSTTOPN532;
    for (uint8_t i = 0; i < len; ++i) { W.write(cmd[i]); sum = (uint8_t)(sum + cmd[i]); }
    W.write((uint8_t)(~sum + 1));  /* DCS       */
    W.write((uint8_t)0x00);        /* postamble */
    W.endTransmission();
}

/* Poll the leading I2C status byte until bit0 (RDY) is set. */
static bool waitReady(uint16_t timeout_ms)
{
    const uint32_t start = millis();
    while ((uint32_t)(millis() - start) < timeout_ms) {
        if (W.requestFrom(PN532_ADDR, (uint8_t)1) && W.available()) {
            if (W.read() & 0x01) return true;
        }
        delay(1);
    }
    return false;
}

static bool readAck(void)
{
    uint8_t buf[7] = { 0 };
    if (!W.requestFrom(PN532_ADDR, (uint8_t)7)) return false;
    for (uint8_t i = 0; i < 7 && W.available(); ++i) buf[i] = W.read();
    /* buf[0] = status; buf[1..6] = ACK frame */
    return memcmp(buf + 1, ACK_FRAME, 6) == 0;
}

/* Reads a response frame and returns the PD (data) bytes after the TFI+cmd echo.
 * Returns the number of data bytes copied into out, or -1 on error. */
/* NOTE ON SIZES: this used to use a 64-byte scratch buffer with uint8_t
 * lengths, which silently capped every response at ~54 data bytes. That is
 * fine for MIFARE (16-byte blocks) but truncates ISO14443-4 APDU responses --
 * an EMV READ RECORD can return 256 bytes + SW. The buffer and the length
 * types are widened here so APDU traffic survives intact; nfc_begin() also
 * grows the Wire RX buffer to match, since the ESP32 I2C driver's default is
 * far smaller than a full PN532 frame. */
static int readResponse(uint8_t *out, uint16_t out_max, uint16_t timeout_ms)
{
    if (!waitReady(timeout_ms)) return -1;

    static uint8_t b[300];
    uint16_t want = (uint16_t)(out_max + 10);
    if (want > sizeof(b)) want = sizeof(b);
    if (!W.requestFrom(PN532_ADDR, (size_t)want)) return -1;

    uint16_t n = 0;
    while (W.available() && n < sizeof(b)) b[n++] = W.read();
    /* b[0]=status, b[1..3]=00 00 FF, b[4]=LEN, b[5]=LCS, b[6]=TFI(0xD5), b[7]=cmd+1, b[8..]=PD */
    if (n < 8 || b[1] != 0x00 || b[2] != 0x00 || b[3] != 0xFF) return -1;
    if (b[6] != 0xD5) return -1;
    const uint8_t LEN = b[4];
    if (LEN < 2) return -1;
    /* A PN532 normal-mode frame carries a single-byte LEN, so data is <=255. */
    uint16_t dataLen = (uint16_t)(LEN - 2); /* minus TFI + cmd echo */
    if (dataLen > out_max) dataLen = out_max;
    if (8u + dataLen > n) dataLen = (n > 8) ? (uint16_t)(n - 8) : 0;
    memcpy(out, b + 8, dataLen);
    return dataLen;
}

static bool command(const uint8_t *cmd, uint8_t len, uint8_t *resp, uint16_t resp_max, int *resp_len, uint16_t timeout_ms)
{
    writeCommand(cmd, len);
    if (!waitReady(timeout_ms)) return false;
    if (!readAck()) return false;
    const int n = readResponse(resp, resp_max, timeout_ms);
    if (resp_len) *resp_len = n;
    return n >= 0;
}

bool nfc_begin(void)
{
    pinMode(TE_NFC_RF_RST, OUTPUT);
    digitalWrite(TE_NFC_RF_RST, LOW);  delay(10);
    digitalWrite(TE_NFC_RF_RST, HIGH); delay(10);

    W.begin(TE_NFC_SDA, TE_NFC_SCL, 100000);
    W.setTimeOut(50);
    /* The default ESP32 I2C RX buffer is far smaller than a full PN532 frame.
     * ISO14443-4 APDU responses (EMV records) reach ~260 bytes, so grow it or
     * every long read is silently truncated. */
    W.setBufferSize(300);
    delay(10);

    /* Probe the address before talking protocol, so a wiring/bus fault is
     * distinguishable from a protocol fault. If the PN532 does not ACK, dump
     * every address that DOES answer -- that immediately shows whether the bus
     * is alive at all (the charger 0x6B and fuel gauge 0x55 should reply) or
     * whether the pins are dead. Without this the only symptom was a blanket
     * "not found", which told us nothing. */
    W.beginTransmission(PN532_ADDR);
    const uint8_t ack = W.endTransmission();
    if (ack != 0) {
        Serial.printf("[nfc] PN532 did not ACK at 0x%02X (rc=%u). Scanning bus...\n",
                      PN532_ADDR, ack);
        uint8_t found = 0;
        for (uint8_t a = 0x08; a < 0x78; ++a) {
            W.beginTransmission(a);
            if (W.endTransmission() == 0) {
                Serial.printf("[nfc]   device @ 0x%02X\n", a);
                ++found;
            }
        }
        Serial.printf("[nfc] %u device(s) on SDA %d / SCL %d\n",
                      found, TE_NFC_SDA, TE_NFC_SCL);
        return false;
    }
    Serial.printf("[nfc] PN532 ACK at 0x%02X\n", PN532_ADDR);

    const uint32_t fw = nfc_firmware_version();
    if (fw == 0) {
        Serial.println("[nfc] ACK but GetFirmwareVersion failed (protocol/timing)");
        return false;
    }
    Serial.printf("[nfc] firmware 0x%08lX\n", (unsigned long)fw);

    /* SAMConfiguration: normal mode, timeout 0x14 (~1s), use IRQ. */
    const uint8_t sam[] = { 0x14, 0x01, 0x14, 0x01 };
    uint8_t rsp[8];
    int n;
    return command(sam, sizeof(sam), rsp, sizeof(rsp), &n, 200);
}

uint32_t nfc_firmware_version(void)
{
    const uint8_t cmd[] = { 0x02 }; /* GetFirmwareVersion */
    uint8_t rsp[8];
    int n = 0;
    if (!command(cmd, sizeof(cmd), rsp, sizeof(rsp), &n, 200) || n < 4) return 0;
    return ((uint32_t)rsp[0] << 24) | ((uint32_t)rsp[1] << 16) | ((uint32_t)rsp[2] << 8) | rsp[3];
}

bool nfc_poll_tag(NfcTag *out, uint16_t timeout_ms)
{
    /* InListPassiveTarget: 1 target max, 106 kbps ISO14443 type A. */
    const uint8_t cmd[] = { 0x4A, 0x01, 0x00 };
    uint8_t rsp[32];
    int n = 0;
    if (!command(cmd, sizeof(cmd), rsp, sizeof(rsp), &n, timeout_ms) || n < 6) return false;
    /* rsp[0]=NbTg; then Tg, SENS_RES(2), SEL_RES(1), NFCIDLen, NFCID[...] */
    if (rsp[0] < 1) return false;
    out->atqa    = (uint16_t)((rsp[2] << 8) | rsp[3]);
    out->sak     = rsp[4];
    out->uid_len = rsp[5];
    if (out->uid_len > 10) out->uid_len = 10;
    for (uint8_t i = 0; i < out->uid_len && (6u + i) < (uint8_t)n; ++i) out->uid[i] = rsp[6 + i];
    return true;
}

const char *nfc_tag_type(const NfcTag *t)
{
    switch (t->sak) {
        case 0x00: return "NTAG / Ultralight";
        case 0x08: return "Mifare Classic 1K";
        case 0x09: return "Mifare Mini";
        case 0x18: return "Mifare Classic 4K";
        case 0x10:
        case 0x11: return "Mifare Plus";
        case 0x20: return "DESFire / ISO14443-4";
        case 0x28: return "JCOP / SmartMX";
        default:   return "ISO14443A";
    }
}

bool nfc_mifare_auth(uint8_t block, uint8_t keyType, const uint8_t key[6],
                     const uint8_t *uid, uint8_t uid_len)
{
    /* InDataExchange -> MIFARE Authentication: [0x40][Tg][0x60|A / 0x61|B][block][key6][uid...]. */
    uint8_t ulen = uid_len > 7 ? 7 : uid_len; /* Classic uses the 4-byte UID/cascade tail */
    uint8_t cmd[4 + 6 + 7];
    cmd[0] = 0x40;              /* InDataExchange */
    cmd[1] = 0x01;              /* logical target 1 */
    cmd[2] = keyType ? 0x61 : 0x60;
    cmd[3] = block;
    memcpy(cmd + 4, key, 6);
    memcpy(cmd + 10, uid, ulen);
    uint8_t rsp[8];
    int n = 0;
    if (!command(cmd, (uint8_t)(10 + ulen), rsp, sizeof(rsp), &n, 200) || n < 1) return false;
    return rsp[0] == 0x00; /* InDataExchange status: 0x00 = success */
}

bool nfc_tag_is_iso14443_4(const NfcTag *t)
{
    /* SAK bit 5 (0x20) = "ISO/IEC 14443-4 compliant". EMV cards, ePassports,
     * DESFire and JCOP all set it; MIFARE Classic (0x08/0x18) does not. */
    return t && (t->sak & 0x20) != 0;
}

int nfc_apdu(const uint8_t *apdu, uint8_t apdu_len,
             uint8_t *resp, uint16_t resp_max)
{
    /* InDataExchange: [0x40][Tg=1][C-APDU...] -> [status][R-APDU...].
     * The PN532 does the T=CL block framing, so we pass the APDU straight
     * through. Its buffer caps a single exchange at ~262 bytes, which covers
     * every EMV record (max 256 + SW) without needing chaining. */
    if (!apdu || apdu_len > 253) return -1;

    uint8_t cmd[2 + 253];
    cmd[0] = 0x40;              /* InDataExchange   */
    cmd[1] = 0x01;              /* logical target 1 */
    memcpy(cmd + 2, apdu, apdu_len);

    /* +1 for the leading InDataExchange status byte. */
    uint8_t rsp[260];
    uint16_t want = (uint16_t)((resp_max + 1 > (int)sizeof(rsp)) ? sizeof(rsp) : resp_max + 1);
    int n = 0;
    if (!command(cmd, (uint8_t)(2 + apdu_len), rsp, want, &n, 800) || n < 1) return -1;
    if (rsp[0] != 0x00) return -1;        /* PN532-level error */

    const int data_len = n - 1;           /* strip the status byte */
    if (data_len < 2) return -1;          /* need at least SW1 SW2 */
    const int copy = (data_len > resp_max) ? resp_max : data_len;
    memcpy(resp, rsp + 1, copy);
    return copy;
}

bool nfc_mifare_read(uint8_t block, uint8_t out[16])
{
    const uint8_t cmd[4] = { 0x40, 0x01, 0x30, block }; /* InDataExchange -> MIFARE Read */
    uint8_t rsp[20];
    int n = 0;
    if (!command(cmd, sizeof(cmd), rsp, sizeof(rsp), &n, 200) || n < 17 || rsp[0] != 0x00) return false;
    memcpy(out, rsp + 1, 16);
    return true;
}

void nfc_end(void)
{
    W.end();
}

#else /* not T-Embed: PN532 not present */

bool nfc_begin(void) { return false; }
uint32_t nfc_firmware_version(void) { return 0; }
bool nfc_poll_tag(NfcTag *, uint16_t) { return false; }
const char *nfc_tag_type(const NfcTag *) { return "n/a"; }
bool nfc_mifare_auth(uint8_t, uint8_t, const uint8_t[6], const uint8_t *, uint8_t) { return false; }
bool nfc_mifare_read(uint8_t, uint8_t[16]) { return false; }
int  nfc_apdu(const uint8_t *, uint8_t, uint8_t *, uint16_t) { return -1; }
bool nfc_tag_is_iso14443_4(const NfcTag *) { return false; }
void nfc_end(void) {}

#endif
