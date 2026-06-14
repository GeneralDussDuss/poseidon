/*
 /* mesh_chat — live Meshtastic text chat.
  *
  * Top half: scrolling log of received text messages (from, time, text).
  * Bottom half: input line. ENTER broadcasts. Backtick exits.
  *
  * Shows ACK delivery status for sent messages:
  *   ✓ = ACK received, ✗ = delivery failed, · = pending
  */
 #include "../app.h"
 #include "../theme.h"
 #include "../ui.h"
 #include "../input.h"
 #include "../radio.h"
 #include "../sfx.h"
 #include "../menu.h"
 #include "../mesh/meshtastic.h"
 #include <stdio.h>
 #include <string.h>

 static uint32_t          s_last_send_id    = 0;
 static mesh_ack_status_t s_last_ack_status = MESH_ACK_FAILED;

 static void draw_chat(const char *input, int input_len, bool typing)
 {
     auto &d = M5Cardputer.Display;
     ui_clear_body();

     d.setTextColor(T_ACCENT, T_BG);
     d.setCursor(4, BODY_Y + 2);
     d.printf("MESH  %s  !%08x",
              mesh_own_short_name(), (unsigned int)mesh_own_node_id());

     /* Show ACK delivery status if we have a tracked send. */
     if (s_last_send_id != 0) {
         d.setCursor(SCR_W - 18, BODY_Y + 2);
         if (s_last_ack_status == MESH_ACK_OK) {
             d.setTextColor(T_GOOD, T_BG);
             d.print("\xFB");   /* checkmark glyph in M5 font */
         } else if (s_last_ack_status == MESH_ACK_FAILED) {
             d.setTextColor(T_BAD, T_BG);
             d.print("x");
         } else {
             d.setTextColor(T_DIM, T_BG);
             d.print("\xFA");   /* dot glyph */
         }
     }

     d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT);

    /* Snapshot the last 6 messages oldest-first under the ring's mutex —
     * the raw mesh_messages() pointer is not chronological after the
     * ring wraps and isn't mutex-safe against RX task writes. */
    mesh_message_t snap[6];
    int count = mesh_snapshot_messages(snap, 6);

    int y = BODY_Y + 16;
    for (int i = 0; i < count; i++) {
        const mesh_message_t &m = snap[i];
        uint32_t age = (millis() - m.when_ms) / 1000;
        d.setTextColor((m.to == mesh_own_node_id()) ? T_GOOD : T_ACCENT2, T_BG);
        d.setCursor(4, y);
        d.printf("!%08x %3lus", (unsigned int)m.from, (unsigned long)age);
        d.setTextColor(T_FG, T_BG);
        d.setCursor(4, y + 8);
        char line[40];
        size_t n = m.text_len;
        if (n > sizeof(line) - 1) n = sizeof(line) - 1;
        memcpy(line, m.text, n);
        line[n] = '\0';
        d.print(line);
        y += 18;
        if (y > BODY_Y + BODY_H - 30) break;
    }
    if (count == 0) {
        d.setTextColor(T_DIM, T_BG);
        d.setCursor(4, BODY_Y + 30);
        d.print("no messages yet — listening");
    }

    /* Input row */
    int input_y = BODY_Y + BODY_H - 14;
    d.drawFastHLine(4, input_y - 2, SCR_W - 8, T_DIM);
    d.setTextColor(typing ? T_ACCENT : T_DIM, T_BG);
    d.setCursor(4, input_y);
    d.print(typing ? "> " : "T=type ");
    d.setTextColor(T_FG, T_BG);
    d.print(input);
    if (typing) d.print('_');
}

void feat_mesh_chat(void)
{
    radio_switch(RADIO_LORA);
    if (!mesh_begin()) {
        ui_toast("mesh init failed", T_BAD, 1500);
        radio_switch(RADIO_NONE);
        return;
    }

    char input[128] = {0};
    int input_len = 0;
    bool typing = false;
    bool dirty = true;

    /* Reset ACK tracking for this session. */
    s_last_send_id = 0;
    s_last_ack_status = MESH_ACK_FAILED;

    ui_draw_footer("T=type  R=reset  `=back");

    while (true) {
        if (mesh_drain_new_message()) { dirty = true; sfx_scan_hit(); }

        /* Poll ACK delivery status. */
        if (s_last_send_id != 0) {
            mesh_ack_status_t st = mesh_ack_status(s_last_send_id);
            if (st != s_last_ack_status) {
                s_last_ack_status = st;
                dirty = true;
                if (st == MESH_ACK_OK) {
                    sfx_scan_hit();
                    ui_toast("delivered", T_GOOD, 800);
                } else if (st == MESH_ACK_FAILED) {
                    sfx_error();
                    ui_toast("no ACK", T_BAD, 1000);
                }
            }
        }

        if (dirty) {
            draw_chat(input, input_len, typing);
            dirty = false;
        }
        mesh_tick();

        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }

        if (!typing) {
            if (k == PK_ESC) break;
            if (k == '?') { ui_show_current_help(); dirty = true; }
            if (k == 't' || k == 'T') { typing = true; dirty = true; }
            if (k == 'r' || k == 'R') { mesh_clear_messages(); dirty = true; }
        } else {
            if (k == PK_ESC) {
                typing = false; input_len = 0; input[0] = 0; dirty = true;
            } else if (k == PK_ENTER) {
                if (input_len > 0) {
                    uint32_t pid = mesh_send_broadcast_text(input);
                    if (pid) {
                        s_last_send_id = pid;
                        s_last_ack_status = MESH_ACK_PENDING;
                        ui_toast("sent..", T_GOOD, 500);
                    } else {
                        ui_toast("TX failed", T_BAD, 800);
                    }
                    input_len = 0; input[0] = 0;
                }
                typing = false;
                dirty = true;
            } else if (k == PK_BKSP) {
                if (input_len > 0) { input[--input_len] = 0; dirty = true; }
            } else if (k >= ' ' && k < 0x80 && input_len < (int)sizeof(input) - 1) {
                input[input_len++] = (char)k;
                input[input_len] = 0;
                dirty = true;
            }
        }
    }

    /* Stop the RX task before returning — any other LoRa feature the user
     * opens next will call lora_begin() which calls lora_end() which
     * deletes the SX1262. If our RX task is still running it'll
     * dereference that freed object and InstrFetchProhibited on the stale
     * vtable. Losing the roster between feature entries is the right
     * trade vs a hard panic. */
    mesh_end();
}
