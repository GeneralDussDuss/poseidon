/*
 * SaltyJack — info / homage landing page.
 *
 * All credit for the ideas + most of the C code in this submenu goes to
 * @7h30th3r0n3 (Evil-M5Project, RaspyJack). This page exists to make that
 * loud and visible every time the menu is opened.
 */
#include "../../app.h"
#include "../../theme.h"
#include "../../ui.h"
#include "../../input.h"
#include "saltyjack.h"

void feat_saltyjack_info(void)
{
    auto &d = M5Cardputer.Display;
    ui_clear_body();

    /* Title band */
    d.setTextColor(T_ACCENT2, T_BG);
    d.setCursor(4, BODY_Y + 2);
    d.print("SALTYJACK");
    d.setTextColor(T_DIM, T_BG);
    d.setCursor(80, BODY_Y + 3);
    d.print("LAN attack suite");
    d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT2);

    /* Homage block — the point of this page. */
    d.setTextColor(T_FG, T_BG);
    d.setCursor(4, BODY_Y + 18);
    d.print("by @7h30th3r0n3");
    d.setTextColor(T_DIM, T_BG);
    d.setCursor(4, BODY_Y + 28);
    d.print("ported from Evil-M5Project");
    d.setCursor(4, BODY_Y + 38);
    d.print("+ RaspyJack (RPi edition)");

    /* What it does, in one line per attack */
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 54);
    d.print("arsenal:");
    d.setTextColor(T_FG, T_BG);
    d.setCursor(6, BODY_Y + 64); d.print("- DHCP starve");
    d.setCursor(6, BODY_Y + 72); d.print("- rogue DHCP (sta / ap)");
    d.setCursor(6, BODY_Y + 80); d.print("- responder LLMNR/NBNS/SMB");
    d.setCursor(6, BODY_Y + 88); d.print("- WPAD NTLM harvest");
    d.setCursor(6, BODY_Y + 96); d.print("- on-device NTLMv2 crack");

    d.setTextColor(T_WARN, T_BG);
    d.setCursor(4, BODY_Y + 110);
    d.print("authorized testing only");

    ui_draw_footer("`=back");

    while (true) {
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(30); continue; }
        if (k == PK_ESC) break;
    }
}
