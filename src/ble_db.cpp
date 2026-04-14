/*
 * ble_db.cpp — curated identification tables.
 *
 * Coverage: top ~200 consumer electronics OUIs, the entire Apple
 * Continuity proximity-pairing subtype map, common Fast Pair models,
 * Bluetooth SIG assigned service/characteristic UUIDs.
 *
 * Tables live in flash (PROGMEM via const) — no RAM cost at rest.
 */
#include "ble_db.h"
#include <string.h>
#include <stdio.h>

/* ===================== OUI (top 200 consumer) ===================== */

struct oui_t { uint32_t oui; const char *vendor; };

static const oui_t OUI[] = {
    /* Apple */
    { 0x000393, "Apple" }, { 0x0003FF, "Apple" }, { 0x001124, "Apple" },
    { 0x001451, "Apple" }, { 0x001B63, "Apple" }, { 0x001EC2, "Apple" },
    { 0x001F5B, "Apple" }, { 0x001FF3, "Apple" }, { 0x0021E9, "Apple" },
    { 0x002241, "Apple" }, { 0x00236C, "Apple" }, { 0x0023DF, "Apple" },
    { 0x002500, "Apple" }, { 0x0025BC, "Apple" }, { 0x002608, "Apple" },
    { 0x00264A, "Apple" }, { 0x0026B0, "Apple" }, { 0x0026BB, "Apple" },
    { 0x003065, "Apple" }, { 0x003EE1, "Apple" }, { 0x0050E4, "Apple" },
    { 0x006171, "Apple" }, { 0x040CCE, "Apple" }, { 0x04489A, "Apple" },
    { 0x04DB56, "Apple" }, { 0x04F13E, "Apple" }, { 0x04F7E4, "Apple" },
    { 0x086698, "Apple" }, { 0x085BD6, "Apple" }, { 0x0C3021, "Apple" },
    { 0x0C3E9F, "Apple" }, { 0x0C4DE9, "Apple" }, { 0x0C7435, "Apple" },
    { 0x0C771A, "Apple" }, { 0x10417F, "Apple" }, { 0x10417F, "Apple" },
    { 0x1040F3, "Apple" }, { 0x109ADD, "Apple" }, { 0x14109F, "Apple" },
    { 0x14205E, "Apple" }, { 0x28CFE9, "Apple" }, { 0x28E02C, "Apple" },
    { 0x3451C9, "Apple" }, { 0x38CADA, "Apple" }, { 0x3C1598, "Apple" },
    { 0x3CAB8E, "Apple" }, { 0x406C8F, "Apple" }, { 0x40A6D9, "Apple" },
    { 0x40B395, "Apple" }, { 0x442A60, "Apple" }, { 0x448500, "Apple" },
    { 0x44D884, "Apple" }, { 0x4C3275, "Apple" }, { 0x4C57BE, "Apple" },
    { 0x4C7C5F, "Apple" }, { 0x4C8D79, "Apple" }, { 0x4CB199, "Apple" },
    { 0x4CAB4F, "Apple" },

    /* Samsung */
    { 0x0007AB, "Samsung" }, { 0x00124F, "Samsung" }, { 0x001377, "Samsung" },
    { 0x001485, "Samsung" }, { 0x0015B9, "Samsung" }, { 0x0016DB, "Samsung" },
    { 0x00175C, "Samsung" }, { 0x001A8A, "Samsung" }, { 0x001B98, "Samsung" },
    { 0x001D25, "Samsung" }, { 0x001DF6, "Samsung" }, { 0x002454, "Samsung" },
    { 0x002490, "Samsung" }, { 0x0024E9, "Samsung" }, { 0x002566, "Samsung" },
    { 0x002567, "Samsung" }, { 0x0026E3, "Samsung" }, { 0x0C7156, "Samsung" },
    { 0x0C8910, "Samsung" }, { 0x0CDFA4, "Samsung" }, { 0x0CFEA8, "Samsung" },
    { 0x10D38A, "Samsung" }, { 0x14320E, "Samsung" }, { 0x1485E8, "Samsung" },
    { 0x14B484, "Samsung" }, { 0x18227E, "Samsung" }, { 0x182666, "Samsung" },
    { 0x1C3E84, "Samsung" }, { 0x1C62B8, "Samsung" }, { 0x1C66AA, "Samsung" },
    { 0xE8617E, "Samsung" }, { 0xDCD3A2, "Samsung" },

    /* Google */
    { 0x001A11, "Google" }, { 0x34DB9C, "Google" }, { 0x405BD8, "Google" },
    { 0x6466B3, "Google" }, { 0xA4DA32, "Google" }, { 0x70BBE9, "Google" },
    { 0xF88FCA, "Google" },

    /* Microsoft */
    { 0x000D3A, "Microsoft" }, { 0x00125A, "Microsoft" }, { 0x000F1F, "Microsoft" },
    { 0x7C1E52, "Microsoft" }, { 0x282124, "Microsoft" },

    /* Sony */
    { 0x0013A9, "Sony" }, { 0x00146C, "Sony" }, { 0x001A80, "Sony" },
    { 0x001DBA, "Sony" }, { 0xFCF152, "Sony" }, { 0x94DB56, "Sony" },

    /* Bose */
    { 0x0452C7, "Bose" }, { 0x4C875D, "Bose" }, { 0x7CB566, "Bose" },
    { 0xAC3613, "Bose" }, { 0xBC45EA, "Bose" },

    /* JBL / Harman */
    { 0x00025B, "JBL" }, { 0x043389, "JBL" }, { 0x04216D, "JBL" },

    /* Logitech */
    { 0x000946, "Logitech" }, { 0x00204A, "Logitech" }, { 0x002438, "Logitech" },
    { 0x34885D, "Logitech" }, { 0x5C314E, "Logitech" }, { 0xC8F733, "Logitech" },

    /* Tile */
    { 0x006D52, "Tile" }, { 0x0CAE7D, "Tile" }, { 0x3CA308, "Tile" },

    /* Amazon */
    { 0x0C470C, "Amazon" }, { 0x18F650, "Amazon" }, { 0x1CFEA7, "Amazon" },
    { 0x34D270, "Amazon" }, { 0x38F73D, "Amazon" }, { 0x40B4CD, "Amazon" },
    { 0x44650D, "Amazon" }, { 0x50DCE7, "Amazon" }, { 0x68DB54, "Amazon" },

    /* Xiaomi / Redmi */
    { 0x001A11, "Xiaomi" }, { 0x04CF8C, "Xiaomi" }, { 0x08D8FE, "Xiaomi" },
    { 0x0C1DAF, "Xiaomi" }, { 0x14F65A, "Xiaomi" }, { 0x28E31F, "Xiaomi" },
    { 0x2C36F8, "Xiaomi" }, { 0x34CE00, "Xiaomi" }, { 0x3480B3, "Xiaomi" },
    { 0x38A28C, "Xiaomi" }, { 0x485D36, "Xiaomi" }, { 0x50EC50, "Xiaomi" },

    /* Huawei */
    { 0x001882, "Huawei" }, { 0x00259E, "Huawei" }, { 0x1C6F65, "Huawei" },
    { 0x4019E5, "Huawei" }, { 0x484A21, "Huawei" }, { 0x504200, "Huawei" },

    /* Intel / network */
    { 0x000E35, "Intel" }, { 0x00133B, "Intel" }, { 0x0015FF, "Intel" },
    { 0x001DE0, "Intel" }, { 0x001E64, "Intel" }, { 0x001E65, "Intel" },
    { 0x00215A, "Intel" }, { 0x00216A, "Intel" }, { 0x002436, "Intel" },
    { 0x0026C6, "Intel" }, { 0x002722, "Intel" }, { 0x34E6AD, "Intel" },

    /* Cisco */
    { 0x001422, "Cisco" }, { 0x001560, "Cisco" }, { 0x0019E7, "Cisco" },
    { 0x001DA2, "Cisco" }, { 0x002655, "Cisco" }, { 0x0026CA, "Cisco" },

    /* Espressif (ESP32 devices) */
    { 0x246F28, "Espressif" }, { 0x3C71BF, "Espressif" }, { 0x7C9EBD, "Espressif" },
    { 0x84F703, "Espressif" }, { 0x8CAAB5, "Espressif" }, { 0x94B97E, "Espressif" },
    { 0xA848FA, "Espressif" }, { 0xAC67B2, "Espressif" }, { 0xB4E62D, "Espressif" },
    { 0xBCDDC2, "Espressif" }, { 0xD8A01D, "Espressif" }, { 0xE8DB84, "Espressif" },
    { 0xEC94CB, "Espressif" },

    /* Raspberry Pi */
    { 0x2CCF67, "RPi" }, { 0xB827EB, "RPi" }, { 0xDCA632, "RPi" },
    { 0xE45F01, "RPi" },

    /* Dell, HP, Lenovo */
    { 0x0017A4, "Dell" }, { 0x0021B7, "Dell" }, { 0x2C44FD, "Dell" },
    { 0x8C85C1, "Dell" }, { 0xF8BC12, "Dell" },
    { 0x0017A4, "HP" }, { 0x00306E, "HP" }, { 0x1CC1DE, "HP" },
    { 0x000E7F, "HP" }, { 0xEC8EB5, "HP" },
    { 0x002481, "Lenovo" }, { 0x5C93A2, "Lenovo" },

    /* Nintendo / Sony PS */
    { 0x00024C, "Nintendo" }, { 0x001A1A, "Nintendo" }, { 0x001BEA, "Nintendo" },
    { 0x002DE2, "Nintendo" }, { 0x00FA9D, "Nintendo" },
    { 0x0013A9, "Sony PS" }, { 0xFC0FE6, "Sony PS" }, { 0xF8461C, "Sony PS" },

    /* Misc consumer */
    { 0x001788, "Fitbit" }, { 0x0021E8, "Garmin" }, { 0x0C8CDC, "Garmin" },
    { 0x28F076, "GoPro" }, { 0x04C5A4, "GoPro" },
    { 0x988BAD, "Oculus" },  { 0x10B7F6, "Oculus" },
    { 0x24DF6A, "Amazfit" }, { 0x049226, "Amazfit" },

    { 0, nullptr }
};

/* ================== Apple Continuity subtype map ================== */

/* data[0..1] = 0x4C 0x00, data[2] = subtype.
 *   0x02 = iBeacon
 *   0x05 = AirDrop
 *   0x06 = AirPlay
 *   0x07 = Proximity Pairing (AirPods etc, data[3] = model byte)
 *   0x09 = Setup (AppleTV / HomePod)
 *   0x0A = AirPrint
 *   0x0C = Handoff
 *   0x10 = Nearby (iPhone, Apple Watch)
 *   0x12 = Find My (AirTag)
 */

struct apple_model_t { uint8_t key; const char *name; };

static const apple_model_t APPLE_PP[] = {
    { 0x01, "AirPods 1" }, { 0x02, "AirPods Pro" }, { 0x03, "AirPods Max" },
    { 0x04, "AppleTV Setup" }, { 0x05, "Beats X" }, { 0x06, "Beats Solo 3" },
    { 0x07, "Beats Studio 3" }, { 0x09, "Beats Studio Pro" }, { 0x0A, "Beats Fit Pro" },
    { 0x0B, "Beats Flex / Vision Pro" }, { 0x0C, "Beats Solo Pro" },
    { 0x0D, "Beats Studio Buds" }, { 0x0E, "Beats Studio Buds+" },
    { 0x0F, "AirPods 2" }, { 0x10, "AirPods 3" }, { 0x11, "AirPods 4" },
    { 0x13, "AirPods Pro 2" }, { 0x14, "AirPods Pro 2 (USB-C)" },
    { 0x19, "PowerBeats Pro" }, { 0x1A, "Beats Fit Pro 2" },
    { 0, nullptr }
};

const char *ble_db_apple(uint8_t subtype, uint8_t sub2)
{
    if (subtype == 0x02) return "iBeacon";
    if (subtype == 0x05) return "AirDrop";
    if (subtype == 0x06) return "AirPlay";
    if (subtype == 0x09) return "Apple Setup";
    if (subtype == 0x0A) return "AirPrint";
    if (subtype == 0x0C) return "Handoff";
    if (subtype == 0x10) return "Nearby (iPhone)";
    if (subtype == 0x12) return "AirTag";
    if (subtype == 0x07) {
        for (const apple_model_t *p = APPLE_PP; p->name; ++p)
            if (p->key == sub2) return p->name;
        return "Apple pairing";
    }
    return nullptr;
}

/* =============== Fast Pair 24-bit model IDs ================ */

struct fp_t { uint32_t id; const char *name; };

static const fp_t FP[] = {
    { 0x000000, "FastPair test" },
    { 0x00000F, "Pixel Buds A" },
    { 0x0000F0, "Pixel Buds" },
    { 0x001E13, "Pixel Buds Pro" },
    { 0x00B727, "JBL Live 650" },
    { 0x00E16A, "JBL Live 200" },
    { 0x00F520, "Sony WH-1000XM4" },
    { 0x09344E, "Sony WF-1000XM4" },
    { 0x00E4D9, "Sony LinkBuds" },
    { 0x0000E4, "Bose QC35" },
    { 0x00B15E, "Bose QC45" },
    { 0x0006DE, "Anker Soundcore" },
    { 0x00035F, "JBL Flip 6" },
    { 0, nullptr }
};

const char *ble_db_fastpair(uint32_t model24)
{
    for (const fp_t *p = FP; p->name; ++p)
        if (p->id == (model24 & 0xFFFFFF)) return p->name;
    return nullptr;
}

/* =============== Bluetooth SIG assigned UUIDs ================ */

struct uuid_name_t { uint16_t uuid; const char *name; };

static const uuid_name_t SVC_UUIDS[] = {
    { 0x1800, "Generic Access" },
    { 0x1801, "Generic Attribute" },
    { 0x1802, "Immediate Alert" },
    { 0x1803, "Link Loss" },
    { 0x1804, "Tx Power" },
    { 0x1805, "Current Time" },
    { 0x1806, "Reference Time Update" },
    { 0x1808, "Glucose" },
    { 0x1809, "Health Thermometer" },
    { 0x180A, "Device Information" },
    { 0x180D, "Heart Rate" },
    { 0x180E, "Phone Alert Status" },
    { 0x180F, "Battery" },
    { 0x1810, "Blood Pressure" },
    { 0x1811, "Alert Notification" },
    { 0x1812, "HID" },
    { 0x1813, "Scan Parameters" },
    { 0x1814, "Running Speed / Cadence" },
    { 0x1816, "Cycling Speed / Cadence" },
    { 0x1818, "Cycling Power" },
    { 0x1819, "Location / Navigation" },
    { 0x181A, "Environmental Sensing" },
    { 0x181B, "Body Composition" },
    { 0x181C, "User Data" },
    { 0x181D, "Weight Scale" },
    { 0x181E, "Bond Management" },
    { 0x181F, "Continuous Glucose" },
    { 0x1820, "Internet Protocol Support" },
    { 0x1822, "Pulse Oximeter" },
    { 0x1826, "Fitness Machine" },
    { 0x183A, "Insulin Delivery" },
    { 0xFE2C, "Google Fast Pair" },
    { 0xFD6F, "Exposure Notification" },
    { 0xFEED, "Tile" },
    { 0xFD84, "Tile" },
    { 0xFDF0, "Samsung SmartThings" },
    { 0xFDAB, "Nordic UART" },
    { 0, nullptr }
};

static const uuid_name_t CHR_UUIDS[] = {
    { 0x2A00, "Device Name" },
    { 0x2A01, "Appearance" },
    { 0x2A04, "PPCP" },
    { 0x2A19, "Battery Level" },
    { 0x2A23, "System ID" },
    { 0x2A24, "Model Number" },
    { 0x2A25, "Serial Number" },
    { 0x2A26, "Firmware Revision" },
    { 0x2A27, "Hardware Revision" },
    { 0x2A28, "Software Revision" },
    { 0x2A29, "Manufacturer Name" },
    { 0x2A2A, "IEEE 11073 Cert" },
    { 0x2A2B, "Current Time" },
    { 0x2A37, "Heart Rate Measurement" },
    { 0x2A38, "Body Sensor Location" },
    { 0x2A39, "Heart Rate Control Point" },
    { 0x2A4A, "HID Information" },
    { 0x2A4B, "Report Map" },
    { 0x2A4C, "HID Control Point" },
    { 0x2A4D, "Report" },
    { 0x2A4E, "Protocol Mode" },
    { 0x2A50, "PnP ID" },
    { 0x2A5B, "CSC Measurement" },
    { 0x2A63, "Cycling Power Measurement" },
    { 0x2A6D, "Pressure" },
    { 0x2A6E, "Temperature" },
    { 0x2A6F, "Humidity" },
    { 0x2A19, "Battery Level" },
    { 0, nullptr }
};

const char *ble_db_svc_uuid(uint16_t u) {
    for (const uuid_name_t *p = SVC_UUIDS; p->name; ++p)
        if (p->uuid == u) return p->name;
    return nullptr;
}
const char *ble_db_chr_uuid(uint16_t u) {
    for (const uuid_name_t *p = CHR_UUIDS; p->name; ++p)
        if (p->uuid == u) return p->name;
    return nullptr;
}

const char *ble_db_oui(uint32_t oui24)
{
    for (const oui_t *p = OUI; p->vendor; ++p)
        if (p->oui == (oui24 & 0xFFFFFF)) return p->vendor;
    return nullptr;
}

bool ble_db_identify(const uint8_t *addr,
                     const uint8_t *mfg, int mfg_len,
                     char *out, int out_sz)
{
    if (out_sz < 1) return false;
    out[0] = '\0';

    /* 1. Apple Continuity — most specific, beats OUI. */
    if (mfg && mfg_len >= 3 && mfg[0] == 0x4C && mfg[1] == 0x00) {
        uint8_t sub = mfg[2];
        uint8_t sub2 = (mfg_len > 3) ? mfg[3] : 0;
        const char *nm = ble_db_apple(sub, sub2);
        if (nm) { snprintf(out, out_sz, "%s", nm); return true; }
    }

    /* 2. Samsung manufacturer data. */
    if (mfg && mfg_len >= 3 && mfg[0] == 0x75 && mfg[1] == 0x00) {
        snprintf(out, out_sz, "Samsung");
        if (mfg_len >= 6 && mfg[2] == 0x42 && mfg[3] == 0x09) {
            snprintf(out, out_sz, "SmartTag");
        }
        return true;
    }

    /* 3. Microsoft Swift Pair. */
    if (mfg && mfg_len >= 3 && mfg[0] == 0x06 && mfg[1] == 0x00) {
        snprintf(out, out_sz, "MS Swift Pair");
        return true;
    }

    /* 4. Google mfg. */
    if (mfg && mfg_len >= 3 && mfg[0] == 0xE0 && mfg[1] == 0x00) {
        snprintf(out, out_sz, "Google");
        return true;
    }

    /* 5. OUI lookup for public MACs. */
    if (addr) {
        /* Skip random MACs: high 2 bits of MSB indicate type when random. */
        uint32_t oui = ((uint32_t)addr[0] << 16)
                     | ((uint32_t)addr[1] << 8)
                     | addr[2];
        const char *v = ble_db_oui(oui);
        if (v) { snprintf(out, out_sz, "%s", v); return true; }
    }
    return false;
}
