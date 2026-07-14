#include "hid_decode.h"
#include <string.h>
#include <stdio.h>

// ---- HID item type/tag constants (USB HID 1.11, section 6.2.2) ------------
#define HID_TYPE_MAIN   0
#define HID_TYPE_GLOBAL 1
#define HID_TYPE_LOCAL  2

#define TAG_INPUT       0x8   // Main
#define TAG_COLLECTION  0xA   // Main
#define TAG_USAGE_PAGE  0x0   // Global
#define TAG_REPORT_SIZE 0x7   // Global
#define TAG_REPORT_ID   0x8   // Global
#define TAG_REPORT_CNT  0x9   // Global
#define TAG_USAGE       0x0   // Local

#define UP_GENERIC_DESKTOP 0x01
#define UP_KEYBOARD        0x07
#define USAGE_KEYBOARD     0x06

// Input item data-flag bits: bit0 = constant, bit1 = variable.
#define INPUT_CONSTANT 0x01
#define INPUT_VARIABLE 0x02

bool hid_parse_report_map(const uint8_t *map, int len, hid_layout_t *out) {
    out->is_keyboard     = false;
    out->report_id       = 0;
    out->modifier_offset = -1;
    out->key_offset      = -1;
    out->key_count       = 0;

    int      usage_page   = 0;
    int      report_size  = 0;
    int      report_count = 0;
    bool     have_kbd_top = false;   // saw Usage(Keyboard) on the desktop page
    int      base_off     = 0;       // 1 if a report ID prefixes the report
    int      input_bit    = 0;       // running bit position in the input report

    int i = 0;
    while (i < len) {
        uint8_t prefix = map[i++];
        int size = prefix & 0x03;
        if (size == 3) size = 4;            // 0,1,2,3 -> 0,1,2,4 data bytes
        int type = (prefix >> 2) & 0x03;
        int tag  = (prefix >> 4) & 0x0F;

        uint32_t data = 0;
        for (int b = 0; b < size && i < len; ++b) data |= (uint32_t)map[i++] << (8 * b);

        if (type == HID_TYPE_GLOBAL) {
            if      (tag == TAG_USAGE_PAGE)  usage_page   = (int)data;
            else if (tag == TAG_REPORT_SIZE) report_size  = (int)data;
            else if (tag == TAG_REPORT_CNT)  report_count = (int)data;
            else if (tag == TAG_REPORT_ID) { out->report_id = (int)data; base_off = 1; }
        } else if (type == HID_TYPE_LOCAL) {
            if (tag == TAG_USAGE && usage_page == UP_GENERIC_DESKTOP && data == USAGE_KEYBOARD)
                have_kbd_top = true;
        } else if (type == HID_TYPE_MAIN) {
            if (tag == TAG_INPUT) {
                bool constant  = (data & INPUT_CONSTANT) != 0;
                bool variable  = (data & INPUT_VARIABLE) != 0;
                bool kbd_page  = (usage_page == UP_KEYBOARD);
                int  byte_off  = base_off + input_bit / 8;

                if (kbd_page && variable && !constant && report_size == 1 &&
                    out->modifier_offset < 0) {
                    out->modifier_offset = byte_off;      // 8x1-bit modifier byte
                } else if (kbd_page && !variable && !constant && report_size == 8 &&
                           out->key_offset < 0) {
                    out->key_offset = byte_off;           // keycode array
                    out->key_count  = report_count;
                }
                input_bit += report_size * report_count;  // only Input advances
            }
            // Output/Feature main items belong to other reports: ignore here.
        }
    }

    out->is_keyboard = have_kbd_top && out->key_offset >= 0;
    return out->is_keyboard;
}

// ---- key naming -----------------------------------------------------------

static const char *MOD_NAMES[8] = {
    "LCtrl", "LShift", "LAlt", "LGui", "RCtrl", "RShift", "RAlt", "RGui"
};

// Fill `buf` with the name for a keyboard/keypad usage id (page 0x07).
static void key_name(uint8_t kc, char *buf, int sz) {
    if (kc >= 0x04 && kc <= 0x1D)      { buf[0] = (char)('A' + (kc - 0x04)); buf[1] = 0; return; }
    if (kc >= 0x1E && kc <= 0x26)      { buf[0] = (char)('1' + (kc - 0x1E)); buf[1] = 0; return; }
    if (kc == 0x27)                    { buf[0] = '0'; buf[1] = 0; return; }
    if (kc >= 0x3A && kc <= 0x45)      { snprintf(buf, sz, "F%d", kc - 0x3A + 1); return; }
    const char *n = 0;
    switch (kc) {
        case 0x28: n = "Enter";     break;
        case 0x29: n = "Esc";       break;
        case 0x2A: n = "Backspace"; break;
        case 0x2B: n = "Tab";       break;
        case 0x2C: n = "Space";     break;
        case 0x2D: n = "-";         break;
        case 0x2E: n = "=";         break;
        case 0x2F: n = "[";         break;
        case 0x30: n = "]";         break;
        case 0x31: n = "\\";        break;
        case 0x33: n = ";";         break;
        case 0x34: n = "'";         break;
        case 0x35: n = "`";         break;
        case 0x36: n = ",";         break;
        case 0x37: n = ".";         break;
        case 0x38: n = "/";         break;
        case 0x39: n = "CapsLock";  break;
        case 0x4F: n = "Right";     break;
        case 0x50: n = "Left";      break;
        case 0x51: n = "Down";      break;
        case 0x52: n = "Up";        break;
        default:   snprintf(buf, sz, "0x%02X", kc); return;
    }
    snprintf(buf, sz, "%s", n);
}

static void append_token(char *out, int out_sz, int *len, int *n, const char *tok) {
    int need = (*n > 0 ? 1 : 0) + (int)strlen(tok);
    if (*len + need + 1 > out_sz) return;   // leave room for NUL
    if (*n > 0) out[(*len)++] = '+';
    while (*tok) out[(*len)++] = *tok++;
    out[*len] = 0;
    (*n)++;
}

int hid_decode_keyboard(const hid_layout_t *layout, const uint8_t *report,
                        int rlen, char *out, int out_sz) {
    int len = 0, n = 0;
    if (out_sz > 0) out[0] = 0;
    if (!layout || !layout->is_keyboard) return 0;

    if (layout->modifier_offset >= 0 && layout->modifier_offset < rlen) {
        uint8_t mod = report[layout->modifier_offset];
        for (int b = 0; b < 8; ++b)
            if (mod & (1 << b)) append_token(out, out_sz, &len, &n, MOD_NAMES[b]);
    }

    for (int k = 0; k < layout->key_count; ++k) {
        int off = layout->key_offset + k;
        if (off < 0 || off >= rlen) break;
        uint8_t kc = report[off];
        if (kc == 0x00 || kc == 0x01) continue;   // none / ErrorRollOver
        char nb[12];
        key_name(kc, nb, sizeof(nb));
        append_token(out, out_sz, &len, &n, nb);
    }
    return n;
}
