/*
 * tools — handy utilities in the Flipper tradition.
 *
 *   SD format   : wipe + re-FAT the microSD card
 *   Flashlight  : full-screen white panic torch
 *   Screen test : RGB bars + color grid
 *   Stopwatch   : count up with start/stop/lap
 *   Dice        : 4d6 roller, coin flip, magic 8-ball
 *   Morse       : type text, blink + beep in morse
 *   MAC rand    : randomize the WiFi MAC (survives one session)
 *   Calc        : tiny RPN calculator
 */
#include "app.h"
#include "../theme.h"
#include "ui.h"
#include "input.h"
#include "radio.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <SD.h>
#include "../sd_helper.h"
#include <FS.h>
#include <esp_random.h>
#include <functional>

/* ================= SD format ================= */

void feat_tool_sd_format(void)
{
    ui_clear_body();
    auto &d = M5Cardputer.Display;
    d.setTextColor(T_BAD, T_BG);
    d.setCursor(4, BODY_Y + 2); d.print("SD FORMAT");
    d.drawFastHLine(4, BODY_Y + 12, 80, T_BAD);
    d.setTextColor(T_WARN, T_BG);
    d.setCursor(4, BODY_Y + 22); d.print("THIS WIPES EVERYTHING");
    d.setTextColor(T_FG, T_BG);
    d.setCursor(4, BODY_Y + 38); d.print("type YES and enter");
    ui_draw_footer("ENTER=confirm  `=cancel");
    char buf[8];
    if (!input_line("confirm:", buf, sizeof(buf))) return;
    if (strcmp(buf, "YES") != 0) { ui_toast("cancelled", T_DIM, 500); return; }

    ui_toast("formatting...", T_WARN, 0);
    if (!sd_mount()) { ui_toast("SD mount fail", T_BAD, 1200); return; }
    /* Walk + delete every file + dir. No FAT format API on Arduino SD,
     * so deep-delete is the best we can do. */
    std::function<bool(const char *)> nuke = [&](const char *path) -> bool {
        File f = SD.open(path);
        if (!f) return false;
        if (f.isDirectory()) {
            File c = f.openNextFile();
            while (c) {
                char sub[192];
                snprintf(sub, sizeof(sub), "%s/%s", path, c.name());
                nuke(sub);
                c = f.openNextFile();
            }
            f.close();
            if (strcmp(path, "/") != 0) SD.rmdir(path);
        } else {
            f.close();
            SD.remove(path);
        }
        return true;
    };
    nuke("/");
    ui_toast("done", T_GOOD, 800);
}

/* ================= Flashlight ================= */

void feat_tool_flashlight(void)
{
    auto &d = M5Cardputer.Display;
    d.fillScreen(0xFFFF);
    d.setTextColor(0x0000, 0xFFFF);
    d.setCursor(60, SCR_H - 10); d.print("any key to exit");
    while (true) {
        uint16_t k = input_poll();
        if (k != PK_NONE) break;
        delay(50);
    }
}

/* ================= Screen test ================= */

void feat_tool_screen_test(void)
{
    auto &d = M5Cardputer.Display;
    const uint16_t cols[] = { 0xF800, 0x07E0, 0x001F, 0xFFFF, 0x0000, T_ACCENT, T_WARN };
    int idx = 0;
    while (true) {
        d.fillScreen(cols[idx]);
        d.setTextColor(cols[idx] == 0xFFFF ? 0x0000 : 0xFFFF, cols[idx]);
        d.setCursor(4, 4);
        d.printf("color %d/%d  any=next `=back", idx + 1, (int)(sizeof(cols) / sizeof(cols[0])));
        /* Gradient band. */
        for (int x = 0; x < SCR_W; ++x) {
            uint8_t t = (uint8_t)(x * 31 / SCR_W);
            d.drawFastVLine(x, SCR_H - 20, 20, (t << 11));
        }
        uint16_t k = PK_NONE;
        while (k == PK_NONE) { k = input_poll(); delay(30); }
        if (k == PK_ESC) return;
        idx = (idx + 1) % (int)(sizeof(cols) / sizeof(cols[0]));
    }
}

/* ================= Stopwatch ================= */

void feat_tool_stopwatch(void)
{
    ui_clear_body();
    ui_draw_footer("SPACE=start/stop L=lap R=reset `=back");
    auto &d = M5Cardputer.Display;

    uint32_t start_ms = 0;
    uint32_t elapsed  = 0;
    bool running = false;
    char laps[4][16] = {{0}};
    int  lap_n = 0;

    while (true) {
        uint32_t now = millis();
        uint32_t shown = running ? (elapsed + (now - start_ms)) : elapsed;

        /* Redraw each tick. */
        ui_clear_body();
        d.setTextColor(T_ACCENT, T_BG);
        d.setCursor(4, BODY_Y + 2); d.print("STOPWATCH");
        d.drawFastHLine(4, BODY_Y + 12, 80, T_ACCENT);

        char buf[16];
        uint32_t s  = shown / 1000;
        uint32_t ms = (shown % 1000) / 10;
        snprintf(buf, sizeof(buf), "%02lu:%02lu.%02lu",
                 (unsigned long)(s / 60), (unsigned long)(s % 60),
                 (unsigned long)ms);
        d.setTextColor(running ? T_GOOD : T_FG, T_BG);
        d.setTextSize(3);
        int w = d.textWidth(buf);
        d.setCursor((SCR_W - w) / 2, BODY_Y + 24);
        d.print(buf);
        d.setTextSize(1);

        d.setTextColor(T_DIM, T_BG);
        for (int i = 0; i < lap_n; ++i) {
            d.setCursor(4, BODY_Y + 60 + i * 10);
            d.printf("L%d  %s", i + 1, laps[i]);
        }

        uint16_t k = input_poll();
        if (k == PK_ESC) return;
        if (k == PK_SPACE || k == PK_ENTER) {
            if (running) { elapsed += now - start_ms; running = false; }
            else         { start_ms = now;            running = true;  }
        } else if (k == 'l' || k == 'L') {
            if (lap_n < 4) { strncpy(laps[lap_n++], buf, 15); }
        } else if (k == 'r' || k == 'R') {
            elapsed = 0; running = false; lap_n = 0;
        }
        delay(30);
    }
}

/* ================= Dice / 8-ball / coin ================= */

static const char *s_8ball[] = {
    "yes", "absolutely", "no", "doubtful", "ask again later",
    "signs point to yes", "very doubtful", "without a doubt",
    "my reply is no", "as i see it yes", "cannot predict",
    "outlook good", "outlook not so good"
};

void feat_tool_chance(void)
{
    auto &d = M5Cardputer.Display;
    int mode = 0;  /* 0=dice 1=coin 2=8ball */
    char last[48] = "roll...";

    while (true) {
        ui_clear_body();
        d.setTextColor(T_ACCENT, T_BG);
        d.setCursor(4, BODY_Y + 2);
        d.print(mode == 0 ? "DICE (2d6)" : mode == 1 ? "COIN" : "8-BALL");
        d.drawFastHLine(4, BODY_Y + 12, 80, T_ACCENT);

        d.setTextColor(T_WARN, T_BG);
        d.setTextSize(2);
        int w = d.textWidth(last) * 2;
        d.setCursor((SCR_W - w) / 2, BODY_Y + 30);
        d.print(last);
        d.setTextSize(1);

        d.setTextColor(T_DIM, T_BG);
        d.setCursor(4, BODY_Y + 70); d.print("SPACE=roll  M=mode");
        ui_draw_footer("SPACE=roll M=mode `=back");

        uint16_t k = PK_NONE;
        while (k == PK_NONE) { k = input_poll(); delay(20); }
        if (k == PK_ESC) return;
        if (k == 'm' || k == 'M') { mode = (mode + 1) % 3; continue; }
        if (k == PK_SPACE || k == PK_ENTER) {
            if (mode == 0) {
                int a = 1 + (esp_random() % 6);
                int b = 1 + (esp_random() % 6);
                snprintf(last, sizeof(last), "%d + %d = %d", a, b, a + b);
            } else if (mode == 1) {
                snprintf(last, sizeof(last), "%s", (esp_random() & 1) ? "HEADS" : "TAILS");
            } else {
                int n = sizeof(s_8ball) / sizeof(s_8ball[0]);
                snprintf(last, sizeof(last), "%s", s_8ball[esp_random() % n]);
            }
            M5Cardputer.Speaker.tone(1500, 60);
        }
    }
}

/* ================= Morse code sender ================= */

static const char *s_morse[] = {
    /* A-Z */
    ".-","-...","-.-.","-..",".","..-.","--.","....","..",".---","-.-",".-..",
    "--","-.","---",".--.","--.-",".-.","...","-","..-","...-",".--","-..-","-.--","--..",
    /* 0-9 */
    "-----",".----","..---","...--","....-",".....","-....","--...","---..","----."
};

static void morse_send(const char *s)
{
    const int unit = 100;  /* ms per dot */
    auto &d = M5Cardputer.Display;
    for (const char *p = s; *p; ++p) {
        char c = toupper(*p);
        const char *m = nullptr;
        if (c >= 'A' && c <= 'Z') m = s_morse[c - 'A'];
        else if (c >= '0' && c <= '9') m = s_morse[26 + c - '0'];
        if (!m) { delay(unit * 7); continue; }
        for (const char *x = m; *x; ++x) {
            d.fillScreen(T_ACCENT);
            M5Cardputer.Speaker.tone(800, (*x == '-') ? unit * 3 : unit);
            delay((*x == '-') ? unit * 3 : unit);
            d.fillScreen(T_BG);
            delay(unit);
        }
        delay(unit * 3);
    }
}

void feat_tool_morse(void)
{
    char msg[64];
    if (!input_line("text:", msg, sizeof(msg))) return;
    if (!msg[0]) return;
    morse_send(msg);
    ui_toast("sent", T_GOOD, 500);
}

/* ================= MAC randomizer ================= */

void feat_tool_mac_rand(void)
{
    uint8_t mac[6];
    for (int i = 0; i < 6; ++i) mac[i] = (uint8_t)esp_random();
    mac[0] &= 0xFE;  /* unicast */
    mac[0] |= 0x02;  /* locally administered */

    WiFi.mode(WIFI_STA);
    esp_wifi_set_mac(WIFI_IF_STA, mac);

    ui_clear_body();
    auto &d = M5Cardputer.Display;
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2); d.print("MAC RANDOMIZED");
    d.drawFastHLine(4, BODY_Y + 12, 130, T_ACCENT);
    d.setTextColor(T_FG, T_BG);
    d.setCursor(4, BODY_Y + 26); d.printf("%02X:%02X:%02X:%02X:%02X:%02X",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    d.setTextColor(T_DIM, T_BG);
    d.setCursor(4, BODY_Y + 46); d.print("resets on reboot");
    ui_draw_footer("`=back");
    while (true) {
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(40); continue; }
        if (k == PK_ESC) break;
    }
}

/* ================= Tiny calculator ================= */

void feat_tool_calc(void)
{
    /* Supports +, -, *, / on a single expression. */
    char expr[32];
    if (!input_line("expr (e.g. 12*7+3):", expr, sizeof(expr))) return;

    /* Simplest evaluator: left-to-right, no precedence. */
    double acc = 0;
    char op = '+';
    const char *p = expr;
    while (*p) {
        while (*p == ' ') ++p;
        if (!*p) break;
        double num = 0;
        bool have = false;
        while (*p >= '0' && *p <= '9') { num = num * 10 + (*p - '0'); ++p; have = true; }
        if (*p == '.') { ++p; double s = 0.1; while (*p >= '0' && *p <= '9') { num += (*p - '0') * s; s *= 0.1; ++p; have = true; } }
        if (!have) break;
        switch (op) {
        case '+': acc += num; break;
        case '-': acc -= num; break;
        case '*': acc *= num; break;
        case '/': if (num != 0) acc /= num; break;
        }
        while (*p == ' ') ++p;
        if (!*p) break;
        op = *p++;
    }

    ui_clear_body();
    auto &d = M5Cardputer.Display;
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2); d.print("CALC");
    d.drawFastHLine(4, BODY_Y + 12, 50, T_ACCENT);
    d.setTextColor(T_FG, T_BG);
    d.setCursor(4, BODY_Y + 22); d.printf("%s", expr);
    d.setTextColor(T_GOOD, T_BG);
    d.setTextSize(2);
    char out[24];
    if (acc == (long)acc) snprintf(out, sizeof(out), "= %ld", (long)acc);
    else                  snprintf(out, sizeof(out), "= %.4f", acc);
    d.setCursor(4, BODY_Y + 40); d.print(out);
    d.setTextSize(1);
    ui_draw_footer("`=back");
    while (true) {
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(40); continue; }
        if (k == PK_ESC) break;
    }
}
