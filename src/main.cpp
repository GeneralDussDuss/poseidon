/*
 * POSEIDON main — boot + splash + menu.
 */
#include "app.h"
#include "ui.h"
#include "input.h"
#include "menu.h"
#include "radio.h"
#include "sd_helper.h"
#include "version.h"

void setup()
{
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    M5Cardputer.Display.setRotation(1);  /* landscape, keyboard at the bottom */
    Serial.begin(115200);
    delay(100);
    Serial.printf("\n[POSEIDON] %s (%s) boot\n",
                  poseidon_version(), poseidon_build_date());

    /* Mount SD on boot if a card is present. Non-fatal if absent. */
    if (sd_mount()) Serial.println("[POSEIDON] sd mounted");
    else            Serial.println("[POSEIDON] sd absent");

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
