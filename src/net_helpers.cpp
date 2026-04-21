/*
 * net_helpers.cpp — see header for scope + rationale.
 */
#include "net_helpers.h"

bool net_tcp_open(IPAddress ip, uint16_t port, uint32_t timeout_ms)
{
    WiFiClient c;
    /* connect() takes its own timeout in ms; nothing else about the
     * socket matters since we close immediately on success. Avoid
     * stacking a setTimeout — that's in SECONDS and only applies to
     * stream reads, so combining the two just makes teardown slower. */
    if (!c.connect(ip, port, timeout_ms)) return false;
    c.stop();
    return true;
}

int net_http_get(IPAddress ip, uint16_t port, const char *path,
                 const char *auth_header,
                 String *out_body, String *out_headers,
                 uint32_t timeout_ms)
{
    if (out_body)    *out_body    = "";
    if (out_headers) *out_headers = "";

    WiFiClient c;
    if (!c.connect(ip, port, timeout_ms)) return 0;

    c.printf("GET %s HTTP/1.0\r\n", (path && *path) ? path : "/");
    c.printf("Host: %s\r\n", ip.toString().c_str());
    c.print("User-Agent: Mozilla/5.0 (poseidon)\r\n");
    c.print("Accept: */*\r\n");
    c.print("Connection: close\r\n");
    if (auth_header && *auth_header) {
        c.printf("Authorization: %s\r\n", auth_header);
    }
    c.print("\r\n");

    uint32_t deadline = millis() + timeout_ms;
    int code = 0;
    bool in_body = false;
    /* HTTP/1.0 servers half-close as soon as the response is flushed,
     * so c.connected() can go false with bytes still buffered. Keep
     * draining as long as there's data OR the socket is live. */
    while ((c.connected() || c.available()) && millis() < deadline) {
        if (!c.available()) { delay(5); continue; }
        String line = c.readStringUntil('\n');
        if (!in_body) {
            if (line.startsWith("HTTP/")) {
                int sp = line.indexOf(' ');
                if (sp > 0) code = line.substring(sp + 1, sp + 4).toInt();
            }
            if (line.length() <= 1) {   /* end of headers (blank / CRLF) */
                in_body = true;
                continue;
            }
            if (out_headers && out_headers->length() < 512) *out_headers += line;
        } else if (out_body && out_body->length() < 512) {
            *out_body += line;
        }
        if (out_body && out_body->length() >= 512) break;
    }
    c.stop();
    return code;
}