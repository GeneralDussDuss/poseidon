/*
 * panel_tembed - injects a T-Embed ST7789 panel into M5.Display.
 *
 * M5Cardputer.Display is a reference to M5.Display, and M5GFX::init takes a
 * Panel_Device*. Injecting our own panel therefore retargets every existing
 * M5Cardputer.Display call site without touching any of them.
 */
#pragma once

#if defined(POSEIDON_BOARD_TEMBED)
void tembed_display_init(void);
#endif
