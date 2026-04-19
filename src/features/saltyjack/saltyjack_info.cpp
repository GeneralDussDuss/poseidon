/*
 * SaltyJack — info / homage landing page.
 *
 * RaspyJack-style phosphor terminal UI with a seafoam/cyan ocean twist.
 * All credit for the ideas + most of the C code in this submenu goes
 * to @7h30th3r0n3 (Evil-M5Project, RaspyJack).
 */
#include "../../app.h"
#include "../../ui.h"
#include "../../input.h"
#include "saltyjack.h"
#include "saltyjack_style.h"

void feat_saltyjack_info(void)
{
    auto &d = M5Cardputer.Display;
    sj_frame("SaltyJack");

    d.setTextColor(SJ_FG, SJ_BG);
    d.setCursor(SJ_CONTENT_X, BODY_Y + 18);
    d.print("by @7h30th3r0n3");

    d.setTextColor(SJ_FG_DIM, SJ_BG);
    d.setCursor(SJ_CONTENT_X, BODY_Y + 27);
    d.print("porting Evil-M5Project");
    d.setCursor(SJ_CONTENT_X, BODY_Y + 36);
    d.print("and RaspyJack attacks");

    sj_divider(BODY_Y + 47);

    d.setTextColor(SJ_ACCENT, SJ_BG);
    d.setCursor(SJ_CONTENT_X, BODY_Y + 52);
    d.print("arsenal");
    d.setTextColor(SJ_FG, SJ_BG);
    d.setCursor(SJ_CONTENT_X + 2, BODY_Y + 62); d.print("- DHCP starve");
    d.setCursor(SJ_CONTENT_X + 2, BODY_Y + 70); d.print("- rogue DHCP (sta/ap)");
    d.setCursor(SJ_CONTENT_X + 2, BODY_Y + 78); d.print("- responder LLMNR/NBNS");
    d.setCursor(SJ_CONTENT_X + 2, BODY_Y + 86); d.print("- WPAD NTLM harvest");
    d.setCursor(SJ_CONTENT_X + 2, BODY_Y + 94); d.print("- on-device NTLMv2 crack");

    d.setTextColor(SJ_WARN, SJ_BG);
    d.setCursor(SJ_CONTENT_X, BODY_Y + 106);
    d.print("authorized testing only");

    sj_footer("`=back");

    while (true) {
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(30); continue; }
        if (k == PK_ESC) break;
    }
}
