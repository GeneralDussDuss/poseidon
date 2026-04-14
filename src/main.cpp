/*
 * POSEIDON main — boot + splash + menu.
 */
#include "app.h"
#include "ui.h"
#include "input.h"
#include "menu.h"
#include "radio.h"

void setup()
{
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    M5Cardputer.Display.setRotation(1);  /* landscape, keyboard at the bottom */
    ui_init();

    ui_splash();  /* animates, then waits for a key press internally */
}

void loop()
{
    menu_run();
    /* menu_run only returns on a quit — rare. Fall through to a
     * quiescent poll loop so the device doesn't deadlock. */
    ui_clear_body();
    ui_toast("menu exited", COL_WARN, 800);
    delay(200);
}
