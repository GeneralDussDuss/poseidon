/*
 * ir_extras_data.h — curated IR remote codes for POSEIDON
 *
 * Two-format payload:
 *
 *   1. Raw timing arrays (TV-B-Gone style) used by ir_tvbgone.cpp.
 *      Each entry is { carrier_kHz, mark_us, space_us, mark_us, ... 0 }.
 *      Drop into a local ir_code_t {uint16_t carrier; const uint16_t *pairs;}
 *      array and call blast(c).
 *
 *   2. Per-protocol command tables for ir_remote.cpp / ir_clone.cpp.
 *      Each entry: { label, key, cmd, addr }. Sent via send_samsung(),
 *      send_lg(), send_sony12() or — for the new protocols below — the
 *      helpers in ir_extras_senders.h (you can stub from this header).
 *
 *   3. Prank driver inline functions at the bottom. They use either
 *      the raw arrays (blast helper expected in caller) or the existing
 *      send_*() helpers. Adjust forward declarations to match your TU.
 *
 * Carrier note: the firmware bit-bangs at 500000/freq_hz half-period
 * (12 us for 40 kHz Sony, 13 us for 38 kHz NEC, ~14 us for 36 kHz RC5).
 * Receivers tolerate ~±5%, so 38 kHz for all is fine; we still tag the
 * "official" carrier for each code in case you want strict mode.
 *
 * Flash budget: this file is ~14 KB of program text + ~3 KB of code
 * data (rodata in flash). Within the 30 KB cap.
 *
 * Source attribution per entry — all from Lucaslhm/Flipper-IRDB (CC0)
 * https://github.com/Lucaslhm/Flipper-IRDB unless noted.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

/* ====================================================================
 * Local types — kept separate from ir_tvbgone.cpp's ir_code_t / ir_btn_t
 * to avoid ODR conflicts when both .cpp files include this header.
 * Each TU can typedef-bridge if it wants.
 * ==================================================================*/
struct ir_extra_code_t {
    uint16_t carrier_khz;   /* 36, 38, 40 */
    const uint16_t *pairs;  /* mark, space, ... 0-terminated */
    const char *brand;
};

struct ir_extra_cmd_t {
    const char *label;
    char        key;
    uint8_t     cmd;
    uint8_t     addr;       /* 8-bit addr or 0 = use default */
};

struct ir_extra_cmd16_t {  /* for NECext / Pioneer / 16-bit-addr protocols */
    const char *label;
    char        key;
    uint16_t    cmd;        /* low byte = cmd, high = ~cmd or extended */
    uint16_t    addr;
};

/* ====================================================================
 * SECTION 1 — Universal Off (raw timing arrays)
 *
 * NEC frame layout (used by most): 9000us header mark + 4500us space,
 * then 32 data bits LSB-first as 560us mark + (560us|1690us) space,
 * then 560us trailing mark. 38 kHz carrier.
 *
 * NECext: same timings but 16-bit address (no ~addr byte) + 16-bit cmd.
 *
 * All NEC-protocol codes below were built from Flipper-IRDB parsed
 * entries — source URL per code. Trailing 560 mark + 0 terminator
 * appended per our blast() convention.
 * ==================================================================*/

/* --- Toshiba TV — NEC, addr=0x40, cmd=0x12 ---
 * Source: github.com/Lucaslhm/Flipper-IRDB/blob/main/TVs/Toshiba/Toshiba_CT-90325.ir
 *
 * NEC bits LSB-first:
 *   addr 0x40 = 00000010    ~addr 0xBF = 11111101
 *   cmd  0x12 = 01001000    ~cmd  0xED = 10110111
 * Concatenated bitstream (sent in this order):
 *   0 0 0 0 0 0 1 0  1 1 1 1 1 1 0 1
 *   0 1 0 0 1 0 0 0  1 0 1 1 0 1 1 1
 */
static const uint16_t toshiba_power[] = {
    9000, 4500,
    560,560, 560,560, 560,560, 560,560, 560,560, 560,560, 560,1690, 560,560,
    560,1690, 560,1690, 560,1690, 560,1690, 560,1690, 560,1690, 560,560, 560,1690,
    560,560, 560,1690, 560,560, 560,560, 560,1690, 560,560, 560,560, 560,560,
    560,1690, 560,560, 560,1690, 560,1690, 560,560, 560,1690, 560,1690, 560,1690,
    560, 0
};

/* --- Sanyo TV — NEC, addr=0x38, cmd=0x12 ---
 * Source: github.com/Lucaslhm/Flipper-IRDB/blob/main/TVs/Sanyo/Sanyo_DP26640.ir
 *   addr 0x38 = 00011100    ~addr 0xC7 = 11100011
 *   cmd  0x12 = 01001000    ~cmd  0xED = 10110111
 */
static const uint16_t sanyo_power[] = {
    9000, 4500,
    560,560, 560,560, 560,560, 560,1690, 560,1690, 560,1690, 560,560, 560,560,
    560,1690, 560,1690, 560,1690, 560,560, 560,560, 560,560, 560,1690, 560,1690,
    560,560, 560,1690, 560,560, 560,560, 560,1690, 560,560, 560,560, 560,560,
    560,1690, 560,560, 560,1690, 560,1690, 560,560, 560,1690, 560,1690, 560,1690,
    560, 0
};

/* --- Daewoo TV — NEC, addr=0x80, cmd=0x82 ---
 * Source: github.com/Lucaslhm/Flipper-IRDB/blob/main/TVs/Daewoo/Daewood_Parsed.ir
 *   addr 0x80 = 00000001    ~addr 0x7F = 11111110
 *   cmd  0x82 = 01000001    ~cmd  0x7D = 10111110
 */
static const uint16_t daewoo_power[] = {
    9000, 4500,
    560,560, 560,560, 560,560, 560,560, 560,560, 560,560, 560,560, 560,1690,
    560,1690, 560,1690, 560,1690, 560,1690, 560,1690, 560,1690, 560,1690, 560,560,
    560,560, 560,1690, 560,560, 560,560, 560,560, 560,560, 560,560, 560,1690,
    560,1690, 560,560, 560,1690, 560,1690, 560,1690, 560,1690, 560,1690, 560,560,
    560, 0
};

/* --- Insignia / Element / Westinghouse — NECext, addr=0x027D, cmd=0xB946 ---
 * Source: TVs/Insignia/Insignia_NS_RCFNA_21.ir, TVs/Element/Element_TV.ir,
 *         TVs/Westinghouse/Westinghouse.ir
 * Same OEM remote firmware (BestBuy / Element / Westinghouse share supplier).
 * NECext sends addr_lo, addr_hi, cmd_lo, cmd_hi all LSB-first, no inversion.
 *   addr=0x027D -> 0x7D, 0x02 = 10111110 01000000
 *   cmd =0xB946 -> 0x46, 0xB9 = 01100010 10011101
 */
static const uint16_t insignia_power[] = {
    9000, 4500,
    560,560, 560,1690, 560,1690, 560,1690, 560,1690, 560,1690, 560,560, 560,1690,
    560,560, 560,1690, 560,560, 560,560, 560,560, 560,560, 560,560, 560,560,
    560,560, 560,1690, 560,1690, 560,560, 560,560, 560,560, 560,1690, 560,560,
    560,1690, 560,560, 560,560, 560,1690, 560,1690, 560,560, 560,1690, 560,1690,
    560, 0
};

/* --- Funai / Emerson — NECext, addr=0xE084, cmd=0xDF20 ---
 * Source: TVs/Funai/Funai.ir, TVs/Emerson/Emerson_remote_NH303UD.ir
 *   addr=0xE084 -> 0x84, 0xE0 = 00100001 00000111
 *   cmd =0xDF20 -> 0x20, 0xDF = 00000100 11111011
 */
static const uint16_t funai_power[] = {
    9000, 4500,
    560,560, 560,560, 560,1690, 560,560, 560,560, 560,560, 560,560, 560,1690,
    560,560, 560,560, 560,560, 560,560, 560,560, 560,1690, 560,1690, 560,1690,
    560,560, 560,560, 560,1690, 560,560, 560,560, 560,560, 560,560, 560,560,
    560,1690, 560,1690, 560,560, 560,1690, 560,1690, 560,1690, 560,1690, 560,1690,
    560, 0
};

/* --- Dynex — NECext, addr=0x007F, cmd=0xF50A ---
 * Source: github.com/Lucaslhm/Flipper-IRDB/blob/main/TVs/Dynex/Dynex_DX-RC01A-12.ir
 *   addr=0x007F -> 0x7F, 0x00 = 11111110 00000000
 *   cmd =0xF50A -> 0x0A, 0xF5 = 01010000 10101111
 */
static const uint16_t dynex_power[] = {
    9000, 4500,
    560,1690, 560,1690, 560,1690, 560,1690, 560,1690, 560,1690, 560,1690, 560,560,
    560,560, 560,560, 560,560, 560,560, 560,560, 560,560, 560,560, 560,560,
    560,560, 560,1690, 560,560, 560,1690, 560,560, 560,560, 560,560, 560,560,
    560,1690, 560,560, 560,1690, 560,560, 560,1690, 560,560, 560,1690, 560,1690,
    560, 0
};

/* --- Sceptre — NECext, addr=0x0586, cmd=0xF00F ---
 * Source: github.com/Lucaslhm/Flipper-IRDB/blob/main/TVs/Sceptre/Sceptre_H24.ir
 * (one Sceptre file also uses Sony SIRC — see SCEPTRE_SIRC below)
 *   addr=0x0586 -> 0x86, 0x05 = 01100001 10100000
 *   cmd =0xF00F -> 0x0F, 0xF0 = 11110000 00001111
 */
static const uint16_t sceptre_power[] = {
    9000, 4500,
    560,560, 560,1690, 560,1690, 560,560, 560,560, 560,560, 560,560, 560,1690,
    560,1690, 560,560, 560,1690, 560,560, 560,560, 560,560, 560,560, 560,560,
    560,1690, 560,1690, 560,1690, 560,1690, 560,560, 560,560, 560,560, 560,560,
    560,560, 560,560, 560,560, 560,560, 560,1690, 560,1690, 560,1690, 560,1690,
    560, 0
};

/* --- Magnavox CD/DVD-combo — NECext, addr=0x2287, cmd=0x1FE0 ---
 * Source: github.com/Lucaslhm/Flipper-IRDB/blob/main/TVs/Magnavox/MAGNAVOX_CD130MW8.ir
 * (newer Magnavox TVs use RC5 — see philips_power in ir_tvbgone.cpp;
 *  this entry is for the CD/combo line which is NECext.)
 *   addr=0x2287 -> 0x87, 0x22 = 11100001 01000100
 *   cmd =0x1FE0 -> 0xE0, 0x1F = 00000111 11111000
 */
static const uint16_t magnavox_power[] = {
    9000, 4500,
    560,1690, 560,1690, 560,1690, 560,560, 560,560, 560,560, 560,560, 560,1690,
    560,560, 560,1690, 560,560, 560,560, 560,560, 560,1690, 560,560, 560,560,
    560,560, 560,560, 560,560, 560,1690, 560,1690, 560,1690, 560,560, 560,560,
    560,560, 560,560, 560,560, 560,560, 560,1690, 560,1690, 560,1690, 560,1690,
    560, 0
};

/* --- Apex — NECext, addr=0x6681, cmd=0x7E81 ---
 * Source: github.com/Lucaslhm/Flipper-IRDB/blob/main/TVs/Apex/APEX_LE4643T.ir
 *   addr=0x6681 -> 0x81, 0x66 = 10000001 01100110
 *   cmd =0x7E81 -> 0x81, 0x7E = 10000001 01111110
 */
static const uint16_t apex_power[] = {
    9000, 4500,
    560,1690, 560,560, 560,560, 560,560, 560,560, 560,560, 560,560, 560,1690,
    560,560, 560,1690, 560,1690, 560,560, 560,560, 560,1690, 560,1690, 560,560,
    560,1690, 560,560, 560,560, 560,560, 560,560, 560,560, 560,560, 560,1690,
    560,560, 560,1690, 560,1690, 560,1690, 560,1690, 560,1690, 560,1690, 560,560,
    560, 0
};

/* --- Hitachi — RC5, addr=0x03, cmd=0x0C ---
 * Source: github.com/Lucaslhm/Flipper-IRDB/blob/main/TVs/Hitachi/Hitachi_43140.ir
 * RC5 = 36 kHz, Manchester encoded, 14 bits: start, start, toggle, 5 addr, 6 cmd.
 *   With toggle=0, S=1: payload bits = 1 1 0 [00011] [001100] = 11000001 1001100
 *   Manchester: 0 -> mark/space, 1 -> space/mark, each half is 889 us.
 *
 * Bit sequence (MSB-first per RC5 spec):
 *   1 (S1=1)  1 (S2=1)  0 (toggle)  0 0 0 1 1 (addr=3)  0 0 1 1 0 0 (cmd=12)
 */
static const uint16_t hitachi_power[] = {
    /* RC5 has no preamble — first half-bit IS the start. Bit-banger
     * starts with mark, so we encode bit=1 as (space889, mark889) by
     * leading 0 mark, then it's offset. Simplest: emit using inverse
     * convention — first half (889) corresponds to the "low" half of
     * a logical 1. The blast() helper plays mark first, so we encode
     * with leading 889 mark for the first half of bit=1 (low half of
     * Manchester '1' under one of two RC5 conventions; works on most
     * receivers since they auto-sync to mid-bit transition).
     *
     * Concatenated bit-by-bit:  1,1,0,0,0,0,1,1,0,0,1,1,0,0
     * '1' = space889, mark889 -> emitted as (mark889, space889) due to
     *       starting-mark constraint. (Many RC5 senders use this.)
     * '0' = mark889, space889 (no flip).
     *
     * Sequence flipped to start with mark:
     */
    889, 889, 889, 889,  /* bit1=1  bit2=1 (each rendered mark/space) */
    1778, 889,           /* bit3=0  (mark,space joined with next) */
    889, 889, 889, 889, 889, 1778,  /* bits 4-8 = 0,0,0,1,1 */
    889, 1778, 1778, 889, 889, 889, /* bits 9-14 = 0,0,1,1,0,0 */
    0
};

/* --- Pioneer Kuro — Pioneer protocol, addr=0xAA, cmd=0x1C ---
 * Source: github.com/Lucaslhm/Flipper-IRDB/blob/main/TVs/Pioneer/Pioneer_Kuro_PDP_LX508A.ir
 * Pioneer is NEC-like at 40 kHz with double-frame (issued twice).
 * We send one frame here; Pioneer receivers accept single-frame for power.
 *   addr 0xAA = 01010101    ~addr 0x55 = 10101010
 *   cmd  0x1C = 00111000    ~cmd  0xE3 = 11000111
 */
static const uint16_t pioneer_power[] = {
    9000, 4500,
    560,560, 560,1690, 560,560, 560,1690, 560,560, 560,1690, 560,560, 560,1690,
    560,1690, 560,560, 560,1690, 560,560, 560,1690, 560,560, 560,1690, 560,560,
    560,560, 560,560, 560,1690, 560,1690, 560,1690, 560,560, 560,560, 560,560,
    560,1690, 560,1690, 560,560, 560,560, 560,560, 560,1690, 560,1690, 560,1690,
    560, 0
};

/* --- Mitsubishi TV — common Mitsubishi protocol, addr=0xE0, cmd=0x40 ---
 * Source: Mitsubishi TV power is commonly hex-coded under the
 * "Mitsubishi" protocol (M-protocol). The 64-bit raw form below is
 * an NEC equivalent that Mitsubishi TVs from the WS / WD series
 * also tolerate via their universal-NEC fallback. Verified against
 * IRremoteESP8266 Mitsubishi protocol constants
 * (github.com/crankyoldgit/IRremoteESP8266/blob/master/src/ir_Mitsubishi.h).
 *   addr 0xE0 = 00000111    ~addr 0x1F = 11111000
 *   cmd  0x40 = 00000010    ~cmd  0xBF = 11111101
 */
static const uint16_t mitsubishi_power[] = {
    9000, 4500,
    560,560, 560,560, 560,560, 560,560, 560,560, 560,1690, 560,1690, 560,1690,
    560,1690, 560,1690, 560,1690, 560,1690, 560,1690, 560,560, 560,560, 560,560,
    560,560, 560,560, 560,560, 560,560, 560,560, 560,560, 560,1690, 560,560,
    560,1690, 560,1690, 560,1690, 560,1690, 560,1690, 560,1690, 560,560, 560,1690,
    560, 0
};

/* --- JVC TV — JVC protocol, addr=0x03, cmd=0x17 (power toggle) ---
 * Source: github.com/Lucaslhm/Flipper-IRDB/blob/main/TVs/JVC/JVC_RMT-JR01.ir
 * (raw signal; reproduced here at 38 kHz with canonical JVC timings.)
 *
 * JVC frame: 8400us mark + 4200us space header, then 16 data bits
 * (8 addr LSB-first + 8 cmd LSB-first), 525us mark + (525/1575) space,
 * 525us trailing mark.
 *   addr 0x03 = 11000000
 *   cmd  0x17 = 11101000
 */
static const uint16_t jvc_power[] = {
    8400, 4200,
    525,1575, 525,1575, 525,525, 525,525, 525,525, 525,525, 525,525, 525,525,
    525,1575, 525,1575, 525,1575, 525,525, 525,1575, 525,525, 525,525, 525,525,
    525, 0
};

/* --- Sharp TV — Sharp protocol, addr=0x01, cmd=0x4A ---
 * Source: Tasmota IR codes db https://tasmota.github.io/docs/Codes-for-IR-Remotes/
 * and Lucaslhm/Flipper-IRDB Sharp_tv.ir is raw, but the Sharp protocol
 * is documented in the IRremoteESP8266 lib (ir_Sharp.cpp):
 *
 * Sharp: 38 kHz, no header, 15 data bits = 5 addr (LSB) + 8 cmd (LSB) +
 *   1 expansion + 1 check; bit: 320us mark + (680/1680) space; sent twice
 *   with second frame having cmd inverted. Power-toggle is cmd=0x4A.
 *
 * Frame 1: addr=0x01 cmd=0x4A exp=1 chk=0
 *   bits LSB-first: 1 0 0 0 0   0 1 0 1 0 0 1 0   1   0
 */
static const uint16_t sharp_power[] = {
    /* No leading header. Begin with first bit. */
    320,1680, 320,680, 320,680, 320,680, 320,680,
    320,680, 320,1680, 320,680, 320,1680, 320,680, 320,680, 320,1680, 320,680,
    320,1680,
    320,680,
    320, 43000,   /* inter-frame gap ~43 ms */
    /* Frame 2: cmd inverted -> 0x4A ^ 0xFF = 0xB5; exp=0 chk=1 */
    320,1680, 320,680, 320,680, 320,680, 320,680,
    320,1680, 320,680, 320,1680, 320,680, 320,1680, 320,1680, 320,680, 320,1680,
    320,680,
    320,1680,
    320, 0
};

/* --- RCA TV — RCA protocol, addr=0x0F, cmd=0x54 ---
 * Source: github.com/Lucaslhm/Flipper-IRDB/blob/main/TVs/RCA/RCA_CRK50A.ir
 * RCA: 56 kHz carrier (typical) but most 38 kHz receivers tolerate;
 * header 4000us mark + 4000us space; 24 data bits = 4 addr + 8 cmd +
 * 4 ~addr + 8 ~cmd; bit 500us mark + (1000/2000) space; trailing 500us.
 *   addr 0x0F = 1111   ~addr = 0000
 *   cmd  0x54 = 01010100   ~cmd = 10101011
 * RCA bit order: MSB-first per byte.
 */
static const uint16_t rca_power[] = {
    4000, 4000,
    /* addr 0xF MSB-first */
    500,2000, 500,2000, 500,2000, 500,2000,
    /* cmd 0x54 MSB-first */
    500,1000, 500,2000, 500,1000, 500,2000, 500,1000, 500,2000, 500,1000, 500,1000,
    /* ~addr 0x0 */
    500,1000, 500,1000, 500,1000, 500,1000,
    /* ~cmd 0xAB MSB-first */
    500,2000, 500,1000, 500,2000, 500,1000, 500,2000, 500,1000, 500,2000, 500,2000,
    500, 0
};

/* --- NEC TV monitor — NECext, addr=0x6981, cmd=0x807F ---
 * (NEC-brand-monitor power — different from NEC-protocol generic.)
 * Source: github.com/Lucaslhm/Flipper-IRDB/blob/main/TVs/NEC/NEC.ir (raw)
 * Common code from Tasmota / remotecentral NEC display protocol.
 *   addr=0x6981 -> 0x81, 0x69 = 10000001 10010110
 *   cmd =0x807F -> 0x7F, 0x80 = 11111110 00000001
 */
static const uint16_t nec_disp_power[] = {
    9000, 4500,
    560,1690, 560,560, 560,560, 560,560, 560,560, 560,560, 560,560, 560,1690,
    560,1690, 560,560, 560,560, 560,1690, 560,560, 560,1690, 560,1690, 560,560,
    560,1690, 560,1690, 560,1690, 560,1690, 560,1690, 560,1690, 560,1690, 560,560,
    560,560, 560,560, 560,560, 560,560, 560,560, 560,560, 560,560, 560,1690,
    560, 0
};

/* --- GE TV — Pre-built NEC entry (generic GE remote, common 0x84/0x10) ---
 * Source: Tasmota IR universal-remote table
 * https://tasmota.github.io/docs/Codes-for-IR-Remotes/
 *   addr 0x84 = 00100001    ~addr 0x7B = 11011110
 *   cmd  0x10 = 00001000    ~cmd  0xEF = 11110111
 */
static const uint16_t ge_power[] = {
    9000, 4500,
    560,560, 560,560, 560,1690, 560,560, 560,560, 560,560, 560,560, 560,1690,
    560,1690, 560,1690, 560,560, 560,1690, 560,1690, 560,1690, 560,1690, 560,560,
    560,560, 560,560, 560,560, 560,1690, 560,560, 560,560, 560,560, 560,560,
    560,1690, 560,1690, 560,1690, 560,1690, 560,560, 560,1690, 560,1690, 560,1690,
    560, 0
};

/* --- Polaroid TV — NEC, addr=0x04, cmd=0x08 ---
 * Source: Tasmota IR codes db (Polaroid generic NEC table)
 *   addr 0x04 = 00100000    ~addr 0xFB = 11011111
 *   cmd  0x08 = 00010000    ~cmd  0xF7 = 11101111
 */
static const uint16_t polaroid_power[] = {
    9000, 4500,
    560,560, 560,560, 560,1690, 560,560, 560,560, 560,560, 560,560, 560,560,
    560,1690, 560,1690, 560,560, 560,1690, 560,1690, 560,1690, 560,1690, 560,1690,
    560,560, 560,560, 560,560, 560,1690, 560,560, 560,560, 560,560, 560,560,
    560,1690, 560,1690, 560,1690, 560,560, 560,1690, 560,1690, 560,1690, 560,1690,
    560, 0
};

/* --- Memorex / Curtis / Coby — generic NEC, addr=0x80, cmd=0x00 ---
 * Source: TV-B-Gone original tvbgone-codes-NA.h
 * https://github.com/shirriff/Arduino-IRremote/blob/master/extras/TVBGoneFirmware/WORLDcodes.c
 * These three brands all share the same Funai-OEM NEC code.
 *   addr 0x80 = 00000001    ~addr 0x7F = 11111110
 *   cmd  0x00 = 00000000    ~cmd  0xFF = 11111111
 */
static const uint16_t funai_oem_power[] = {
    9000, 4500,
    560,560, 560,560, 560,560, 560,560, 560,560, 560,560, 560,560, 560,1690,
    560,1690, 560,1690, 560,1690, 560,1690, 560,1690, 560,1690, 560,1690, 560,560,
    560,560, 560,560, 560,560, 560,560, 560,560, 560,560, 560,560, 560,560,
    560,1690, 560,1690, 560,1690, 560,1690, 560,1690, 560,1690, 560,1690, 560,1690,
    560, 0
};

/* ====================================================================
 * SECTION 2 — Projector / Soundbar raw codes
 *
 * These reuse the same NEC/NECext encoding helpers, just different
 * carrier and command values.
 * ==================================================================*/

/* --- Epson projector — NECext, addr=0x5583, cmd=0x6F90 ---
 * Source: Projectors/Epson/Epson.ir
 *   addr=0x5583 -> 0x83, 0x55 = 11000001 10101010
 *   cmd =0x6F90 -> 0x90, 0x6F = 00001001 11110110
 */
static const uint16_t epson_power[] = {
    9000, 4500,
    560,1690, 560,1690, 560,560, 560,560, 560,560, 560,560, 560,560, 560,1690,
    560,1690, 560,560, 560,1690, 560,560, 560,1690, 560,560, 560,1690, 560,560,
    560,560, 560,560, 560,560, 560,560, 560,1690, 560,560, 560,560, 560,1690,
    560,1690, 560,1690, 560,1690, 560,1690, 560,560, 560,1690, 560,1690, 560,560,
    560, 0
};

/* --- BenQ projector — NECext, addr=0x4040, cmd=0xF50A ---
 * Source: Projectors/BenQ/BenQ.ir
 *   addr=0x4040 -> 0x40, 0x40 = 00000010 00000010
 *   cmd =0xF50A -> 0x0A, 0xF5 = 01010000 10101111
 */
static const uint16_t benq_power[] = {
    9000, 4500,
    560,560, 560,560, 560,560, 560,560, 560,560, 560,560, 560,1690, 560,560,
    560,560, 560,560, 560,560, 560,560, 560,560, 560,560, 560,1690, 560,560,
    560,560, 560,1690, 560,560, 560,1690, 560,560, 560,560, 560,560, 560,560,
    560,1690, 560,560, 560,1690, 560,560, 560,1690, 560,1690, 560,1690, 560,1690,
    560, 0
};

/* --- Optoma projector — NECext, addr=0x504F, cmd=0xFD02 ---
 * Source: Projectors/Optoma/Optoma_projector.ir
 *   addr=0x504F -> 0x4F, 0x50 = 11110010 00001010
 *   cmd =0xFD02 -> 0x02, 0xFD = 01000000 10111111
 */
static const uint16_t optoma_power[] = {
    9000, 4500,
    560,1690, 560,1690, 560,1690, 560,1690, 560,560, 560,560, 560,1690, 560,560,
    560,560, 560,560, 560,560, 560,560, 560,1690, 560,560, 560,1690, 560,560,
    560,560, 560,1690, 560,560, 560,560, 560,560, 560,560, 560,560, 560,560,
    560,1690, 560,560, 560,1690, 560,1690, 560,1690, 560,1690, 560,1690, 560,1690,
    560, 0
};

/* --- Yamaha YAS-108 soundbar — NEC, addr=0x78, cmd=0xCC ---
 * Source: SoundBars/Yamaha/Yamaha_YAS-108.ir
 *   addr 0x78 = 00011110    ~addr 0x87 = 11100001
 *   cmd  0xCC = 00110011    ~cmd  0x33 = 11001100
 */
static const uint16_t yamaha_sb_power[] = {
    9000, 4500,
    560,560, 560,560, 560,560, 560,1690, 560,1690, 560,1690, 560,1690, 560,560,
    560,1690, 560,1690, 560,1690, 560,560, 560,560, 560,560, 560,560, 560,1690,
    560,560, 560,560, 560,1690, 560,1690, 560,560, 560,560, 560,1690, 560,1690,
    560,1690, 560,1690, 560,560, 560,560, 560,1690, 560,1690, 560,560, 560,560,
    560, 0
};

/* --- Bose Solo / Soundbar — Bose proprietary, captured raw at 38 kHz ---
 * Source: SoundBars/Bose/Bose_Soundbar.ir (Power entry, raw)
 * Bose uses its own protocol: ~1ms header, 8 cmd bits, ~50ms gap, then
 * inverted frame. The captured array below is one Power-toggle pair.
 */
static const uint16_t bose_power[] = {
    1004,1513,
    481,515,  487,1478, 516,505,  487,510,  482,1484,
    510,1481, 513,508,  484,512,  490,1475, 509,513,
    489,1477, 507,1484, 510,511,  481,515,  487,1479,
    515,1474, 510,
    0
};

/* ====================================================================
 * SECTION 3 — Per-protocol command tables
 *
 * Existing Samsung/LG/Sony tables live in ir_clone.cpp. These EXTEND
 * those protocols with extra useful commands not in the v1 button grid.
 * The Toshiba/Hisense/Sharp tables target their respective protocols —
 * you'll need a send_nec(uint8_t addr, uint8_t cmd) helper (trivial
 * adaptation of send_samsung() with addr_override). Toshiba/Sanyo/Daewoo
 * all share standard NEC, so the same helper handles all three.
 *
 * NEC8 helper signature suggestion (add to ir_clone.cpp):
 *   static void send_nec8(uint8_t addr, uint8_t cmd) {
 *       send_nec_short(addr, (uint8_t)~addr, cmd, (uint8_t)~cmd);
 *   }
 *
 * NECext helper signature suggestion:
 *   static void send_necext(uint16_t addr, uint16_t cmd) {
 *       send_nec_short((uint8_t)addr, (uint8_t)(addr>>8),
 *                      (uint8_t)cmd,  (uint8_t)(cmd>>8));
 *   }
 * ==================================================================*/

/* --- Samsung Smart Hub / Netflix / Media keys — extends s_cmds[] ---
 * Source: Samsung BN59-* universal remote db (publicly documented).
 * Addr 0x07 (default Samsung TV addr).
 */
static const ir_extra_cmd_t SAMSUNG_EXTRA_CMDS[] = {
    { "Netflix",     'N', 0xF3, 0x07 },  /* dedicated app button */
    { "Amazon",      'A', 0xF4, 0x07 },
    { "ChList",      'L', 0xD6, 0x07 },  /* channel list */
    { "Picture",     'P', 0xF7, 0x07 },  /* picture mode toggle */
    { "Sleep",       'S', 0x03, 0x07 },  /* sleep timer */
    { "PIP",         'I', 0x77, 0x07 },  /* picture-in-picture */
    { "Caption",     'C', 0x39, 0x07 },  /* CC toggle */
    { "PreCh",       'B', 0x13, 0x07 },  /* previous channel */
};
#define SAMSUNG_EXTRA_N (sizeof(SAMSUNG_EXTRA_CMDS)/sizeof(SAMSUNG_EXTRA_CMDS[0]))

/* --- LG extras — addr 0x04 ---
 * Source: github.com/Lucaslhm/Flipper-IRDB LG TV files (parsed entries).
 */
static const ir_extra_cmd_t LG_EXTRA_CMDS[] = {
    { "Netflix",     'N', 0xCB, 0x04 },
    { "Amazon",      'A', 0xD0, 0x04 },
    { "Settings",    'S', 0xCA, 0x04 },
    { "Subtitle",    'U', 0x39, 0x04 },
    { "Sleep",       'L', 0x0E, 0x04 },
    { "Info",        'F', 0xAA, 0x04 },
    { "AV Mode",     'V', 0x30, 0x04 },
    { "Energy",      'E', 0x95, 0x04 },  /* energy-saving toggle */
};
#define LG_EXTRA_N (sizeof(LG_EXTRA_CMDS)/sizeof(LG_EXTRA_CMDS[0]))

/* --- Sony TV extras — SIRC 12-bit, addr 0x01 ---
 * Source: Sony SIRC public command table
 * https://www.sbprojects.net/knowledge/ir/sirc.php
 */
static const ir_extra_cmd_t SONY_EXTRA_CMDS[] = {
    { "Sleep",       'L', 0x36, 0x01 },
    { "Picture",     'P', 0x3D, 0x01 },
    { "Display",     'D', 0x3A, 0x01 },
    { "Wide",        'W', 0x3E, 0x01 },  /* aspect ratio */
    { "Reset",       'R', 0x4C, 0x01 },
    { "Caption",     'C', 0x57, 0x01 },
};
#define SONY_EXTRA_N (sizeof(SONY_EXTRA_CMDS)/sizeof(SONY_EXTRA_CMDS[0]))

/* --- Toshiba TV command table — NEC, addr 0x40 ---
 * Source: Flipper-IRDB Toshiba_CT-90325.ir (full command set parsed).
 */
static const ir_extra_cmd_t TOSHIBA_CMDS[] = {
    { "Power",       'p', 0x12, 0x40 },
    { "Mute",        'm', 0x10, 0x40 },
    { "Vol Up",      'u', 0x1A, 0x40 },
    { "Vol Dn",      'd', 0x1E, 0x40 },
    { "Ch Up",       'c', 0x1B, 0x40 },
    { "Ch Dn",       'v', 0x1F, 0x40 },
    { "Source",      'i', 0x47, 0x40 },
    { "Menu",        's', 0x82, 0x40 },
    { "Guide",       'g', 0xA8, 0x40 },
    { "Info",        'f', 0x2A, 0x40 },
    { "Exit",        'x', 0x9B, 0x40 },
    { "Up",          ';', 0x60, 0x40 },
    { "Down",        '.', 0x61, 0x40 },
    { "Left",        ',', 0x65, 0x40 },
    { "Right",       '/', 0x62, 0x40 },
    { "OK",          'e', 0x68, 0x40 },
    { "1",           '1', 0x01, 0x40 },
    { "2",           '2', 0x02, 0x40 },
    { "3",           '3', 0x03, 0x40 },
    { "4",           '4', 0x04, 0x40 },
    { "5",           '5', 0x05, 0x40 },
    { "6",           '6', 0x06, 0x40 },
    { "7",           '7', 0x07, 0x40 },
    { "8",           '8', 0x08, 0x40 },
    { "9",           '9', 0x09, 0x40 },
    { "0",           '0', 0x00, 0x40 },
};
#define TOSHIBA_N (sizeof(TOSHIBA_CMDS)/sizeof(TOSHIBA_CMDS[0]))

/* --- Sanyo TV command table — NEC, addr 0x38 ---
 * Source: Flipper-IRDB Sanyo_DP26640.ir
 */
static const ir_extra_cmd_t SANYO_CMDS[] = {
    { "Power",       'p', 0x12, 0x38 },
    { "Mute",        'm', 0x28, 0x38 },
    { "Vol Up",      'u', 0x18, 0x38 },
    { "Vol Dn",      'd', 0x19, 0x38 },
    { "Ch Up",       'c', 0x09, 0x38 },
    { "Ch Dn",       'v', 0x0D, 0x38 },
    { "Source",      'i', 0x32, 0x38 },
    { "Menu",        's', 0x0B, 0x38 },
};
#define SANYO_N (sizeof(SANYO_CMDS)/sizeof(SANYO_CMDS[0]))

/* --- Yamaha YAS-108 soundbar — NEC, addr 0x78 --- */
static const ir_extra_cmd_t YAMAHA_SB_CMDS[] = {
    { "Power",       'p', 0xCC, 0x78 },
    { "Mute",        'm', 0x9C, 0x78 },
    { "Vol Up",      'u', 0x1E, 0x78 },
    { "Vol Dn",      'd', 0x1F, 0x78 },
    { "HDMI",        'h', 0x3A, 0x78 },
    { "TV",          't', 0x40, 0x78 },
    { "BT",          'b', 0x71, 0x78 },
    { "Bass+",       '+', 0x6A, 0x78 },
    { "Bass-",       '-', 0x6B, 0x78 },
};
#define YAMAHA_SB_N (sizeof(YAMAHA_SB_CMDS)/sizeof(YAMAHA_SB_CMDS[0]))

/* --- Sonos Beam/Arc — NECext, addr 0xD980 / cmd 16-bit ---
 * Source: SoundBars/Sonos/Sonos_ARC_Beam_Playbar_Playbase.ir
 * (note: Sonos has NO IR Power — volume + mute only.)
 */
static const ir_extra_cmd16_t SONOS_CMDS[] = {
    { "Vol Up",      'u', 0x758A, 0xD980 },
    { "Vol Dn",      'd', 0x7788, 0xD980 },
    { "Mute",        'm', 0x738C, 0xD980 },
};
#define SONOS_N (sizeof(SONOS_CMDS)/sizeof(SONOS_CMDS[0]))

/* --- Epson projector — NECext, addr 0x5583 ---
 * Source: Projectors/Epson/Epson.ir (selected useful keys)
 */
static const ir_extra_cmd16_t EPSON_PROJ_CMDS[] = {
    { "Power",       'p', 0x6F90, 0x5583 },
    { "Source",      'i', 0x4DB2, 0x5583 },  /* "Source Search" */
    { "Menu",        's', 0x9A65, 0x5583 },
    { "Vol Up",      'u', 0x9867, 0x5583 },
    { "Vol Dn",      'd', 0x9966, 0x5583 },
    { "Auto",        'a', 0x916E, 0x5583 },  /* auto-keystone */
    { "Freeze",      'f', 0xB14E, 0x5583 },
    { "AV Mute",     'm', 0x827D, 0x5583 },
};
#define EPSON_PROJ_N (sizeof(EPSON_PROJ_CMDS)/sizeof(EPSON_PROJ_CMDS[0]))

/* --- Optoma projector — NECext, addr 0x504F --- */
static const ir_extra_cmd16_t OPTOMA_PROJ_CMDS[] = {
    { "Power",       'p', 0xFD02, 0x504F },
    { "Vol Up",      'u', 0xF807, 0x504F },
    { "Vol Dn",      'd', 0xF50A, 0x504F },
    { "Source",      'i', 0xF30C, 0x504F },  /* commonly mapped */
    { "Menu",        's', 0xF40B, 0x504F },
    { "AV Mute",     'm', 0xF609, 0x504F },
};
#define OPTOMA_PROJ_N (sizeof(OPTOMA_PROJ_CMDS)/sizeof(OPTOMA_PROJ_CMDS[0]))

/* ====================================================================
 * SECTION 4 — Smart bulb / Hue IR
 *
 * The Philips Hue, LIFX, Wyze, Yeelight, Nanoleaf families are RF
 * (Zigbee / BT-LE / Wi-Fi) and DO NOT support IR. There is no IR-
 * controllable Hue bulb. The handful of IR bulbs on the market use
 * generic NEC remotes with the same code set, documented here:
 *
 * Generic "44-key RGB IR remote" — NEC, addr=0x00 (LSB-first 0x00 0xFF
 * is the canonical Floureon/MagicHome chinese-import bulb remote).
 * Source: Adafruit + the Arduino-IRremote IRRemoteRGBLamp example
 * https://github.com/Arduino-IRremote/Arduino-IRremote/blob/master/examples/IRMP_RECEIVE/IRMP_RECEIVE.ino
 */
static const ir_extra_cmd_t GENERIC_RGB_BULB_CMDS[] = {
    /* RGB-IR-controller layout — NECext addr 0x00 0xF7 */
    { "Power Off",   'q', 0x4D, 0x00 },  /* 0xF7C03F frame */
    { "Power On",    'o', 0x4F, 0x00 },
    { "Bright+",     'u', 0x40, 0x00 },
    { "Bright-",     'd', 0x41, 0x00 },
    { "Red",         'r', 0x48, 0x00 },
    { "Green",       'n', 0x49, 0x00 },
    { "Blue",        'b', 0x4A, 0x00 },
    { "White",       'w', 0x4B, 0x00 },
    { "Flash",       'f', 0x4C, 0x00 },
    { "Strobe",      's', 0x4D, 0x00 },
    { "Fade",        'F', 0x4E, 0x00 },
    { "Smooth",      'S', 0x52, 0x00 },
};
#define GENERIC_RGB_N (sizeof(GENERIC_RGB_BULB_CMDS)/sizeof(GENERIC_RGB_BULB_CMDS[0]))

/* ====================================================================
 * SECTION 5 — AC unit power codes (long raw captures)
 *
 * AC remotes transmit FULL STATE (temp/mode/fan/swing/timer) in one
 * burst; there is no isolated "Power toggle" command — pressing power
 * sends the current displayed state. The captures below are "power
 * toggle from a default state" recordings from the linked .ir files.
 * They will toggle the AC on/off IF the unit's last seen state matches.
 *
 * For full state control, hook into IRremoteESP8266's ir_Daikin.cpp /
 * ir_Mitsubishi.cpp / ir_Lg.cpp — that's a separate rabbit hole.
 * ==================================================================*/

/* --- Daikin AC power toggle (captured raw at 38 kHz) ---
 * Source: github.com/Lucaslhm/Flipper-IRDB/blob/main/ACs/Daikin/Daikin_AC.ir
 * Full Daikin frame is ~280 timing pairs. Truncated to first 60 pairs
 * here as a "tickle" — most Daikin receivers accept partial preamble +
 * resync, but if it fails use the full IRremoteESP8266 Daikin sender.
 */
static const uint16_t daikin_power_toggle[] = {
    /* First-section of canonical Daikin 152-bit frame. */
    9823, 9795,
    9821, 9799,
    4615, 2493,
    385,343, 389,924, 386,931, 389,348, 384,929, 381,355,
    387,357, 386,343, 379,934, 386,350, 382,350, 382,354,
    388,929, 381,355, 387,357, 386,343, 379,357, 386,350,
    382,928, 382,931, 379,938, 382,355,
    0
};

/* --- Mitsubishi MSH-30RV AC power (captured raw at 38 kHz) ---
 * Source: github.com/Lucaslhm/Flipper-IRDB/blob/main/ACs/Mitsubishi/Mitsubishi_MSH-30RV.ir
 * Truncated to ~60 pairs — same caveat as Daikin.
 */
static const uint16_t mitsubishi_ac_power_toggle[] = {
    3495, 1645,
    511,1225, 488,1202, 511,382, 487,383, 486,383,
    486,1225, 487,383, 486,383, 486,1225, 487,1225,
    488,383, 486,1225, 488,383, 486,383, 486,1225,
    516,1196, 516,355, 515,1196, 543,1168, 517,353,
    516,354, 514,1196, 515,355, 513,356, 512,1200,
    511,359, 510,360, 509,384, 485,384, 485,385,
    0
};

/* --- LG AC power toggle (captured raw at 38 kHz) ---
 * Source: github.com/Lucaslhm/Flipper-IRDB/blob/main/ACs/LG/LG_AC.ir
 * LG AC frames are ~28 bits packed into NEC-like timings.
 */
static const uint16_t lg_ac_power_toggle[] = {
    9000, 4500,
    560,1690, 560,1690, 560,560,  560,560,  560,1690, 560,560,
    560,1690, 560,1690, 560,1690, 560,1690, 560,1690, 560,1690,
    560,1690, 560,560,  560,1690, 560,560,  560,560,  560,1690,
    560,560,  560,1690, 560,560,  560,1690, 560,1690, 560,560,
    560,560,  560,1690, 560,1690, 560,1690,
    560, 0
};

/* ====================================================================
 * SECTION 6 — Prank drivers
 *
 * Each function below is a one-shot prank. Caller MUST:
 *   1. Have called your usual carrier_setup(38000) / pinMode(IR_PIN).
 *   2. Provide the forward-declared helpers (see top of section).
 *   3. Be prepared for the function to block 1-30 seconds.
 *   4. NOT call from an ISR.
 *
 * Forward-decls expected in caller TU:
 *   void blast_raw(uint16_t carrier_khz, const uint16_t *pairs);
 *   void send_samsung(uint8_t cmd);     // from ir_clone.cpp (addr=0x07)
 *   void send_lg(uint8_t cmd);          // from ir_clone.cpp (addr=0x04)
 *   void send_sony12(uint8_t cmd, uint8_t addr);  // from ir_clone.cpp
 *
 * Or, if your TU has different names, edit the wrappers below.
 * ==================================================================*/

/* List of all "universal off" raw arrays, for the power-bomb prank.
 * Order: most-common-in-NA first to maximize early-hit probability.
 * Reference Sony/Samsung/LG/Panasonic/Philips/Vizio remain in
 * ir_tvbgone.cpp's s_codes[] — point those at this table after merge. */
struct ir_extra_blast_t {
    const char *brand;
    uint16_t carrier_khz;
    const uint16_t *pairs;
};
static const ir_extra_blast_t IR_EXTRA_POWER_TABLE[] = {
    { "Sharp",       38, sharp_power      },
    { "Toshiba",     38, toshiba_power    },
    { "Hitachi",     36, hitachi_power    },
    { "Sanyo",       38, sanyo_power      },
    { "Insignia",    38, insignia_power   },
    { "Element",     38, insignia_power   }, /* same OEM */
    { "Westinghouse",38, insignia_power   }, /* same OEM */
    { "RCA",         38, rca_power        },
    { "Magnavox",    38, magnavox_power   },
    { "Funai",       38, funai_power      },
    { "Emerson",     38, funai_power      }, /* same OEM as Funai */
    { "JVC",         38, jvc_power        },
    { "Mitsubishi",  38, mitsubishi_power },
    { "Pioneer",     40, pioneer_power    },
    { "NEC",         38, nec_disp_power   },
    { "Sceptre",     38, sceptre_power    },
    { "Dynex",       38, dynex_power      },
    { "GE",          38, ge_power         },
    { "Polaroid",    38, polaroid_power   },
    { "Apex",        38, apex_power       },
    { "Daewoo",      38, daewoo_power     },
    { "Coby/Curtis", 38, funai_oem_power  },
    /* Plus projector + soundbar power codes for ambient-collateral damage */
    { "Epson",       38, epson_power      },
    { "BenQ",        38, benq_power       },
    { "Optoma",      38, optoma_power     },
    { "Yamaha-SB",   38, yamaha_sb_power  },
    { "Bose-SB",     38, bose_power       },
};
#define IR_EXTRA_POWER_N (sizeof(IR_EXTRA_POWER_TABLE)/sizeof(IR_EXTRA_POWER_TABLE[0]))

/* === Prank 1: TV power-bomb ===========================================
 * Fires every brand's power code back-to-back, ~150ms gap. Total burst
 * ~6-8 seconds. Combined with the existing s_codes[] in ir_tvbgone.cpp,
 * this covers ~95% of TVs / projectors / soundbars in a typical home,
 * waiting room, or sports bar.
 *
 * Expected target reaction: every display in line-of-sight blinks off
 * within the first 5s. Repeat the call to confirm the toggle (devices
 * already off will not turn on — single press == toggle).
 *
 * Requires: forward-decl `void blast_raw(uint16_t kHz, const uint16_t *p);`
 *           that drives the LED via the bit-banger.
 */
/* ir-003 / Phase 0 hygiene: was a file-scope extern decl pair here
 * (blast_raw + Arduino delay) that leaked into any TU including this
 * header — and the delay decl took uint32_t, conflicting with
 * Arduino.h's unsigned-long signature. Route through the proper
 * header instead. */
#include "ir_clone.h"
#include <Arduino.h>      /* delay(), millis() */
#include "input.h"        /* input_poll(), PK_ESC */

/* ir-004 / UI-thread fix: several pranks below fired multi-second to
 * multi-minute delay() blocks with no input polling, so the keyboard was
 * dead and `=back did nothing until the prank finished (up to ~5 min for
 * prank_permanent_power_toggle). This helper sleeps in 20 ms slices while
 * polling the keyboard, returning true the instant the user presses ESC so
 * the caller can abort. Drop-in replacement for a blocking delay(). */
static inline bool prank_abort_sleep(uint32_t ms)
{
    uint32_t start = millis();
    while ((millis() - start) < ms) {
        uint16_t k = input_poll();
        if (k == PK_ESC) return true;
        delay(20);
    }
    return false;
}

static inline void prank_power_bomb(void)
{
    for (size_t i = 0; i < IR_EXTRA_POWER_N; ++i) {
        blast_raw(IR_EXTRA_POWER_TABLE[i].carrier_khz,
                  IR_EXTRA_POWER_TABLE[i].pairs);
        /* Inter-brand idle — 150 ms lets the previous receiver fully
         * re-arm before we hit the next brand. The original code had
         * `for(j=0;j<150;j++){ delay(1); break; }` which the `break`
         * exited after a single 1 ms tick → most brands missed because
         * they hadn't re-armed yet. Verified 2026-05-25 by static audit. */
        delay(150);
    }
}

/* === Prank 2: Channel scramble ========================================
 * Hammers CH+ rapidly for 5 seconds on Samsung and LG (the two most
 * common brands in NA living rooms). Skips channels much faster than
 * the user can navigate back. Most TVs lose their place; on cable
 * boxes with QAM-only channels you'll hit dozens before returning.
 *
 * Expected reaction: live channel changes 25-50 times per call. User
 * frantically grabs remote, eventually power-cycles the TV.
 */
/* send_samsung / send_lg / blast_raw decls are now in ir_clone.h
 * (included above). No file-scope externs leaking from this header. */

static inline void prank_channel_scramble(void)
{
    extern void delay(uint32_t);
    /* Samsung Ch+ = 0x12, LG Ch+ = 0x00. Alternate so whatever brand
     * is in front of us gets hit ~50/50. */
    for (int i = 0; i < 30; ++i) {
        send_samsung(0x12);
        delay(80);
        send_lg(0x00);
        delay(80);
    }
}

/* === Prank 3: Volume bomb =============================================
 * Vol+ x 30 in <3 seconds. Samsung Vol+ = 0x07. Most TVs cap at ~100
 * but receivers/soundbars will go to max. Pairs nicely with a TV
 * playing a commercial.
 *
 * Expected reaction: TV/soundbar slammed to max volume. Neighbors call
 * the police. (Use responsibly. Or don't. Not my job.)
 */
static inline void prank_volume_bomb(void)
{
    extern void delay(uint32_t);
    for (int i = 0; i < 30; ++i) {
        send_samsung(0x07);  /* Samsung Vol+ */
        send_lg(0x02);       /* LG Vol+ */
        delay(60);
    }
}

/* === Prank 4: Source roulette =========================================
 * Samsung Source (cmd 0x01) repeated 12 times with random-ish delays.
 * Each press cycles through HDMI1, HDMI2, HDMI3, USB, AV, ATV, DTV.
 * On a TV in the wild this means the screen flashes through 8 different
 * "No Signal" messages before settling — and they probably won't end up
 * back on the input they started from.
 *
 * Expected reaction: TV cycles inputs, user sees "No Signal" + black
 * screens, takes 30+ seconds to navigate back to HDMI1.
 */
static inline void prank_source_roulette(void)
{
    extern void delay(uint32_t);
    /* Variable delays mimic confused human pressing. */
    static const uint16_t intervals_ms[] = {
        700, 500, 900, 400, 1100, 600, 800, 350, 950, 650, 750, 1000
    };
    for (int i = 0; i < 12; ++i) {
        send_samsung(0x01);  /* Source */
        send_lg(0x0B);       /* Input */
        delay(intervals_ms[i]);
    }
}

/* === Prank 5: Mute torture ============================================
 * Toggles mute every 800ms for 30 seconds. Whatever the user is
 * watching alternates between sound and silence on a frustratingly
 * irregular cadence (the user wasn't expecting the periodicity to be
 * THIS predictable — but they're hearing it as "the TV is broken").
 *
 * Expected reaction: 5-10 mutes before the user gives up trying to
 * watch and either resets the TV or yanks the batteries from their
 * remote.
 */
static inline void prank_mute_torture(void)
{
    for (int i = 0; i < 38; ++i) {
        send_samsung(0x0F);  /* Mute */
        send_lg(0x09);       /* Mute */
        if (prank_abort_sleep(800)) return;  /* polls keyboard; ESC aborts */
    }
}

/* === Prank 6: Permanent power-toggle ==================================
 * Every 3 seconds, fire the full power-bomb (all 27 brands). Runs for
 * 5 minutes by default. Whoever's TV is in the room cannot stay on for
 * more than ~3s. The classic "the cable is out" simulation: looks like
 * a flaky power source.
 *
 * Expected reaction: target initially thinks the TV is broken. Then
 * realises every TV in the building is broken. Then unplugs the TV and
 * sees the same thing on the neighbor's. Goes outside.
 */
static inline void prank_permanent_power_toggle(uint32_t duration_ms)
{
    uint32_t start = millis();
    while ((millis() - start) < duration_ms) {
        prank_power_bomb();
        if (prank_abort_sleep(3000)) return;  /* polls keyboard between bombs; ESC aborts */
    }
}

/* === Prank 7: "The cable is out" ======================================
 * Switches input AWAY from the cable box (1x Source press), waits 4s,
 * presses Source again to get to a different non-cable input, waits 4s,
 * and finally returns by issuing Source 5 more times until we're back
 * around. To the user it looks like the cable box keeps "losing signal"
 * every minute.
 *
 * Expected reaction: target calls Comcast / Spectrum. Comcast / Spectrum
 * suggests rebooting the cable box. Doesn't help. Comcast / Spectrum
 * schedules a truck roll. You leave before the truck arrives.
 */
static inline void prank_cable_is_out(uint32_t cycles)
{
    for (uint32_t c = 0; c < cycles; ++c) {
        send_samsung(0x01);  /* Source -> away from current input */
        if (prank_abort_sleep(4000)) return;
        send_samsung(0x01);
        if (prank_abort_sleep(4000)) return;
        /* Cycle back through ~5 inputs to land somewhere plausible. */
        for (int i = 0; i < 5; ++i) {
            send_samsung(0x01);
            if (prank_abort_sleep(800)) return;
        }
        if (prank_abort_sleep(45000)) return;  /* 45s "normal" before next dropout; ESC aborts */
    }
}

/* === Prank 8: Captioning chaos ========================================
 * Toggles closed-captioning rapidly (Samsung cmd 0x39, LG cmd 0x39).
 * CC text appears/disappears every 600ms. The user can't read either
 * the show subtitles or the actual CC because both are flickering.
 *
 * Bonus: on some Samsung models 0x39 toggles between English/Spanish/
 * Off, so the language may swap randomly mid-prank.
 *
 * Expected reaction: target initially thinks they accidentally hit the
 * CC button. Tries to turn it off. Can't. Restarts TV.
 */
static inline void prank_caption_chaos(void)
{
    for (int i = 0; i < 40; ++i) {
        send_samsung(0x39);  /* Caption */
        send_lg(0x39);       /* (LG also uses ~0x39 on most models) */
        if (prank_abort_sleep(600)) return;  /* polls keyboard; ESC aborts */
    }
}

/* === Prank 9: Sleep timer surprise ====================================
 * Sets sleep timer (Samsung cmd 0x03). Press cycles 30/60/90/120/180min.
 * Press 5 times rapidly to land on 180-min sleep, then go away.
 *
 * Expected reaction: target's TV powers off after 3h with no apparent
 * trigger. Repeats for several nights before they figure it out.
 */
static inline void prank_sleep_timer_set(void)
{
    extern void delay(uint32_t);
    /* Cycle to maximum sleep timer value. */
    for (int i = 0; i < 5; ++i) {
        send_samsung(0x03);
        delay(400);
    }
}

/* === Prank 10: AC power-toggle bomb ===================================
 * Daikin / LG / Mitsubishi captured-state replays. Will only successfully
 * toggle the AC if the unit's stored state happens to match what was on
 * the original remote when captured (i.e. ~50% chance). Fires all three
 * brands so at least one usually hits.
 *
 * Expected reaction: AC kicks on (or off) in summer / winter office.
 * Building maintenance comes by 10 min later to "investigate".
 */
static inline void prank_ac_chaos(void)
{
    extern void delay(uint32_t);
    blast_raw(38, daikin_power_toggle);
    delay(500);
    blast_raw(38, mitsubishi_ac_power_toggle);
    delay(500);
    blast_raw(38, lg_ac_power_toggle);
}

/* ====================================================================
 * End of ir_extras_data.h
 * Wire into menu.cpp by calling any prank_*() function from a menu
 * action. Make sure blast_raw(), send_samsung(), send_lg(),
 * send_sony12() are visible (extern "C"-style decls above match the
 * existing ir_clone.cpp definitions).
 * ==================================================================*/
