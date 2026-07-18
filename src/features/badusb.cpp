/*
 * badusb — USB HID keyboard payload runner.
 *
 * Uses the ESP32-S3's native USB in HID mode. On the host computer the
 * Cardputer appears as a standard USB keyboard. We run DuckyScript-lite
 * payloads either from the built-in library or from SD (/poseidon/ducky/*.txt).
 *
 * Supported DuckyScript commands:
 *   REM <comment>
 *   DELAY <ms>
 *   STRING <text>       — type the rest of the line verbatim
 *   ENTER / TAB / ESC / SPACE / BKSP
 *   GUI [key]           — Windows/Cmd (+ optional letter)
 *   CTRL / ALT / SHIFT [key]
 *   COMBO CTRL ALT T    — chord any modifiers + final key
 */
#include "app.h"
#include "../theme.h"
#include "ui.h"
#include "input.h"
#include "mimir.h"
#include "kerberos_bootmode.h"
#include <USB.h>
#include <USBHIDKeyboard.h>
#include <SD.h>

/* The keyboard registers its HID interface in its constructor (static-init).
 * In KERBEROS key mode we must present ONLY the FIDO HID interface, so the
 * keyboard is not constructed then (nullptr). BadUSB is unreachable in key
 * mode anyway: loop() runs KERBEROS directly and reboots to normal on exit. */
static USBHIDKeyboard *s_kbd_obj = kerb_boot_key_mode() ? nullptr : new USBHIDKeyboard();
#define s_kbd (*s_kbd_obj)
static bool           s_hid_up = false;

static void hid_ensure(void)
{
    if (s_hid_up) return;
    USB.begin();
    s_kbd.begin();
    delay(400);  /* host enumeration settle */
    s_hid_up = true;
}

static void type_string(const char *s)
{
    for (; *s; ++s) {
        s_kbd.write((uint8_t)*s);
        delay(4);
    }
}

/* Built-in payloads. DuckyScript lite, one statement per line. */
struct payload_t { const char *name; const char *script; };

/* Extended payload library — must include AFTER payload_t is defined
 * since the headers declare arrays of payload_t but don't define the
 * struct themselves. */
#include "badusb_extras.h"
#include "badusb_pranks_data.h"

static const char PAY_HELLO[] =
    "DELAY 500\n"
    "STRING hello from poseidon\n"
    "ENTER\n";

static const char PAY_NOTEPAD[] =
    "GUI r\n"
    "DELAY 400\n"
    "STRING notepad\n"
    "ENTER\n"
    "DELAY 800\n"
    "STRING you have been pwned by POSEIDON\n"
    "ENTER\n"
    "STRING   commander of the deep\n";

static const char PAY_RICKROLL[] =
    "GUI r\n"
    "DELAY 400\n"
    "STRING https://youtu.be/dQw4w9WgXcQ\n"
    "ENTER\n";

static const char PAY_LOCK[] =
    "GUI l\n";

static const char PAY_TERMINAL[] =
    "CTRL ALT t\n"
    "DELAY 500\n"
    "STRING echo pwned > /tmp/poseidon\n"
    "ENTER\n";

static const payload_t s_payloads[] = {
    { "Hello",      PAY_HELLO     },
    { "Notepad",    PAY_NOTEPAD   },
    { "Rickroll",   PAY_RICKROLL  },
    { "Lock",       PAY_LOCK      },
    { "Terminal",   PAY_TERMINAL  },
};
#define PAY_N (sizeof(s_payloads)/sizeof(s_payloads[0]))

static int keycode(const char *k)
{
    /* Single printable char → its ASCII (keyboard library accepts these). */
    if (strlen(k) == 1) return (uint8_t)k[0];
    if (!strcasecmp(k, "ENTER"))  return KEY_RETURN;
    if (!strcasecmp(k, "TAB"))    return KEY_TAB;
    if (!strcasecmp(k, "ESC"))    return KEY_ESC;
    if (!strcasecmp(k, "SPACE"))  return ' ';
    if (!strcasecmp(k, "BKSP"))   return KEY_BACKSPACE;
    if (!strcasecmp(k, "DEL"))    return KEY_DELETE;
    if (!strcasecmp(k, "UP"))     return KEY_UP_ARROW;
    if (!strcasecmp(k, "DOWN"))   return KEY_DOWN_ARROW;
    if (!strcasecmp(k, "LEFT"))   return KEY_LEFT_ARROW;
    if (!strcasecmp(k, "RIGHT"))  return KEY_RIGHT_ARROW;
    if (!strcasecmp(k, "F1"))     return KEY_F1;
    if (!strcasecmp(k, "F2"))     return KEY_F2;
    if (!strcasecmp(k, "F3"))     return KEY_F3;
    if (!strcasecmp(k, "F4"))     return KEY_F4;
    return 0;
}

static void run_modifier_combo(uint8_t modifier, const char *tail)
{
    /* tail may be another modifier or a final key. */
    if (!tail || !*tail) { s_kbd.press(modifier); delay(40); s_kbd.release(modifier); return; }
    char kbuf[16];
    strncpy(kbuf, tail, sizeof(kbuf) - 1);
    kbuf[sizeof(kbuf) - 1] = '\0';

    int k = keycode(kbuf);
    if (k) {
        s_kbd.press(modifier);
        s_kbd.press(k);
        delay(40);
        s_kbd.releaseAll();
    }
}

static void exec_line(const char *line)
{
    while (*line == ' ' || *line == '\t') ++line;
    if (!*line || *line == '\n' || *line == '\r') return;
    char buf[160];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    /* strip trailing newline */
    for (int i = strlen(buf) - 1; i >= 0 && (buf[i] == '\r' || buf[i] == '\n'); --i) buf[i] = 0;

    char *cmd = strtok(buf, " ");
    if (!cmd) return;
    char *arg = strtok(nullptr, "");

    if (!strcasecmp(cmd, "REM")) return;
    if (!strcasecmp(cmd, "DELAY")) { delay(arg ? atoi(arg) : 100); return; }
    if (!strcasecmp(cmd, "STRING")) { if (arg) type_string(arg); return; }
    if (!strcasecmp(cmd, "ENTER")) { s_kbd.write(KEY_RETURN); return; }
    if (!strcasecmp(cmd, "TAB"))   { s_kbd.write(KEY_TAB); return; }
    if (!strcasecmp(cmd, "ESC"))   { s_kbd.write(KEY_ESC); return; }
    if (!strcasecmp(cmd, "SPACE")) { s_kbd.write(' '); return; }
    if (!strcasecmp(cmd, "BKSP"))  { s_kbd.write(KEY_BACKSPACE); return; }

    if (!strcasecmp(cmd, "GUI"))   { run_modifier_combo(KEY_LEFT_GUI, arg); return; }
    if (!strcasecmp(cmd, "CTRL"))  {
        if (arg && !strncasecmp(arg, "ALT ", 4)) {
            /* CTRL ALT x */
            char *final_key = strtok(arg + 4, " ");
            int k = keycode(final_key ? final_key : "");
            if (k) { s_kbd.press(KEY_LEFT_CTRL); s_kbd.press(KEY_LEFT_ALT); s_kbd.press(k); delay(40); s_kbd.releaseAll(); }
            return;
        }
        run_modifier_combo(KEY_LEFT_CTRL, arg); return;
    }
    if (!strcasecmp(cmd, "ALT"))   { run_modifier_combo(KEY_LEFT_ALT,  arg); return; }
    if (!strcasecmp(cmd, "SHIFT")) { run_modifier_combo(KEY_LEFT_SHIFT, arg); return; }
}

static void run_payload(const char *script)
{
    ui_clear_body();
    auto &d = M5Cardputer.Display;
    d.setTextColor(T_BAD, T_BG);
    d.setCursor(4, BODY_Y + 2); d.print("RUNNING");
    d.drawFastHLine(4, BODY_Y + 12, 80, T_BAD);
    ui_draw_footer("`=abort");
    ui_draw_status("usb-hid", "run");

    hid_ensure();
    const char *p = script;
    int ln = 0;
    while (*p) {
        const char *eol = strchr(p, '\n');
        int len = eol ? (int)(eol - p) : (int)strlen(p);
        char line[160];
        int n = len < 159 ? len : 159;
        memcpy(line, p, n);
        line[n] = '\0';

        ln++;
        d.fillRect(0, BODY_Y + 22, SCR_W, 14, T_BG);
        d.setTextColor(T_FG, T_BG);
        d.setCursor(4, BODY_Y + 22);
        d.printf("%d: %.36s", ln, line);

        exec_line(line);

        /* Abort on ESC. */
        uint16_t k = input_poll();
        if (k == PK_ESC) {
            s_kbd.releaseAll();
            return;
        }

        if (!eol) break;
        p = eol + 1;
    }

    s_kbd.releaseAll();
    ui_toast("done", T_GOOD, 800);
}

static int pick_payload(void)
{
    ui_clear_body();
    auto &d = M5Cardputer.Display;
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2); d.print("BADUSB PAYLOADS");
    d.drawFastHLine(4, BODY_Y + 12, 130, T_ACCENT);
    d.setTextColor(T_FG, T_BG);
    for (size_t i = 0; i < PAY_N; ++i) {
        d.setCursor(4, BODY_Y + 22 + (int)i * 12);
        d.printf("[%d] %s", (int)(i + 1), s_payloads[i].name);
    }
    d.setTextColor(T_DIM, T_BG);
    d.setCursor(4, BODY_Y + 22 + (int)PAY_N * 12);
    d.print("[T] Type custom now");
    ui_draw_footer("1-5=run  T=type  `=back");

    while (true) {
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) return -1;
        if (k >= '1' && k < '1' + (int)PAY_N) return k - '1';
        if (k == 't' || k == 'T') return -2;
    }
}

/* ---------------------------------------------------------------------------
 * Live keyboard — real-time USB HID passthrough.
 *
 * Every Cardputer keypress is forwarded to the connected USB host the instant
 * it is pressed (no line buffering, unlike [T] Type custom). Built for driving
 * a phone whose touchscreen is dead: plug Cardputer -> phone over USB-C, then
 * type the PIN to unlock. USB HID input is accepted at the Android
 * before-first-unlock (BFU) lock screen even though adb is blocked there, so
 * this works right after a reboot. Printable keys, Enter, Backspace, Tab and
 * the arrow keys all pass through. Backtick / ESC is reserved to exit.
 * ------------------------------------------------------------------------- */
void feat_badusb_live(void)
{
    extern bool g_trident_cdc_active;
    if (g_mimir_cdc_active || g_trident_cdc_active) {
        ui_toast("CDC in use", T_WARN, 1000); return;
    }

    ui_clear_body();
    auto &d = M5Cardputer.Display;
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2); d.print("LIVE KEYBOARD");
    d.drawFastHLine(4, BODY_Y + 12, 130, T_ACCENT);
    d.setTextColor(T_FG, T_BG);
    d.setCursor(4, BODY_Y + 20); d.print("Cardputer keys -> host");
    d.setTextColor(T_DIM, T_BG);
    d.setCursor(4, BODY_Y + 32); d.print("Enter/Bksp/arrows pass");
    d.setCursor(4, BODY_Y + 44); d.print("through. `=exit");
    ui_draw_footer("`=exit live kbd");
    ui_draw_status("usb-hid", "live");

    hid_ensure();

    uint32_t sent = 0;
    for (;;) {
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(6); continue; }
        if (k == PK_ESC) break;                 /* reserved: exit live mode */

        const char *lbl = nullptr;
        char cbuf[2] = { 0, 0 };

        if (k >= 0x20 && k <= 0x7E) {           /* printable ASCII */
            s_kbd.write((uint8_t)k);
            cbuf[0] = (char)k; lbl = cbuf;
        }
        else if (k == PK_ENTER) { s_kbd.write(KEY_RETURN);      lbl = "ENTER"; }
        else if (k == PK_BKSP)  { s_kbd.write(KEY_BACKSPACE);   lbl = "BKSP";  }
        else if (k == PK_TAB)   { s_kbd.write(KEY_TAB);         lbl = "TAB";   }
        else if (k == PK_UP)    { s_kbd.write(KEY_UP_ARROW);    lbl = "UP";    }
        else if (k == PK_DOWN)  { s_kbd.write(KEY_DOWN_ARROW);  lbl = "DOWN";  }
        else if (k == PK_LEFT)  { s_kbd.write(KEY_LEFT_ARROW);  lbl = "LEFT";  }
        else if (k == PK_RIGHT) { s_kbd.write(KEY_RIGHT_ARROW); lbl = "RIGHT"; }
        else continue;                          /* ignore FN and unknowns */

        sent++;
        d.fillRect(0, BODY_Y + 56, SCR_W, 14, T_BG);
        d.setTextColor(T_GOOD, T_BG);
        d.setCursor(4, BODY_Y + 58);
        d.printf("sent %lu  last:%s", (unsigned long)sent, lbl);
    }

    s_kbd.releaseAll();
    ui_toast("live kbd off", T_DIM, 600);
}

void feat_badusb(void)
{
    extern bool g_trident_cdc_active;
    if (g_mimir_cdc_active || g_trident_cdc_active) {
        ui_toast("CDC in use", T_WARN, 1000); return;
    }
    while (true) {
        int pick = pick_payload();
        if (pick == -1) return;
        if (pick == -2) {
            char line[128];
            if (!input_line("type to send:", line, sizeof(line))) continue;
            hid_ensure();
            type_string(line);
            s_kbd.write(KEY_RETURN);
            ui_toast("sent", T_GOOD, 500);
            continue;
        }
        run_payload(s_payloads[pick].script);
    }
}

/* ============================================================================
 * Extended payload pickers — scrollable list rendering for the per-OS
 * UberGuidoZ-derived libraries (badusb_extras.h) and the prank highlight
 * reel (badusb_pranks_data.h). Each picker renders a scrollable list with
 * a header + per-item blurb so the user knows what they're firing.
 * ============================================================================ */

/* Shared CDC-busy guard. */
static bool badusb_cdc_busy(void)
{
    extern bool g_trident_cdc_active;
    if (g_mimir_cdc_active || g_trident_cdc_active) {
        ui_toast("CDC in use", T_WARN, 1000);
        return true;
    }
    return false;
}

/* Generic scrollable picker over an array of {name, blurb} entries.
 * `blurbs` may be nullptr if the list type doesn't carry blurbs (the
 * OS-payload arrays don't). Returns selected index, or -1 if ESC. */
static int pick_list_scrollable(const char *title,
                                const char *footer_hint,
                                int count,
                                const char *(*get_name)(int idx, void *ctx),
                                const char *(*get_blurb)(int idx, void *ctx),
                                void *ctx)
{
    int cursor = 0;
    int top = 0;
    const int rows = 6;        /* visible rows in the body */
    const int row_h = 11;
    auto &d = M5Cardputer.Display;
    int last_top = -1, last_cursor = -1;

    ui_clear_body();
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2); d.print(title);
    d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT);
    ui_draw_footer(footer_hint);

    while (true) {
        if (cursor < top)         top = cursor;
        if (cursor >= top + rows) top = cursor - rows + 1;

        if (top != last_top || cursor != last_cursor) {
            d.fillRect(0, BODY_Y + 16, SCR_W, rows * row_h + 2, T_BG);
            for (int i = 0; i < rows && (top + i) < count; ++i) {
                int idx = top + i;
                int y = BODY_Y + 18 + i * row_h;
                if (idx == cursor) {
                    d.fillRect(2, y - 1, SCR_W - 4, row_h, T_SEL_BG);
                    d.drawRect(2, y - 1, SCR_W - 4, row_h, T_SEL_BD);
                }
                d.setTextColor(idx == cursor ? T_FG : T_DIM, idx == cursor ? T_SEL_BG : T_BG);
                d.setCursor(6, y);
                char buf[34];
                snprintf(buf, sizeof(buf), "%-30s", get_name(idx, ctx));
                d.print(buf);
            }

            /* Blurb pane below the list. */
            if (get_blurb && cursor < count) {
                int by = BODY_Y + 18 + rows * row_h + 2;
                d.fillRect(0, by, SCR_W, 18, T_BG);
                d.setTextColor(T_DIM, T_BG);
                d.setCursor(4, by + 2);
                const char *b = get_blurb(cursor, ctx);
                if (b) {
                    char trim[42];
                    snprintf(trim, sizeof(trim), "%.40s", b);
                    d.print(trim);
                }
            }

            last_top = top;
            last_cursor = cursor;
        }

        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC)                       return -1;
        if (k == PK_ENTER || k == ' ')         return cursor;
        if (k == ';' || k == PK_UP)            { if (cursor > 0) cursor--; }
        else if (k == '.' || k == PK_DOWN)     { if (cursor + 1 < count) cursor++; }
    }
}

/* ---- per-OS payload picker (UberGuidoZ subset) ---- */
struct os_payload_ctx { const payload_t *arr; };
static const char *os_payload_name(int i, void *c) {
    return ((os_payload_ctx *)c)->arr[i].name;
}

static void run_os_payload_menu(const char *title,
                                const payload_t *arr,
                                size_t count)
{
    if (badusb_cdc_busy()) return;
    os_payload_ctx ctx = { arr };
    while (true) {
        int pick = pick_list_scrollable(title, ";/.=move  ENTER=run  `=back",
                                        (int)count, os_payload_name, nullptr, &ctx);
        if (pick < 0) return;
        run_payload(arr[pick].script);
    }
}

void feat_badusb_win(void)     { run_os_payload_menu("BADUSB / WINDOWS",   BADUSB_WIN_PAYLOADS,      BADUSB_WIN_N); }
void feat_badusb_mac(void)     { run_os_payload_menu("BADUSB / macOS",     BADUSB_MAC_PAYLOADS,      BADUSB_MAC_N); }
void feat_badusb_linux(void)   { run_os_payload_menu("BADUSB / LINUX",     BADUSB_LINUX_PAYLOADS,    BADUSB_LINUX_N); }
void feat_badusb_android(void) { run_os_payload_menu("BADUSB / ANDROID",   BADUSB_ANDROID_PAYLOADS,  BADUSB_ANDROID_N); }
void feat_badusb_chrome(void)  { run_os_payload_menu("BADUSB / CHROMEOS",  BADUSB_CHROMEOS_PAYLOADS, BADUSB_CHROMEOS_N); }

/* ---- pranks picker — two-level: category → prank ---- */
struct prank_pick_ctx { const prank_t *arr; };
static const char *prank_name(int i, void *c)  { return ((prank_pick_ctx *)c)->arr[i].name;  }
static const char *prank_blurb(int i, void *c) { return ((prank_pick_ctx *)c)->arr[i].blurb; }

struct prank_cat_ctx { /* unused — categories come from PRANK_CATEGORIES */ };
static const char *prank_cat_name (int i, void *)  { return PRANK_CATEGORIES[i].name; }
static const char *prank_cat_blurb(int i, void *)
{
    /* Render a tiny preview: "<N> entries" */
    static char buf[28];
    snprintf(buf, sizeof(buf), "%u entries", (unsigned)PRANK_CATEGORIES[i].count);
    return buf;
}

void feat_badusb_pranks(void)
{
    if (badusb_cdc_busy()) return;
    while (true) {
        int cat = pick_list_scrollable("BADUSB / PRANKS",
                                       ";/.=move  ENTER=open  `=back",
                                       (int)PRANK_CATEGORIES_N,
                                       prank_cat_name, prank_cat_blurb, nullptr);
        if (cat < 0) return;

        /* Inner picker over this category's pranks. */
        prank_pick_ctx ctx = { PRANK_CATEGORIES[cat].items };
        while (true) {
            char title[40];
            snprintf(title, sizeof(title), "PRANK / %s", PRANK_CATEGORIES[cat].name);
            int pick = pick_list_scrollable(title, ";/.=move  ENTER=fire  `=back",
                                            (int)PRANK_CATEGORIES[cat].count,
                                            prank_name, prank_blurb, &ctx);
            if (pick < 0) break;   /* back to category list */
            run_payload(PRANK_CATEGORIES[cat].items[pick].script);
        }
    }
}
