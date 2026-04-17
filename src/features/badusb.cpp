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
#include <USB.h>
#include <USBHIDKeyboard.h>
#include <SD.h>

static USBHIDKeyboard s_kbd;
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
