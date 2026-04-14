/*
 * mesh_status — live view of the PigSync/KRAKEN ESP-NOW mesh.
 */
#include "app.h"
#include "ui.h"
#include "input.h"
#include "mesh.h"
#include "radio.h"

void feat_mesh(void)
{
    /* ESP-NOW uses the WiFi PHY but doesn't need STA association.
     * Bring WiFi up in STA mode (idle), start mesh. */
    radio_switch(RADIO_WIFI);
    mesh_begin("POSEIDON");

    ui_clear_body();
    ui_draw_footer("ESC=stop  any=ignored");

    uint32_t last = 0;
    while (true) {
        uint32_t now = millis();
        if (now - last > 500) {
            last = now;
            auto &d = M5Cardputer.Display;
            ui_draw_status(radio_name(), "mesh");
            ui_clear_body();
            d.setTextColor(COL_ACCENT, COL_BG);
            d.setCursor(4, BODY_Y + 2);
            d.printf("MESH  tx:%lu rx:%lu",
                     (unsigned long)mesh_tx_count(),
                     (unsigned long)mesh_rx_count());

            mesh_peer_t peers[MESH_MAX_PEERS];
            int n = mesh_peers(peers, MESH_MAX_PEERS);
            if (n == 0) {
                d.setTextColor(COL_DIM, COL_BG);
                d.setCursor(4, BODY_Y + 22);
                d.print("no peers yet. broadcasting HELLO every 5s.");
            } else {
                for (int i = 0; i < n && i < 7; ++i) {
                    int y = BODY_Y + 18 + i * 12;
                    d.setTextColor(COL_FG, COL_BG);
                    d.setCursor(4, y);
                    d.printf("%-10s %ddB %luKB %s",
                             peers[i].name, peers[i].rssi,
                             (unsigned long)peers[i].heap_kb,
                             peers[i].has_gps ? "GPS" : "");
                }
            }
        }

        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) break;
    }
    mesh_stop();
}
