/*
 * menu_icons.cpp — dispatch top-level menu hotkeys to bitmap icons.
 *
 * Bitmap data is auto-generated from assets/icons.jpg by
 * scripts/convert_icons.py. To re-run the conversion (e.g. after
 * tweaking the icon sheet):
 *
 *   python scripts/convert_icons.py
 *
 * which rewrites src/menu_icons_data.h.
 */
#include "menu_icons.h"
#include "menu_icons_data.h"
#include <M5Cardputer.h>

extern const menu_node_t MENU_ROOT;

int menu_icon_w(void) { return MENU_ICON_W; }
int menu_icon_h(void) { return MENU_ICON_H; }

const uint8_t *menu_icon_bitmap_for_hotkey(char hotkey)
{
    switch (hotkey) {
    case 'w': return MENU_ICON_WIFI;
    case 'b': return MENU_ICON_BLE;
    case 'i': return MENU_ICON_IR;
    case 't': return MENU_ICON_TRIDENT;
    case 'u': return MENU_ICON_USB;
    case 'n': return MENU_ICON_NETWORK;
    case 'j': return MENU_ICON_SKULL;
    case 'r': return MENU_ICON_RADIO;
    case 'o': return MENU_ICON_TOOLS;
    case 'm': return MENU_ICON_MESH;
    case '5': return MENU_ICON_SATELLITE;
    case 'x': return MENU_ICON_EYE;
    case 'p': return MENU_ICON_LAPTOP;
    case 's': return MENU_ICON_GEAR;
    default:  return nullptr;
    }
}

bool draw_menu_icon(int cx, int cy, uint16_t color,
                    const menu_node_t *parent, const menu_node_t *item)
{
    if (parent != &MENU_ROOT) return false;
    if (!item) return false;

    const uint8_t *bmp = menu_icon_bitmap_for_hotkey(item->hotkey);
    if (!bmp) return false;

    auto &d = M5Cardputer.Display;
    /* drawBitmap origin is top-left; we want the icon centered on
     * (cx, cy) — offset by half the bitmap dimensions. */
    int x = cx - (MENU_ICON_W / 2);
    int y = cy - (MENU_ICON_H / 2);
    d.drawBitmap(x, y, bmp, MENU_ICON_W, MENU_ICON_H, color);
    return true;
}
