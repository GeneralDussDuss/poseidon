/*
 * ble_dult - non-owner interaction with Find My / DULT location trackers.
 *
 * Three protocol surfaces are implemented, because no public source can
 * say which one current Apple hardware still answers on:
 *
 *   AirTag legacy  7DFC9000-7D1C-4951-86AA-8D9728F8D66C
 *                  char 7DFC9001-...  write 0xAF (with response), no stop
 *   Apple FMN      0xFD44
 *                  char 4F860003-943B-49EF-BED4-2F730304427A
 *                  start 01 00 03   stop 01 01 03
 *   DULT standard  15190001-12F4-C226-88ED-2AC5579F2A85
 *                  char 8E0C0001-1D68-FB92-BF61-48377421680E
 *                  start 00 03 (Sound_Start 0x0300 LE)  stop 01 03
 *
 * Every attempt records WHICH surface answered, to serial and to
 * /poseidon/dult-log.csv. That log is the point: it is how we find out
 * whether AirTag 2 moved off the legacy service, which nobody has
 * published.
 *
 * The sound trigger is the anti-stalking mechanism Apple co-authored
 * into draft-ietf-dult-accessory-protocol. It exists so that a person
 * being followed can make the tracker audible. It only works while the
 * accessory is SEPARATED from its owner; near-owner accessories reject
 * Sound_Start with Invalid_command, so the separated state is decoded
 * from the advertisement and surfaced before the operator tries.
 *
 * Research backing every constant: .superpowers/sdd/2026-08-07-tembed-
 * bringup/airtag-sound-protocol.md
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

class NimBLEAdvertisedDevice;

enum dult_kind_t : uint8_t {
    DULT_KIND_UNKNOWN = 0,
    DULT_KIND_APPLE_FM,   /* Apple 0x004C type 0x12 - AirTag / AirPods /
                           * third-party Find My accessory. Cannot be told
                           * apart from a separated iPhone/Watch by advert
                           * alone; only accessories answer the sound path. */
    DULT_KIND_DULT,       /* generic DULT accessory (0xFCB2 service data) */
    DULT_KIND_GOOGLE,     /* Google Find My Device / Find Hub (Eddystone) */
    DULT_KIND_TILE,       /* no non-owner sound surface exists */
    DULT_KIND_SAMSUNG,    /* no non-owner sound surface exists */
};

enum dult_state_t : uint8_t {
    DULT_STATE_UNKNOWN = 0,
    DULT_STATE_SEPARATED,   /* owner out of range - sound is available */
    DULT_STATE_NEAR_OWNER,  /* owner in range - accessory WILL refuse */
};

enum dult_proto_t : uint8_t {
    DULT_PROTO_NONE = 0,
    DULT_PROTO_AIRTAG,
    DULT_PROTO_DULT,
    DULT_PROTO_FMN,
    DULT_PROTO_FMN_ALT,   /* FMN payload without the undocumented 0x01 */
};

struct dult_target_t {
    uint8_t      addr[6];
    bool         addr_public;
    dult_kind_t  kind;
    dult_state_t state;
    uint8_t      battery;   /* 0 full .. 3 very low, 0xFF unknown (Apple only) */
    int8_t       rssi;
    dult_proto_t hint;      /* pre-connect hint from advertised service UUIDs */
    char         label[12];
};

/* Classify an advertisement. Returns false when the device is not a
 * recognised tracker. Fills every field of *out. */
bool dult_classify(const NimBLEAdvertisedDevice *d, dult_target_t *out);

/* True only for device classes that actually expose a non-owner sound
 * control point. Tile and Samsung SmartTag do NOT - AirGuard deliberately
 * does not implement a connectable path for either, and there is no
 * protocol to implement. The UI must not offer sound for those. */
bool dult_kind_can_sound(dult_kind_t k);

const char *dult_proto_name(dult_proto_t p);
const char *dult_kind_name(dult_kind_t k);
const char *dult_state_name(dult_state_t s);

/* Progress sink. Set by the UI so each protocol step is visible live.
 * Every line is also written to serial with a [DULT] tag. */
typedef void (*dult_progress_fn)(const char *line);
void dult_set_progress(dult_progress_fn fn);

/* Blocking. Connects (3 attempts), walks the protocol priority list for
 * this device kind, and writes Sound_Start / Sound_Stop on the first
 * surface that is present. *answered gets the protocol that responded.
 * Returns true if a control point accepted the write. */
bool dult_sound(const dult_target_t *t, bool stop, dult_proto_t *answered,
                char *summary, size_t summary_sz);

struct dult_detail_t {
    bool any;
    char mfg[28];
    char model[28];
    char category[28];
    char fw[28];
    char netid[28];
    char batt[28];
};

/* Blocking. Silent enumeration over the DULT non-owner characteristic:
 * Get_Manufacturer_Name / Model_Name / Accessory_Category /
 * Firmware_Version / Network_ID / Battery_Level. Makes no noise. */
bool dult_read_details(const dult_target_t *t, dult_detail_t *out,
                       char *summary, size_t summary_sz);

/* Full-screen target view: identity, separated state, and the action
 * menu (encoder long-press). Stops nothing itself - the caller must have
 * stopped any active scan, because NimBLE cannot connect while a
 * discovery is running. */
void dult_target_screen(const dult_target_t *t);
