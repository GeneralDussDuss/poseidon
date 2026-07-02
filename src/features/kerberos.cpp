/*
 * kerberos — FIDO U2F security key feature (Phase 1).
 *
 * Enter this mode, plug into a PC, and the Cardputer enumerates as a USB
 * security key. Register it at a site's security-key settings; the screen
 * shows a sign-in prompt and ENTER approves (user presence). Phase 1 speaks
 * U2F/CTAP1, which browsers accept as a second-factor security key.
 *
 * Storage is plain NVS for Phase 1: a random 32-byte device key wraps stateless
 * credential handles, and one global signature counter. Phase 4 replaces the
 * NVS key with an eFuse+PIN derived key.
 */
#include "app.h"
#include "../theme.h"
#include "ui.h"
#include "input.h"
#include "mimir.h"
#include <Preferences.h>

#include "u2f.h"
#include "kerberos_hid.h"
#include "kerberos_crypto.h"
#include "kerberos_attestation.h"

static uint32_t s_counter = 0;
static uint32_t s_persisted = 0;

// User presence: draw the approval prompt, block on the keyboard, and keep the
// host alive with CTAPHID KEEPALIVE frames so a slow human approval never times
// out. Runs from kerberos_hid_poll() on the main loop, never the USB callback.
static bool kerberos_user_present(void *) {
    auto &d = M5Cardputer.Display;
    ui_clear_body();
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2); d.print("APPROVE SIGN-IN");
    d.drawFastHLine(4, BODY_Y + 12, 150, T_ACCENT);
    d.setTextColor(T_FG, T_BG);
    d.setCursor(4, BODY_Y + 24); d.print("A site is asking KERBEROS");
    d.setCursor(4, BODY_Y + 36); d.print("to sign in.");
    ui_draw_footer("ENTER=approve  `=deny");
    while (true) {
        uint16_t k = input_poll();
        if (k == PK_ENTER) { ui_toast("approved", T_GOOD, 500); return true; }
        if (k == PK_ESC)   { ui_toast("denied", T_WARN, 500); return false; }
        kerberos_hid_keepalive(2);   // 2 = user presence needed
        delay(90);
    }
}

static uint16_t u2f_msg_thunk(const uint8_t *req, uint16_t rl,
                              uint8_t *resp, uint16_t cap, void *ctx) {
    return u2f_handle((const u2f_cfg_t *)ctx, req, rl, resp, cap);
}

static void persist_counter(void) {
    if (s_counter == s_persisted) return;
    Preferences p; p.begin("kerberos", false);
    p.putUInt("ctr", s_counter);
    p.end();
    s_persisted = s_counter;
}

void feat_kerberos(void) {
    extern bool g_trident_cdc_active;
    if (g_mimir_cdc_active || g_trident_cdc_active) {
        ui_toast("CDC in use", T_WARN, 1000); return;
    }

    // Load or create the device wrapping key and signature counter.
    static uint8_t devkey[32];
    Preferences prefs; prefs.begin("kerberos", false);
    if (prefs.getBytesLength("devkey") == 32) {
        prefs.getBytes("devkey", devkey, 32);
    } else {
        kerb_mbedtls_crypto()->rand(devkey, 32, nullptr);
        prefs.putBytes("devkey", devkey, 32);
    }
    s_counter = prefs.getUInt("ctr", 0);
    s_persisted = s_counter;
    prefs.end();

    static u2f_cfg_t cfg;
    cfg.cy           = kerb_mbedtls_crypto();
    cfg.devkey       = devkey;
    cfg.att_cert     = KERB_ATT_CERT;
    cfg.att_cert_len = KERB_ATT_CERT_LEN;
    cfg.att_priv     = KERB_ATT_PRIV;
    cfg.counter      = &s_counter;
    cfg.user_present = kerberos_user_present;
    cfg.ui           = nullptr;

    kerberos_hid_set_msg_handler(u2f_msg_thunk, &cfg);
    kerberos_hid_begin();

    // Status screen. Redraw once, then poll the transport and the keyboard.
    auto &d = M5Cardputer.Display;
    ui_clear_body();
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2); d.print("KERBEROS");
    d.drawFastHLine(4, BODY_Y + 12, 150, T_ACCENT);
    d.setTextColor(T_FG, T_BG);
    d.setCursor(4, BODY_Y + 24); d.print("FIDO security key active.");
    d.setCursor(4, BODY_Y + 36); d.print("Plug into a PC and add it");
    d.setCursor(4, BODY_Y + 48); d.print("as a security key.");
    ui_draw_footer("`=exit");
    ui_draw_status("fido", "up");

    while (true) {
        kerberos_hid_poll();
        persist_counter();
        uint16_t k = input_poll();
        if (k == PK_ESC) break;
        delay(2);
    }
    persist_counter();
    ui_toast("KERBEROS off", T_DIM, 700);
}
