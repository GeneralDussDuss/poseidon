/*
 * mesh_nodes — live Meshtastic roster.
 *
 * Scrollable list of seen nodes: short name / node id / SNR / RSSI / hops /
 * last-seen / GPS pin indicator. ENTER on a node opens the page feature
 * targeting that node.
 */
#include "../app.h"
#include "../theme.h"
#include "../ui.h"
#include "../input.h"
#include "../radio.h"
#include "../mesh/meshtastic.h"
#include <stdio.h>

extern void feat_mesh_page_to(uint32_t dest);

void feat_mesh_nodes(void)
{
    radio_switch(RADIO_LORA);
    if (!mesh_begin()) {
        ui_toast("mesh init failed", T_BAD, 1500);
        radio_switch(RADIO_NONE);
        return;
    }

    int cursor = 0;
    ui_draw_footer(";/.=move  ENTER=page  I=info  `=back");

    while (true) {
        int count;
        const mesh_node_t *nodes = mesh_nodes(&count);

        auto &d = M5Cardputer.Display;
        ui_clear_body();
        d.setTextColor(T_ACCENT, T_BG);
        d.setCursor(4, BODY_Y + 2);
        d.printf("MESH NODES %d", count);
        d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT);

        if (count == 0) {
            d.setTextColor(T_DIM, T_BG);
            d.setCursor(4, BODY_Y + 30);
            d.print("no nodes seen yet");
            d.setCursor(4, BODY_Y + 42);
            d.print("waiting for mesh traffic...");
        } else {
            if (cursor < 0) cursor = 0;
            if (cursor >= count) cursor = count - 1;
            int rows = 7;
            int first = cursor - rows / 2;
            if (first < 0) first = 0;
            if (first + rows > count) first = count - rows;
            if (first < 0) first = 0;

            for (int r = 0; r < rows && first + r < count; r++) {
                const mesh_node_t &n = nodes[first + r];
                int y = BODY_Y + 16 + r * 12;
                bool sel = (first + r == cursor);
                if (sel) d.fillRect(0, y - 1, SCR_W, 12, T_SEL_BG);

                d.setTextColor(sel ? T_ACCENT : T_FG, sel ? T_SEL_BG : T_BG);
                d.setCursor(3, y);
                const char *name = n.short_name[0] ? n.short_name : "?";
                d.printf("%-4.4s", name);
                d.setTextColor(sel ? T_FG : T_DIM, sel ? T_SEL_BG : T_BG);
                d.setCursor(34, y); d.printf("!%08x", (unsigned int)n.id);
                d.setCursor(110, y); d.printf("%+3d", (int)n.last_snr);
                d.setCursor(130, y); d.printf("%3d", (int)n.last_rssi);
                uint32_t age = (millis() - n.last_seen_ms) / 1000;
                d.setCursor(156, y);
                if (age < 60)       d.printf("%2lus", (unsigned long)age);
                else if (age < 3600) d.printf("%2lum", (unsigned long)(age / 60));
                else                 d.printf("%2luh", (unsigned long)(age / 3600));
                d.setCursor(176, y); d.printf("h%d", (int)n.hops);
                if (n.has_position) {
                    d.setTextColor(T_GOOD, sel ? T_SEL_BG : T_BG);
                    d.setCursor(196, y); d.print("GPS");
                }
            }
        }

        mesh_tick();

        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(30); continue; }
        if (k == PK_ESC) break;
        if (k == ';' || k == PK_UP)   { if (cursor > 0) cursor--; }
        if (k == '.' || k == PK_DOWN) { if (cursor + 1 < count) cursor++; }
        if (k == PK_ENTER && count > 0) {
            feat_mesh_page_to(nodes[cursor].id);
        }
        if ((k == 'i' || k == 'I') && count > 0) {
            /* Info panel overlay — long name + lat/lon if present. */
            const mesh_node_t &n = nodes[cursor];
            d.fillRect(10, BODY_Y + 20, SCR_W - 20, BODY_H - 30, T_BG);
            d.drawRect(10, BODY_Y + 20, SCR_W - 20, BODY_H - 30, T_ACCENT);
            d.setTextColor(T_ACCENT, T_BG);
            d.setCursor(14, BODY_Y + 24); d.printf("!%08x", (unsigned int)n.id);
            d.setTextColor(T_FG, T_BG);
            d.setCursor(14, BODY_Y + 36); d.print(n.long_name[0] ? n.long_name : "<no name>");
            d.setCursor(14, BODY_Y + 48); d.printf("SNR %+d  RSSI %d", (int)n.last_snr, (int)n.last_rssi);
            d.setCursor(14, BODY_Y + 58); d.printf("hops %d", (int)n.hops);
            if (n.has_position) {
                d.setCursor(14, BODY_Y + 68);
                d.printf("%+8.4f", n.latitude_i / 1e7);
                d.setCursor(14, BODY_Y + 78);
                d.printf("%+8.4f", n.longitude_i / 1e7);
                d.setCursor(14, BODY_Y + 88);
                d.printf("alt %dm", (int)n.altitude);
            }
            d.setTextColor(T_DIM, T_BG);
            d.setCursor(14, BODY_Y + BODY_H - 18); d.print("any key to close");
            while (input_poll() == PK_NONE) delay(20);
        }
    }
    /* Stop RX task before returning — any other LoRa feature the user
     * opens next calls lora_begin() which frees the SX1262. If our RX
     * task is still running it would dereference a stale vtable and
     * panic with InstrFetchProhibited. */
    mesh_end();
}
