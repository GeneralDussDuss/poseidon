/*
 * ble_dult - non-owner Find My / DULT tracker interaction.
 * See ble_dult.h for the protocol table and the research reference.
 */
#include "app.h"
#include "../theme.h"
#include "ui.h"
#include "input.h"
#include "radio.h"
#include "ble_dult.h"
#include "../sd_helper.h"
#include <NimBLEDevice.h>
#include <SD.h>
#include <stdarg.h>

/* ---------------- protocol constants (verified, do not substitute) ------- */

#define UUID_AIRTAG_SVC "7dfc9000-7d1c-4951-86aa-8d9728f8d66c"
#define UUID_AIRTAG_CHR "7dfc9001-7d1c-4951-86aa-8d9728f8d66c"
#define UUID_DULT_SVC   "15190001-12f4-c226-88ed-2ac5579f2a85"
#define UUID_DULT_CHR   "8e0c0001-1d68-fb92-bf61-48377421680e"
#define UUID_FMN_CHR    "4f860003-943b-49ef-bed4-2f730304427a"
/* Apple Find My Network GATT service is the 16-bit 0xFD44. */
#define UUID16_FMN_SVC  ((uint16_t)0xFD44)

/* Apple advertisement: manufacturer data as NimBLE returns it starts at
 * the company ID, so the byte AirGuard calls mfg[1] is md[3] here:
 *   md[0..1] = 4C 00 company id
 *   md[2]    = 12       Find My / offline finding type
 *   md[3]    = 19       full payload length -> SEPARATED
 *   md[4]    = status   bits 7:6 = battery level */
#define APPLE_CID        0x004Cu
#define APPLE_TYPE_FINDMY 0x12
#define APPLE_SEPARATED   0x19

static const uint8_t OP_SOUND_START[2] = { 0x00, 0x03 };  /* 0x0300 LE */
static const uint8_t OP_SOUND_STOP[2]  = { 0x01, 0x03 };  /* 0x0301 LE */
static const uint8_t OP_FMN_START[3]   = { 0x01, 0x00, 0x03 };
static const uint8_t OP_FMN_STOP[3]    = { 0x01, 0x01, 0x03 };
static const uint8_t OP_AIRTAG_BEEP    = 0xAF;

#define DULT_RSP_COMMAND    0x0302u   /* Command_Response */
#define DULT_RSP_COMPLETED  0x0303u   /* Sound_Completed */

/* ---------------- progress / logging ------------------------------------ */

static dult_progress_fn s_prog = nullptr;

void dult_set_progress(dult_progress_fn fn) { s_prog = fn; }

static void plog(const char *fmt, ...)
{
    char b[72];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    Serial.printf("[DULT] %s\n", b);
    if (s_prog) s_prog(b);
}

/* Persistent CSV so the protocol answers survive a power cycle. This is
 * the whole reason all three surfaces are implemented: which one answers
 * on a given tag is unpublished data. */
static void dult_csv(const dult_target_t *t, const char *op,
                     dult_proto_t proto, bool ok, const char *detail)
{
    if (!sd_mount()) return;
    SD.mkdir("/poseidon");
    sd_rotate_on_size("/poseidon/dult-log.csv", 128 * 1024);
    bool fresh = !SD.exists("/poseidon/dult-log.csv");
    File f = SD.open("/poseidon/dult-log.csv", FILE_APPEND);
    if (!f) return;
    if (fresh) f.println("ms,mac,kind,state,op,protocol,result,detail");
    f.printf("%lu,%02X:%02X:%02X:%02X:%02X:%02X,%s,%s,%s,%s,%s,\"%s\"\n",
             (unsigned long)millis(),
             t->addr[5], t->addr[4], t->addr[3], t->addr[2], t->addr[1], t->addr[0],
             dult_kind_name(t->kind), dult_state_name(t->state), op,
             dult_proto_name(proto), ok ? "ok" : "fail", detail ? detail : "");
    f.close();
}

/* ---------------- names -------------------------------------------------- */

const char *dult_proto_name(dult_proto_t p)
{
    switch (p) {
    case DULT_PROTO_AIRTAG:  return "AirTag-legacy";
    case DULT_PROTO_DULT:    return "DULT";
    case DULT_PROTO_FMN:     return "AppleFMN";
    case DULT_PROTO_FMN_ALT: return "AppleFMN-alt";
    default:                 return "none";
    }
}

const char *dult_kind_name(dult_kind_t k)
{
    switch (k) {
    case DULT_KIND_APPLE_FM: return "FindMy";
    case DULT_KIND_DULT:     return "DULT";
    case DULT_KIND_GOOGLE:   return "Google";
    case DULT_KIND_TILE:     return "Tile";
    case DULT_KIND_SAMSUNG:  return "SmartTag";
    default:                 return "unknown";
    }
}

const char *dult_state_name(dult_state_t s)
{
    switch (s) {
    case DULT_STATE_SEPARATED:  return "SEPARATED";
    case DULT_STATE_NEAR_OWNER: return "owner-near";
    default:                    return "state?";
    }
}

bool dult_kind_can_sound(dult_kind_t k)
{
    /* Tile and Samsung SmartTag have no non-owner sound protocol at all.
     * AirGuard deliberately leaves both non-connectable (issues #163 and
     * #196, both closed). Offering the action would be a lie. */
    return k == DULT_KIND_APPLE_FM || k == DULT_KIND_DULT || k == DULT_KIND_GOOGLE;
}

static const char *dult_status_name(uint16_t s)
{
    switch (s) {
    case 0x0000: return "Success";
    case 0x0001: return "Invalid_state";
    case 0x0002: return "Invalid_config";
    case 0x0003: return "Invalid_length";
    case 0x0004: return "Invalid_param";
    case 0xFFFF: return "Invalid_command";
    default:     return "status?";
    }
}

/* ---------------- classification from the advertisement ------------------ */

static bool uuid_is(const NimBLEUUID &u, const char *s)
{
    return u.equals(NimBLEUUID(s));
}

bool dult_classify(const NimBLEAdvertisedDevice *d, dult_target_t *out)
{
    if (!d || !out) return false;
    memset(out, 0, sizeof(*out));
    out->battery = 0xFF;
    out->state   = DULT_STATE_UNKNOWN;
    out->hint    = DULT_PROTO_NONE;

    bool matched = false;

    if (d->haveManufacturerData()) {
        std::string md = d->getManufacturerData();
        if (md.size() >= 3) {
            uint16_t cid = (uint8_t)md[0] | ((uint8_t)md[1] << 8);
            if (cid == APPLE_CID && (uint8_t)md[2] == APPLE_TYPE_FINDMY) {
                out->kind = DULT_KIND_APPLE_FM;
                strcpy(out->label, "FindMy");
                /* Separated iff the payload-length byte is 0x19 (full
                 * public key present). Anything else means the owner is
                 * still connected and Sound_Start will be refused. */
                if (md.size() >= 4)
                    out->state = ((uint8_t)md[3] == APPLE_SEPARATED)
                                     ? DULT_STATE_SEPARATED
                                     : DULT_STATE_NEAR_OWNER;
                if (md.size() >= 5) out->battery = ((uint8_t)md[4] >> 6) & 0x03;
                matched = true;
            } else if (cid == 0x0075) {
                out->kind = DULT_KIND_SAMSUNG;
                strcpy(out->label, "SmartTag");
                matched = true;
            }
        }
    }

    if (!matched && d->haveServiceData()) {
        for (uint8_t i = 0; i < d->getServiceDataCount(); ++i) {
            NimBLEUUID su = d->getServiceDataUUID(i);
            if (su.equals(NimBLEUUID((uint16_t)0xFCB2))) {
                /* DULT location-enabled advertisement. Near-owner bit is
                 * the LSB of payload byte 14: 1 = near owner, 0 = separated. */
                out->kind = DULT_KIND_DULT;
                strcpy(out->label, "DULT");
                std::string sd = d->getServiceData(i);
                if (sd.size() >= 15)
                    out->state = ((uint8_t)sd[14] & 0x01) ? DULT_STATE_NEAR_OWNER
                                                          : DULT_STATE_SEPARATED;
                matched = true;
                break;
            }
        }
    }

    if (d->haveServiceUUID()) {
        for (int i = 0; i < d->getServiceUUIDCount(); ++i) {
            NimBLEUUID u = d->getServiceUUID(i);
            if (!matched) {
                if (u.equals(NimBLEUUID((uint16_t)0xFEED)) ||
                    u.equals(NimBLEUUID((uint16_t)0xFD84))) {
                    out->kind = DULT_KIND_TILE;
                    strcpy(out->label, "Tile");
                    matched = true;
                } else if (u.equals(NimBLEUUID((uint16_t)0xFD5A))) {
                    out->kind = DULT_KIND_SAMSUNG;
                    strcpy(out->label, "SmartTag");
                    matched = true;
                } else if (u.equals(NimBLEUUID((uint16_t)0xFEAA))) {
                    /* Eddystone - what Google Find My Device trackers
                     * advertise. Same DULT GATT service underneath. */
                    out->kind = DULT_KIND_GOOGLE;
                    strcpy(out->label, "Google");
                    matched = true;
                }
            }
            /* Pre-connect protocol hint: some FMN accessories DO advertise
             * their service, which saves a failed discovery round trip.
             * AirTags never do, which is why there is a fallback list. */
            if (uuid_is(u, UUID_DULT_SVC))              out->hint = DULT_PROTO_DULT;
            else if (u.equals(NimBLEUUID(UUID16_FMN_SVC))) out->hint = DULT_PROTO_FMN;
            else if (uuid_is(u, UUID_AIRTAG_SVC))       out->hint = DULT_PROTO_AIRTAG;
        }
    }

    if (!matched) return false;

    NimBLEAddress a = d->getAddress();
    memcpy(out->addr, a.getBase()->val, 6);
    out->addr_public = (d->getAddressType() == BLE_ADDR_PUBLIC);
    out->rssi = (int8_t)d->getRSSI();
    if (!out->label[0]) strcpy(out->label, "tracker");
    return true;
}

/* ---------------- connection plumbing ------------------------------------ */

class dult_cli_cb : public NimBLEClientCallbacks {
  public:
    volatile int  reason = 0;
    volatile bool down   = false;
    void onDisconnect(NimBLEClient *, int r) override { reason = r; down = true; }
    void onConnectFail(NimBLEClient *, int r) override { reason = r; }
};
static dult_cli_cb s_cli_cb;

/* NimBLE maps HCI reason 0x13 (remote user terminated) to 0x0213. That is
 * the AirTag hanging up on us, which after a successful write is SUCCESS,
 * not an error - AirGuard treats the same status 19 as EventCompleted. */
#define REASON_REMOTE_TERM 0x0213

static volatile bool  s_ind_ready = false;
static uint8_t        s_ind_buf[64];
static volatile uint8_t s_ind_len = 0;

static void ind_cb(NimBLERemoteCharacteristic *, uint8_t *data, size_t len, bool)
{
    if (len > sizeof(s_ind_buf)) len = sizeof(s_ind_buf);
    memcpy(s_ind_buf, data, len);
    s_ind_len   = (uint8_t)len;
    s_ind_ready = true;
}

static bool wait_ind(uint32_t ms)
{
    uint32_t t0 = millis();
    while (!s_ind_ready) {
        if (millis() - t0 > ms) return false;
        delay(5);
    }
    return true;
}

static NimBLEClient *dult_open(const dult_target_t *t, bool want_mtu)
{
    NimBLEClient *c = NimBLEDevice::createClient();
    if (!c) { plog("no client slot free"); return nullptr; }

    /* Latency is the one real engineering risk here: the tag tears down
     * unauthenticated links fast, and every ATT round trip costs one
     * connection interval. 6..12 = 7.5..15 ms, zero slave latency,
     * 1 s supervision timeout. */
    c->setConnectionParams(6, 12, 0, 100);
    c->setConnectTimeout(4000);
    s_cli_cb.reason = 0;
    s_cli_cb.down   = false;
    c->setClientCallbacks(&s_cli_cb, false);

    NimBLEAddress addr(t->addr, t->addr_public ? BLE_ADDR_PUBLIC : BLE_ADDR_RANDOM);
    /* want_mtu=false skips the MTU exchange entirely - one fewer round
     * trip on a connection we are racing. A 1..3 byte control-point write
     * fits the default 23-byte ATT MTU with room to spare. */
    if (!c->connect(addr, true, false, want_mtu)) {
        plog("connect failed rc=%d", c->getLastError());
        NimBLEDevice::deleteClient(c);
        return nullptr;
    }
    return c;
}

static void dult_close(NimBLEClient *c)
{
    if (!c) return;
    if (c->isConnected()) c->disconnect();
    NimBLEDevice::deleteClient(c);
}

/* ---------------- per-protocol attempts ---------------------------------- */

enum { ATT_ABSENT = 0, ATT_OK = 1, ATT_FAIL = -1 };

/* Subscribe for indications (DULT Table 18 says the responses are
 * indications, not notifications) and fall back to notifications if the
 * characteristic only advertises that, exactly as AirGuard does. */
static void arm_indications(NimBLERemoteCharacteristic *ch)
{
    s_ind_ready = false;
    s_ind_len   = 0;
    if (ch->canIndicate())      ch->subscribe(false, ind_cb, true);
    else if (ch->canNotify())   ch->subscribe(true, ind_cb, true);
}

/* Returns ATT_OK / ATT_FAIL when a Command_Response arrived, ATT_ABSENT
 * when nothing came back inside the window (which is not fatal - several
 * accessories just play the sound). */
static int check_response(void)
{
    if (!wait_ind(2000)) { plog("no indication (may still be ok)"); return ATT_ABSENT; }
    if (s_ind_len < 2)   { plog("short indication len=%u", s_ind_len); return ATT_ABSENT; }
    uint16_t rsp = (uint16_t)s_ind_buf[0] | ((uint16_t)s_ind_buf[1] << 8);
    if (rsp == DULT_RSP_COMMAND && s_ind_len >= 6) {
        uint16_t st = (uint16_t)s_ind_buf[4] | ((uint16_t)s_ind_buf[5] << 8);
        plog("resp 0x0302 %s", dult_status_name(st));
        return (st == 0x0000) ? ATT_OK : ATT_FAIL;
    }
    if (rsp == DULT_RSP_COMPLETED) { plog("Sound_Completed"); return ATT_OK; }
    plog("resp opcode 0x%04X", rsp);
    return ATT_ABSENT;
}

static int try_airtag(NimBLEClient *c, bool stop)
{
    if (stop) return ATT_ABSENT;   /* legacy path has no stop command */
    NimBLERemoteService *svc = c->getService(UUID_AIRTAG_SVC);
    if (!svc) return ATT_ABSENT;
    NimBLERemoteCharacteristic *ch = svc->getCharacteristic(UUID_AIRTAG_CHR);
    if (!ch) { plog("7DFC9000 svc but no 9001 char"); return ATT_ABSENT; }
    plog("AirTag legacy svc found");
    /* Write WITH response. No CCCD, no indications, no descriptors -
     * that is what makes this the fastest of the three. */
    if (!ch->writeValue(&OP_AIRTAG_BEEP, 1, true)) {
        plog("0xAF write failed rc=%d", c->getLastError());
        return ATT_FAIL;
    }
    plog("wrote 0xAF");
    return ATT_OK;
}

static int try_dult(NimBLEClient *c, bool stop)
{
    NimBLERemoteService *svc = c->getService(UUID_DULT_SVC);
    if (!svc) return ATT_ABSENT;
    NimBLERemoteCharacteristic *ch = svc->getCharacteristic(UUID_DULT_CHR);
    if (!ch) { plog("DULT svc but no 8E0C0001 char"); return ATT_ABSENT; }
    plog("DULT svc found");
    arm_indications(ch);
    const uint8_t *op = stop ? OP_SOUND_STOP : OP_SOUND_START;
    if (!ch->writeValue(op, 2, true)) {
        plog("DULT write failed rc=%d", c->getLastError());
        return ATT_FAIL;
    }
    plog("wrote %02X %02X", op[0], op[1]);
    int r = check_response();
    return (r == ATT_FAIL) ? ATT_FAIL : ATT_OK;
}

/* The leading 0x01 in the FMN payload is UNVERIFIED - no public source
 * says what it means. Copy it literally first; if that is rejected, try
 * the bare DULT opcode pair on the same characteristic and log which
 * form worked. That answer is not published anywhere. */
static int try_fmn(NimBLEClient *c, bool stop, dult_proto_t *which)
{
    NimBLERemoteService *svc = c->getService(NimBLEUUID(UUID16_FMN_SVC));
    if (!svc) return ATT_ABSENT;
    NimBLERemoteCharacteristic *ch = svc->getCharacteristic(UUID_FMN_CHR);
    if (!ch) { plog("fd44 svc but no 4F860003 char"); return ATT_ABSENT; }
    plog("Apple FMN svc found");
    arm_indications(ch);

    const uint8_t *op3 = stop ? OP_FMN_STOP : OP_FMN_START;
    bool wrote = ch->writeValue(op3, 3, true);
    int  r     = ATT_FAIL;
    if (wrote) {
        plog("wrote %02X %02X %02X", op3[0], op3[1], op3[2]);
        r = check_response();
        if (r != ATT_FAIL) { *which = DULT_PROTO_FMN; return ATT_OK; }
    } else {
        plog("FMN 3-byte write failed rc=%d", c->getLastError());
    }

    /* Fall back to the 2-byte form without the undocumented header. */
    plog("retry FMN without 0x01");
    arm_indications(ch);
    const uint8_t *op2 = stop ? OP_SOUND_STOP : OP_SOUND_START;
    if (!ch->writeValue(op2, 2, true)) {
        plog("FMN 2-byte write failed too");
        return ATT_FAIL;
    }
    plog("wrote %02X %02X (alt)", op2[0], op2[1]);
    r = check_response();
    *which = DULT_PROTO_FMN_ALT;
    return (r == ATT_FAIL) ? ATT_FAIL : ATT_OK;
}

/* ---------------- sound driver ------------------------------------------- */

static void proto_order(const dult_target_t *t, dult_proto_t *out, int *n)
{
    int i = 0;
    if (t->kind == DULT_KIND_APPLE_FM) {
        /* AirGuard tries the legacy path FIRST for AirTags. */
        out[i++] = DULT_PROTO_AIRTAG;
        out[i++] = DULT_PROTO_DULT;
        out[i++] = DULT_PROTO_FMN;
    } else {
        out[i++] = DULT_PROTO_DULT;
        out[i++] = DULT_PROTO_FMN;
        out[i++] = DULT_PROTO_AIRTAG;
    }
    /* Advertised-service hint jumps the queue. */
    if (t->hint != DULT_PROTO_NONE) {
        for (int j = 0; j < i; ++j) {
            if (out[j] == t->hint) {
                for (int k = j; k > 0; --k) out[k] = out[k - 1];
                out[0] = t->hint;
                break;
            }
        }
    }
    *n = i;
}

bool dult_sound(const dult_target_t *t, bool stop, dult_proto_t *answered,
                char *summary, size_t summary_sz)
{
    if (answered) *answered = DULT_PROTO_NONE;
    if (summary && summary_sz) summary[0] = 0;

    if (!dult_kind_can_sound(t->kind)) {
        snprintf(summary, summary_sz, "%s has no sound protocol", dult_kind_name(t->kind));
        return false;
    }

    dult_proto_t order[4];
    int order_n = 0;
    proto_order(t, order, &order_n);

    for (int attempt = 1; attempt <= 3; ++attempt) {
        plog("attempt %d/3 connecting", attempt);
        NimBLEClient *c = dult_open(t, false);
        if (!c) {
            if (attempt == 3) {
                snprintf(summary, summary_sz, "connect failed x3");
                dult_csv(t, stop ? "stop" : "start", DULT_PROTO_NONE, false, "connect failed");
            }
            delay(200);
            continue;
        }

        bool present   = false;
        bool link_lost = false;
        for (int i = 0; i < order_n; ++i) {
            dult_proto_t p = order[i];
            dult_proto_t used = p;
            int r;
            switch (p) {
            case DULT_PROTO_AIRTAG: r = try_airtag(c, stop); break;
            case DULT_PROTO_DULT:   r = try_dult(c, stop);   break;
            default:                r = try_fmn(c, stop, &used); break;
            }
            if (r == ATT_ABSENT) {
                /* Absent means the service genuinely is not there. A
                 * mid-probe teardown is a different thing entirely and
                 * must not be reported as "device has no control point" -
                 * that is the AirTag racing us, so retry instead. */
                if (!c->isConnected()) {
                    plog("peer hung up (0x%04X) mid-probe", s_cli_cb.reason);
                    link_lost = true;
                    break;
                }
                continue;
            }
            present = true;
            if (r == ATT_OK) {
                if (answered) *answered = used;
                snprintf(summary, summary_sz, "%s answered", dult_proto_name(used));
                plog("OK via %s", dult_proto_name(used));
                dult_csv(t, stop ? "stop" : "start", used, true, summary);
                dult_close(c);
                return true;
            }
            snprintf(summary, summary_sz, "%s rejected", dult_proto_name(used));
            dult_csv(t, stop ? "stop" : "start", used, false, summary);
        }

        int reason = s_cli_cb.reason;
        dult_close(c);

        if (!present && !link_lost) {
            if (stop) {
                snprintf(summary, summary_sz, "no stop path (legacy has none)");
            } else {
                snprintf(summary, summary_sz, "no control point on this device");
            }
            plog("none of the 3 surfaces present");
            dult_csv(t, stop ? "stop" : "start", DULT_PROTO_NONE, false, summary);
            return false;
        }
        if (reason == REASON_REMOTE_TERM)
            plog("peer terminated 0x0213, retrying");
        if (attempt == 3 && link_lost)
            snprintf(summary, summary_sz, "tag tore down link x3");
        delay(250);
    }

    if (!summary || !summary_sz) return false;
    if (!summary[0]) snprintf(summary, summary_sz, "no surface answered");
    dult_csv(t, stop ? "stop" : "start", DULT_PROTO_NONE, false, summary);
    return false;
}

/* ---------------- silent enumeration ------------------------------------- */

struct dult_cmd_t {
    uint16_t op;
    uint16_t rsp;
    const char *name;
    char    *dst;
    size_t   dst_sz;
    bool     as_text;
};

/* Render an indication payload. Prefer a UTF-8 string for the name
 * fields; everything else is shown as raw hex so nothing is invented. */
static void fmt_payload(const uint8_t *p, size_t n, bool as_text,
                        char *out, size_t out_sz)
{
    if (n == 0) { snprintf(out, out_sz, "(empty)"); return; }
    if (as_text) {
        bool printable = true;
        for (size_t i = 0; i < n; ++i)
            if (p[i] < 32 || p[i] > 126) { printable = false; break; }
        if (printable) {
            size_t k = n < out_sz - 1 ? n : out_sz - 1;
            memcpy(out, p, k);
            out[k] = 0;
            return;
        }
    }
    size_t used = 0;
    for (size_t i = 0; i < n && used + 3 < out_sz; ++i)
        used += snprintf(out + used, out_sz - used, "%02X", p[i]);
    out[used < out_sz ? used : out_sz - 1] = 0;
}

bool dult_read_details(const dult_target_t *t, dult_detail_t *out,
                       char *summary, size_t summary_sz)
{
    memset(out, 0, sizeof(*out));
    if (summary && summary_sz) summary[0] = 0;

    dult_cmd_t cmds[] = {
        { 0x0004, 0x0804, "mfg",   out->mfg,      sizeof(out->mfg),      true  },
        { 0x0005, 0x0805, "model", out->model,    sizeof(out->model),    true  },
        { 0x0006, 0x0806, "categ", out->category, sizeof(out->category), false },
        { 0x000A, 0x080A, "fw",    out->fw,       sizeof(out->fw),       false },
        { 0x0009, 0x0809, "netid", out->netid,    sizeof(out->netid),    false },
        { 0x000C, 0x080C, "batt",  out->batt,     sizeof(out->batt),     false },
    };
    const int NCMD = (int)(sizeof(cmds) / sizeof(cmds[0]));

    for (int attempt = 1; attempt <= 3; ++attempt) {
        plog("read attempt %d/3", attempt);
        /* MTU exchange IS wanted here: DULT 3.11.2 requires the accessory
         * to accept an MTU large enough for its longest response, and a
         * Network ID will not fit in 20 bytes. */
        NimBLEClient *c = dult_open(t, true);
        if (!c) { delay(200); continue; }

        NimBLERemoteService *svc = c->getService(UUID_DULT_SVC);
        if (!svc) {
            plog("no DULT service - reads unavailable");
            snprintf(summary, summary_sz, "no DULT service on this device");
            dult_csv(t, "read", DULT_PROTO_NONE, false, "no DULT service");
            dult_close(c);
            return false;
        }
        NimBLERemoteCharacteristic *ch = svc->getCharacteristic(UUID_DULT_CHR);
        if (!ch) {
            snprintf(summary, summary_sz, "DULT svc without control point");
            dult_close(c);
            return false;
        }

        int got = 0;
        for (int i = 0; i < NCMD; ++i) {
            if (!c->isConnected()) { plog("link dropped at %s", cmds[i].name); break; }
            arm_indications(ch);
            uint8_t op[2] = { (uint8_t)(cmds[i].op & 0xFF), (uint8_t)(cmds[i].op >> 8) };
            if (!ch->writeValue(op, 2, true)) {
                snprintf(cmds[i].dst, cmds[i].dst_sz, "write failed");
                continue;
            }
            if (!wait_ind(2000)) { snprintf(cmds[i].dst, cmds[i].dst_sz, "no reply"); continue; }
            if (s_ind_len < 2)   { snprintf(cmds[i].dst, cmds[i].dst_sz, "short reply"); continue; }
            /* Framing: [2-byte response opcode LE][payload]. */
            uint16_t rsp = (uint16_t)s_ind_buf[0] | ((uint16_t)s_ind_buf[1] << 8);
            if (rsp == DULT_RSP_COMMAND && s_ind_len >= 6) {
                uint16_t st = (uint16_t)s_ind_buf[4] | ((uint16_t)s_ind_buf[5] << 8);
                snprintf(cmds[i].dst, cmds[i].dst_sz, "%s", dult_status_name(st));
                plog("%s -> %s", cmds[i].name, dult_status_name(st));
                continue;
            }
            if (rsp != cmds[i].rsp)
                plog("%s: opcode 0x%04X (want 0x%04X)", cmds[i].name, rsp, cmds[i].rsp);
            fmt_payload(s_ind_buf + 2, s_ind_len - 2, cmds[i].as_text,
                        cmds[i].dst, cmds[i].dst_sz);
            plog("%s = %s", cmds[i].name, cmds[i].dst);
            got++;
        }

        dult_close(c);
        if (got > 0) {
            out->any = true;
            snprintf(summary, summary_sz, "%d/%d fields read", got, NCMD);
            dult_csv(t, "read", DULT_PROTO_DULT, true, summary);
            return true;
        }
        delay(250);
    }
    if (!summary[0]) snprintf(summary, summary_sz, "no fields read");
    dult_csv(t, "read", DULT_PROTO_DULT, false, summary);
    return false;
}

/* ---------------- UI ------------------------------------------------------ */

#define LOGL_N 8
#define LOGL_W 46
#define LOG_Y0 (BODY_Y + 74)
/* Rows are clamped to whatever body height the board actually has, so the
 * same code renders on the 320x170 T-Embed and the 240x135 Cardputer
 * without spilling into the footer. */
#define LOG_ROWS (((BODY_H - 74) / 10) < LOGL_N ? ((BODY_H - 74) / 10) : LOGL_N)

static char s_loglines[LOGL_N][LOGL_W];
static int  s_logn = 0;

static void log_paint(void)
{
    auto &d = M5Cardputer.Display;
    for (int i = 0; i < LOG_ROWS; ++i) {
        int y = LOG_Y0 + i * 10;
        d.fillRect(4, y, SCR_W - 8, 9, T_BG);
        if (i < s_logn) {
            d.setTextColor(T_DIM, T_BG);
            d.setCursor(6, y);
            d.print(s_loglines[i]);
        }
    }
}

static void log_push(const char *line)
{
    const int rows = LOG_ROWS;
    if (s_logn >= rows) {
        for (int i = 1; i < rows; ++i) memcpy(s_loglines[i - 1], s_loglines[i], LOGL_W);
        s_logn = rows - 1;
    }
    snprintf(s_loglines[s_logn], LOGL_W, "%s", line);
    s_logn++;
    log_paint();
}

static void draw_header(const dult_target_t *t)
{
    auto &d = M5Cardputer.Display;
    ui_clear_body();
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2);
    d.printf("TRACKER  %s", t->label);
    d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT);

    d.setTextColor(T_FG, T_BG);
    d.setCursor(4, BODY_Y + 16);
    d.printf("%02X:%02X:%02X:%02X:%02X:%02X  %ddBm  %s",
             t->addr[5], t->addr[4], t->addr[3], t->addr[2], t->addr[1], t->addr[0],
             t->rssi, t->addr_public ? "pub" : "rnd");

    uint16_t col = (t->state == DULT_STATE_SEPARATED) ? T_GOOD
                 : (t->state == DULT_STATE_NEAR_OWNER) ? T_WARN : T_DIM;
    d.setTextColor(col, T_BG);
    d.setCursor(4, BODY_Y + 28);
    d.printf("state: %s", dult_state_name(t->state));
    if (t->battery != 0xFF) {
        static const char *bl[4] = { "full", "medium", "low", "very low" };
        d.printf("   batt: %s", bl[t->battery & 3]);
    }

    d.setTextColor(T_DIM, T_BG);
    d.setCursor(4, BODY_Y + 40);
    if (!dult_kind_can_sound(t->kind)) {
        d.setTextColor(T_BAD, T_BG);
        d.printf("no non-owner sound exists for %s", dult_kind_name(t->kind));
    } else if (t->state == DULT_STATE_NEAR_OWNER) {
        d.printf("owner in range - tag will refuse sound");
        d.setCursor(4, BODY_Y + 50);
        d.printf("separation takes ~13min (AirTag) ~9min (AirPods)");
    } else if (t->state == DULT_STATE_SEPARATED) {
        d.printf("separated - sound available to non-owners");
    } else {
        d.printf("state unknown - sound may be refused");
    }
    log_paint();
}

static void show_details(const dult_target_t *t, const dult_detail_t *det,
                         const char *summary)
{
    auto &d = M5Cardputer.Display;
    ui_clear_body();
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2);
    d.print("DULT DETAILS (silent)");
    d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT);

    struct { const char *k; const char *v; } rows[] = {
        { "manufacturer", det->mfg },
        { "model",        det->model },
        { "category",     det->category },
        { "firmware",     det->fw },
        { "network id",   det->netid },
        { "battery",      det->batt },
    };
    for (int i = 0; i < 6; ++i) {
        int y = BODY_Y + 18 + i * 12;
        d.setTextColor(T_DIM, T_BG);
        d.setCursor(6, y);
        d.print(rows[i].k);
        d.setTextColor(T_FG, T_BG);
        d.setCursor(96, y);
        d.print(rows[i].v[0] ? rows[i].v : "-");
    }
    d.setTextColor(T_ACCENT2, T_BG);
    d.setCursor(4, BODY_Y + 96);
    d.print(summary);

    ui_draw_footer("back=return");
    while (true) {
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(30); continue; }
        if (k == PK_ESC || k == PK_ENTER) break;
    }
}

void dult_target_screen(const dult_target_t *t)
{
    radio_switch(RADIO_BLE);
    s_logn = 0;
    draw_header(t);
    ui_draw_footer("hold=actions  back=return");
    ui_draw_status(radio_name(), "dult");

    const bool can_sound = dult_kind_can_sound(t->kind);
    const bool blocked   = (t->state == DULT_STATE_NEAR_OWNER);

    while (true) {
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(30); continue; }
        if (k == PK_ESC) break;
        if (k != PK_ACTIONS && k != PK_ENTER) continue;

        int pick;
        int act_play = -1, act_stop = -1, act_read = -1;
        if (can_sound) {
            const char *lbl[4];
            int n = 0;
            act_play = n; lbl[n++] = blocked ? "Play sound (blocked)" : "Play sound";
            act_stop = n; lbl[n++] = "Stop sound";
            act_read = n; lbl[n++] = "Read details";
            lbl[n++] = "Back";
            pick = ui_action_menu("TRACKER", lbl, n);
        } else {
            /* Tile / SmartTag: no non-owner control point exists at all,
             * so the sound actions are absent rather than present and
             * silently failing. */
            static const char *const lbl[] = { "Back" };
            pick = ui_action_menu("NO SOUND PATH", lbl, 1);
        }
        draw_header(t);
        ui_draw_footer("hold=actions  back=return");
        if (pick < 0 || !can_sound) continue;

        if (pick == act_read) {
            dult_set_progress(log_push);
            dult_detail_t det;
            char sum[48];
            log_push("reading DULT info (silent)...");
            bool ok = dult_read_details(t, &det, sum, sizeof(sum));
            dult_set_progress(nullptr);
            if (ok) show_details(t, &det, sum);
            else    ui_toast(sum, T_WARN, 1800);
            draw_header(t);
            ui_draw_footer("hold=actions  back=return");
            continue;
        }

        if (pick != act_play && pick != act_stop) continue;
        bool stop = (pick == act_stop);

        if (!stop && blocked) {
            /* Do not let the operator mash a button that cannot work
             * without first saying why. Still allow the attempt, because
             * our state read is one advertisement byte. */
            auto &d = M5Cardputer.Display;
            ui_clear_body();
            d.setTextColor(T_WARN, T_BG);
            d.setCursor(4, BODY_Y + 4);  d.print("OWNER IS IN RANGE");
            d.setTextColor(T_FG, T_BG);
            d.setCursor(4, BODY_Y + 20); d.print("The tracker is in near-owner state.");
            d.setCursor(4, BODY_Y + 32); d.print("DULT 3.13.4: it MUST answer");
            d.setCursor(4, BODY_Y + 44); d.print("Sound_Start with Invalid_command.");
            d.setTextColor(T_DIM, T_BG);
            d.setCursor(4, BODY_Y + 60); d.print("Separation takes about 13 min for an");
            d.setCursor(4, BODY_Y + 72); d.print("AirTag, 9 min for AirPods, measured.");
            d.setCursor(4, BODY_Y + 84); d.print("Walk it away, wait, then rescan.");
            ui_draw_footer("select=try anyway  back=cancel");
            bool go = false;
            while (true) {
                uint16_t k2 = input_poll();
                if (k2 == PK_NONE) { delay(30); continue; }
                if (k2 == PK_ESC) break;
                if (k2 == PK_ENTER) { go = true; break; }
            }
            draw_header(t);
            ui_draw_footer("hold=actions  back=return");
            if (!go) continue;
        }

        dult_set_progress(log_push);
        dult_proto_t answered = DULT_PROTO_NONE;
        char sum[48];
        log_push(stop ? "sending Sound_Stop..." : "sending Sound_Start...");
        bool ok = dult_sound(t, stop, &answered, sum, sizeof(sum));
        dult_set_progress(nullptr);

        auto &d = M5Cardputer.Display;
        d.fillRect(0, BODY_Y + 62, SCR_W, 10, T_BG);
        d.setTextColor(ok ? T_GOOD : T_BAD, T_BG);
        d.setCursor(4, BODY_Y + 62);
        d.printf("%s: %s", ok ? "OK" : "FAIL", sum);
        if (ok && !stop) M5Cardputer.Speaker.tone(2600, 90);
        ui_draw_footer("hold=actions  back=return");
    }
}
