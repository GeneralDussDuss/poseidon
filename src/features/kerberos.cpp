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
#include "ctap2.h"
#include "kerberos_hid.h"
#include "kerberos_crypto.h"
#include "kerberos_attestation.h"
#include "kerberos_bootmode.h"

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

// Stable AAGUID for KERBEROS (ASCII "KERBEROSPOSEIDON").
static const uint8_t KERB_AAGUID[16] = {
    0x4B,0x45,0x52,0x42,0x45,0x52,0x4F,0x53, 0x50,0x4F,0x53,0x45,0x49,0x44,0x4F,0x4E
};
static ctap2_cfg_t s_c2;
static uint16_t ctap2_msg_thunk(const uint8_t *req, uint16_t rl,
                                uint8_t *resp, uint16_t cap, void *ctx) {
    return ctap2_handle((const ctap2_cfg_t *)ctx, req, rl, resp, cap);
}

static void persist_counter(void) {
    if (s_counter == s_persisted) return;
    Preferences p; p.begin("kerberos", false);
    p.putUInt("ctr", s_counter);
    p.end();
    s_persisted = s_counter;
}

// Prompt shown in normal mode: entering KERBEROS needs a reboot into key mode
// so FIDO can be the sole USB HID device. Returns true if the user confirmed.
static bool confirm_enter_key_mode(void) {
    auto &d = M5Cardputer.Display;
    ui_clear_body();
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2); d.print("KERBEROS KEY MODE");
    d.drawFastHLine(4, BODY_Y + 12, 150, T_ACCENT);
    d.setTextColor(T_FG, T_BG);
    d.setCursor(4, BODY_Y + 24); d.print("Reboots into FIDO key mode");
    d.setCursor(4, BODY_Y + 36); d.print("(BadUSB off until you exit).");
    ui_draw_footer("ENTER=reboot in  `=cancel");
    while (true) {
        uint16_t k = input_poll();
        if (k == PK_ENTER) return true;
        if (k == PK_ESC)   return false;
        delay(10);
    }
}

void feat_kerberos(void) {
    // Normal mode: FIDO is not on the USB bus. Offer to reboot into key mode,
    // where FIDO is the sole HID device. loop() calls us again after the reboot.
    if (!kerb_boot_key_mode()) {
        if (confirm_enter_key_mode()) kerb_request_key_mode(true);  // reboots
        return;
    }

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

    // CTAP2 handler shares the crypto, device key, attestation, and presence.
    s_c2.cy = cfg.cy; s_c2.devkey = devkey;
    s_c2.aaguid = KERB_AAGUID;
    s_c2.att_cert = KERB_ATT_CERT; s_c2.att_cert_len = KERB_ATT_CERT_LEN;
    s_c2.att_priv = KERB_ATT_PRIV;
    s_c2.user_present = kerberos_user_present; s_c2.ui = nullptr;
    s_c2.counter = &s_counter;            // shared with U2F; persisted by persist_counter()
    s_c2.store = nullptr;                 // resident credentials wired in a later task
    kerberos_hid_set_cbor_handler(ctap2_msg_thunk, &s_c2);

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
    ui_draw_footer("`=exit to normal (reboots)");
    ui_draw_status("fido", "up");

    while (true) {
        kerberos_hid_poll();
        persist_counter();
        uint16_t k = input_poll();
        if (k == PK_ESC) {
            persist_counter();
            kerb_request_key_mode(false);   // reboot back to normal (BadUSB) mode
        }
        delay(2);
    }
}
