/*
 * mesh_position — toggle background broadcast of POSEIDON's own position
 * as a Meshtastic Position packet. When enabled, POSEIDON shows up as
 * a pin in other Meshtastic apps if GPS has a fix.
 */
#include "../app.h"
#include "../theme.h"
#include "../ui.h"
#include "../input.h"
#include "../radio.h"
#include "../gps.h"
#include "../mesh/meshtastic.h"
#include <stdio.h>

void feat_mesh_position(void)
{
    radio_switch(RADIO_LORA);
    if (!mesh_begin()) {
        ui_toast("mesh init failed", T_BAD, 1500);
        radio_switch(RADIO_NONE);
        return;
    }

    ui_draw_footer("T=toggle  B=send now  `=back");
    auto &d = M5Cardputer.Display;

    while (true) {
        ui_clear_body();
        d.setTextColor(T_ACCENT, T_BG);
        d.setCursor(4, BODY_Y + 2); d.print("MESH POSITION");
        d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT);

        bool on = mesh_position_reporting();
        d.setTextColor(on ? T_GOOD : T_DIM, T_BG);
        d.setCursor(4, BODY_Y + 20);
        d.printf("reporting : %s", on ? "ON" : "off");

        const gps_fix_t &fix = gps_get();
        d.setTextColor(fix.valid ? T_GOOD : T_WARN, T_BG);
        d.setCursor(4, BODY_Y + 34);
        d.printf("GPS fix   : %s", fix.valid ? "yes" : "searching");
        if (fix.valid) {
            d.setTextColor(T_FG, T_BG);
            d.setCursor(4, BODY_Y + 46); d.printf("lat %+8.4f", fix.lat_deg);
            d.setCursor(4, BODY_Y + 56); d.printf("lon %+8.4f", fix.lon_deg);
            d.setCursor(4, BODY_Y + 66); d.printf("alt %dm  sats %d", (int)fix.alt_m, (int)fix.sats);
        } else {
            d.setTextColor(T_DIM, T_BG);
            d.setCursor(4, BODY_Y + 46); d.print("no coordinates yet");
        }

        d.setTextColor(T_DIM, T_BG);
        d.setCursor(4, BODY_Y + BODY_H - 14);
        d.printf("node !%08x", (unsigned int)mesh_own_node_id());

        mesh_tick();

        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(100); continue; }
        if (k == PK_ESC) break;
        if (k == 't' || k == 'T') {
            mesh_set_position_reporting(!on);
            ui_toast(on ? "reporting off" : "reporting on",
                     on ? T_DIM : T_GOOD, 800);
        }
        if (k == 'b' || k == 'B') {
            if (mesh_send_position()) {
                ui_toast("sent", T_GOOD, 600);
            } else {
                ui_toast("no GPS fix", T_BAD, 800);
            }
        }
    }
    /* Tear down before the user opens another LoRa feature. The position
     * reporting toggle state is preserved in a static flag in meshtastic_node
     * so re-entering any mesh feature resumes where we left off. */
    mesh_end();
}
