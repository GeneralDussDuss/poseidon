/*
 * mesh_page — send a direct (unicast) Meshtastic text message to a
 * specific node. Two entry paths:
 *   1. feat_mesh_page()         — pick node from roster, then type
 *   2. feat_mesh_page_to(node)  — roster already picked us a node, skip
 *                                  straight to the type screen
 */
#include "../app.h"
#include "../theme.h"
#include "../ui.h"
#include "../input.h"
#include "../radio.h"
#include "../mesh/meshtastic.h"
#include <stdio.h>
#include <string.h>

static void page_to(uint32_t dest)
{
    char input[128] = {0};
    int input_len = 0;
    bool dirty = true;

    auto &d = M5Cardputer.Display;
    ui_draw_footer("ENTER=send  `=cancel");

    while (true) {
        if (dirty) {
            ui_clear_body();
            d.setTextColor(T_ACCENT, T_BG);
            d.setCursor(4, BODY_Y + 2); d.print("PAGE");
            d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT);

            d.setTextColor(T_FG, T_BG);
            d.setCursor(4, BODY_Y + 18);
            d.printf("to !%08x", (unsigned int)dest);

            d.setTextColor(T_DIM, T_BG);
            d.setCursor(4, BODY_Y + 34); d.print("message:");

            d.setTextColor(T_FG, T_BG);
            d.setCursor(4, BODY_Y + 48);
            /* Word-wrap at ~30 chars/line */
            int col = 0, row = 0;
            for (int i = 0; i < input_len; i++) {
                d.setCursor(4 + col * 6, BODY_Y + 48 + row * 10);
                d.print(input[i]);
                col++;
                if (col >= 36) { col = 0; row++; if (row > 3) break; }
            }
            d.setCursor(4 + col * 6, BODY_Y + 48 + row * 10);
            d.print('_');

            d.setTextColor(T_DIM, T_BG);
            d.setCursor(4, BODY_Y + BODY_H - 14);
            d.printf("%d/%d chars", input_len, (int)(sizeof(input) - 1));
            dirty = false;
        }
        mesh_tick();

        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) return;
        if (k == PK_ENTER) {
            if (input_len == 0) continue;
            if (mesh_send_direct_text(dest, input)) {
                ui_toast("paged", T_GOOD, 600);
            } else {
                ui_toast("TX failed", T_BAD, 800);
            }
            return;
        }
        if (k == PK_BKSP) {
            if (input_len > 0) { input[--input_len] = 0; dirty = true; }
            continue;
        }
        if (k >= ' ' && k < 0x80 && input_len < (int)sizeof(input) - 1) {
            input[input_len++] = (char)k;
            input[input_len] = 0;
            dirty = true;
        }
    }
}

/* Called from mesh_nodes after a node is selected.
 * Note: does NOT tear down mesh on exit because mesh_nodes (the caller)
 * is still running and needs the RX task alive. mesh_nodes calls
 * mesh_end() itself on its own exit. */
void feat_mesh_page_to(uint32_t dest)
{
    if (dest == 0 || dest == mesh_own_node_id()) return;
    page_to(dest);
}

/* Top-level menu entry: prompt for hex node id, then page. */
void feat_mesh_page(void)
{
    radio_switch(RADIO_LORA);
    if (!mesh_begin()) {
        ui_toast("mesh init failed", T_BAD, 1500);
        radio_switch(RADIO_NONE);
        return;
    }

    char id_buf[12];
    if (!input_line("Dest node !xxxxxxxx:", id_buf, sizeof(id_buf))) { mesh_end(); return; }
    const char *s = id_buf;
    if (*s == '!') s++;
    uint32_t dest = (uint32_t)strtoul(s, nullptr, 16);
    if (dest == 0) {
        ui_toast("bad id", T_BAD, 1000);
        mesh_end();
        return;
    }
    page_to(dest);
    mesh_end();
}
