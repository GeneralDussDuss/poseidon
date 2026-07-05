/*
 * mesh_traceroute — trace the hop-by-hop path to a Meshtastic node.
 *
 * Shows the node roster; user picks a target; a traceroute request is
 * sent; the response (populated by intermediate Meshtastic firmware
 * nodes) is displayed as:
 *
 *   You → !1234abcd (+6dB) → !5678efab (+3dB) → !deadbeef (dest)
 *
 * Timeout after 10 seconds if no response.
 */
#include "../app.h"
#include "../theme.h"
#include "../ui.h"
#include "../input.h"
#include "../radio.h"
#include "../mesh/meshtastic.h"
#include <stdio.h>
#include <string.h>

static const char *node_short_name(uint32_t id)
{
    int count;
    const mesh_node_t *nodes = mesh_nodes(&count);
    for (int i = 0; i < count; i++) {
        if (nodes[i].id == id && nodes[i].short_name[0])
            return nodes[i].short_name;
    }
    return nullptr;
}

void feat_mesh_traceroute(void)
{
    radio_switch(RADIO_LORA);
    if (!mesh_begin()) {
        ui_toast("mesh init failed", T_BAD, 1500);
        radio_switch(RADIO_NONE);
        return;
    }

    int cursor = 0;
    ui_draw_footer(";/.=move  ENTER=trace  `=back");

    /* ---- Phase 1: pick target node ---- */
    while (true) {
        int count;
        const mesh_node_t *nodes = mesh_nodes(&count);

        auto &d = M5Cardputer.Display;
        ui_clear_body();
        d.setTextColor(T_ACCENT, T_BG);
        d.setCursor(4, BODY_Y + 2);
        d.printf("TRACEROUTE  pick node %d", count);
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
                d.setCursor(34, y);  d.printf("!%08x", (unsigned int)n.id);
                d.setCursor(120, y); d.printf("%+d", (int)n.last_snr);
                d.setCursor(150, y); d.printf("h%d", (int)n.hops);
            }
        }

        mesh_tick();

        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(30); continue; }
        if (k == PK_ESC) { mesh_end(); return; }
        if (k == ';' || k == PK_UP)   { if (cursor > 0) cursor--; }
        if (k == '.' || k == PK_DOWN) { if (cursor + 1 < count) cursor++; }
        if (k == PK_ENTER && count > 0) {
            /* Got our target — send traceroute. */
            break;
        }
    }

    uint32_t target = mesh_nodes(nullptr)[cursor].id;

    /* ---- Phase 2: send and wait for response ---- */
    mesh_traceroute_clear();

    auto &d = M5Cardputer.Display;
    if (!mesh_send_traceroute(target)) {
        ui_toast("TX failed", T_BAD, 1000);
        mesh_end();
        return;
    }

    ui_draw_footer("ESC=cancel");
    uint32_t start_ms = millis();
    const uint32_t TIMEOUT_MS = 10000;
    int dots = 0;

    while (true) {
        /* Check for result. */
        mesh_traceroute_result_t result;
        if (mesh_traceroute_result(&result)) {
            /* ---- Phase 3: display result ---- */
            ui_clear_body();
            d.setTextColor(T_ACCENT, T_BG);
            d.setCursor(4, BODY_Y + 2);
            d.printf("ROUTE to !%08x", (unsigned int)target);
            d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT);

            if (result.hops == 0) {
                /* Direct link — no intermediate hops. */
                d.setTextColor(T_GOOD, T_BG);
                d.setCursor(4, BODY_Y + 22);
                d.print("DIRECT LINK");
                d.setCursor(4, BODY_Y + 34);
                d.print("You --> Target");
                d.setTextColor(T_DIM, T_BG);
                d.setCursor(4, BODY_Y + 48);
                d.print("(no intermediate hops)");
            } else {
                /* Multi-hop path. Show each hop. */
                int y = BODY_Y + 18;
                int scroll = 0;
                bool redraw = true;

                while (true) {
                    if (redraw) {
                        /* Re-draw from scroll offset. */
                        ui_clear_body();
                        d.setTextColor(T_ACCENT, T_BG);
                        d.setCursor(4, BODY_Y + 2);
                        d.printf("ROUTE !%08x  %d hop%s",
                                 (unsigned int)target, result.hops,
                                 result.hops == 1 ? "" : "s");
                        d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT);
                        y = BODY_Y + 18;

                        /* "You" at top if scrolled to 0. */
                        if (scroll == 0) {
                            d.setTextColor(T_FG, T_BG);
                            d.setCursor(4, y);
                            d.print("You");
                            y += 11;
                        }

                        for (int i = (scroll == 0 ? 0 : scroll - 1);
                             i < result.hops && y < BODY_Y + BODY_H - 10; i++) {
                            d.setTextColor(T_FG, T_BG);
                            d.setCursor(4, y);
                            d.print("-> ");

                            const char *sn = node_short_name(result.route[i]);
                            if (sn) {
                                d.printf("%s", sn);
                            } else {
                                d.printf("!%08x", (unsigned int)result.route[i]);
                            }
                            d.setTextColor(T_DIM, T_BG);
                            d.printf(" %+ddB", (int)result.snr[i]);
                            y += 11;
                        }

                        /* Destination at bottom. */
                        if (y < BODY_Y + BODY_H - 10) {
                            d.setTextColor(T_GOOD, T_BG);
                            d.setCursor(4, y);
                            d.print("-> ");
                            const char *dsn = node_short_name(target);
                            if (dsn) d.printf("%s (dest)", dsn);
                            else     d.printf("!%08x (dest)", (unsigned int)target);
                        }
                        redraw = false;
                    }

                    uint16_t k2 = input_poll();
                    if (k2 == PK_NONE) { delay(30); continue; }
                    if (k2 == PK_ESC || k2 == PK_ENTER) break;
                    if ((k2 == '.' || k2 == PK_DOWN) && scroll < result.hops) {
                        scroll++; redraw = true;
                    }
                    if ((k2 == ';' || k2 == PK_UP) && scroll > 0) {
                        scroll--; redraw = true;
                    }
                }
            }

            /* After viewing result, exit. */
            mesh_end();
            return;
        }

        /* Still waiting — show progress animation. */
        ui_clear_body();
        d.setTextColor(T_ACCENT, T_BG);
        d.setCursor(4, BODY_Y + 2);
        d.printf("TRACING !%08x", (unsigned int)target);
        d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT);

        d.setTextColor(T_FG, T_BG);
        d.setCursor(4, BODY_Y + 22);
        d.print("waiting for response");

        /* Animated dots. */
        d.setCursor(4, BODY_Y + 34);
        for (int i = 0; i < dots; i++) d.print('.');
        dots = (dots + 1) % 20;

        /* Progress bar. */
        uint32_t elapsed = millis() - start_ms;
        int bar_w = (int)((uint32_t)(SCR_W - 8) * elapsed / TIMEOUT_MS);
        if (bar_w > SCR_W - 8) bar_w = SCR_W - 8;
        d.fillRect(4, BODY_Y + 50, bar_w, 4, T_ACCENT);

        mesh_tick();

        if (elapsed >= TIMEOUT_MS) {
            /* Timeout — no response. */
            ui_clear_body();
            d.setTextColor(T_BAD, T_BG);
            d.setCursor(4, BODY_Y + 22);
            d.print("NO RESPONSE");
            d.setTextColor(T_DIM, T_BG);
            d.setCursor(4, BODY_Y + 36);
            d.printf("to !%08x", (unsigned int)target);
            d.setCursor(4, BODY_Y + 48);
            d.print("target may be out of range");
            d.setCursor(4, BODY_Y + 60);
            d.print("or mesh is too small");
            ui_draw_footer("any key to exit");
            while (input_poll() == PK_NONE) delay(20);
            mesh_end();
            return;
        }

        /* Allow cancel. */
        uint16_t k = input_poll();
        if (k == PK_ESC) {
            mesh_end();
            return;
        }

        delay(200);
    }
}
