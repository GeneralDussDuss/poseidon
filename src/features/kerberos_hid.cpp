/*
 * kerberos_hid — FIDO USB HID transport.
 *
 * Presents ONE HID interface whose report descriptor carries the FIDO usage
 * page (0xF1D0) with 64-byte input/output reports and no report id, which is
 * what an OS FIDO stack (Windows WebAuthn, Chromium) looks for. Inbound OUT
 * reports are copied into a small ring in the USB callback and processed later
 * from kerberos_hid_poll() on the main loop, because the CTAPHID dispatcher can
 * call the user-presence prompt which blocks on the keyboard.
 *
 * On-device caveat (verify with a browser): the Arduino USBHID class merges all
 * added HID devices into one interface. FIDO must be the ONLY added HID device
 * for the OS to recognise it as a security key, so this interface is created
 * lazily on entering KERBEROS and badusb's keyboard must not have been added in
 * the same boot. If enumeration fails, the fallback is a dedicated raw TinyUSB
 * interface.
 */
#include "kerberos_hid.h"
#include <USB.h>
#include <USBHID.h>
#include <string.h>

#if !CONFIG_TINYUSB_HID_ENABLED
#warning "TinyUSB HID not enabled; KERBEROS FIDO interface will not build"
#endif

static const uint8_t FIDO_REPORT_DESC[] = {
    0x06, 0xD0, 0xF1,       // Usage Page (FIDO Alliance 0xF1D0)
    0x09, 0x01,             // Usage (CTAPHID)
    0xA1, 0x01,             // Collection (Application)
    0x09, 0x20,             //   Usage (Input Report Data)
    0x15, 0x00,             //   Logical Min 0
    0x26, 0xFF, 0x00,       //   Logical Max 255
    0x75, 0x08,             //   Report Size 8
    0x95, 0x40,             //   Report Count 64
    0x81, 0x02,             //   Input (Data,Var,Abs)
    0x09, 0x21,             //   Usage (Output Report Data)
    0x15, 0x00, 0x26, 0xFF, 0x00,
    0x75, 0x08, 0x95, 0x40,
    0x91, 0x02,             //   Output (Data,Var,Abs)
    0xC0                    // End Collection
};

class USBHIDFido : public USBHIDDevice {
public:
    USBHID hid;
    bool   added = false;
    void add() {
        if (!added) { hid.addDevice(this, sizeof(FIDO_REPORT_DESC)); added = true; }
    }
    uint16_t _onGetDescriptor(uint8_t *buffer) override {
        memcpy(buffer, FIDO_REPORT_DESC, sizeof(FIDO_REPORT_DESC));
        return sizeof(FIDO_REPORT_DESC);
    }
    void _onOutput(uint8_t report_id, const uint8_t *buffer, uint16_t len) override;
    bool sendPacket(const uint8_t pkt[64]) { return hid.SendReport(0, pkt, 64); }
};

static USBHIDFido   s_fido;
static ctaphid_ctx_t s_ctx;
static uint32_t     s_cur_cid = 0xFFFFFFFF;

// Inbound ring of 64-byte reports (filled in USB callback, drained in poll).
#define KH_QN 8
static uint8_t  s_q[KH_QN][64];
static volatile uint8_t s_qhead = 0, s_qtail = 0;

static void kh_sink(const uint8_t pkt[64], void *) {
    s_fido.sendPacket(pkt);
}

void USBHIDFido::_onOutput(uint8_t, const uint8_t *buffer, uint16_t len) {
    if (len < 64) return;
    uint8_t next = (uint8_t)((s_qhead + 1) % KH_QN);
    if (next == s_qtail) return;             // ring full, drop (host will retry)
    memcpy(s_q[s_qhead], buffer, 64);
    s_qhead = next;
}

void kerberos_hid_set_msg_handler(ctaphid_msg_fn fn, void *ctx) {
    ctaphid_ctx_init(&s_ctx, kh_sink, nullptr, fn, ctx);
}

void kerberos_hid_begin(void) {
    s_fido.add();
    USB.begin();
}

void kerberos_hid_poll(void) {
    while (s_qtail != s_qhead) {
        uint8_t *pkt = s_q[s_qtail];
        s_cur_cid = ctaphid_dispatch(&s_ctx, pkt);
        s_qtail = (uint8_t)((s_qtail + 1) % KH_QN);
    }
}

bool kerberos_hid_ready(void) { return s_fido.hid.ready(); }

void kerberos_hid_keepalive(uint8_t status) {
    if (s_cur_cid != 0xFFFFFFFF) ctaphid_keepalive(&s_ctx, s_cur_cid, status);
}
