/*
 * net_helpers — single source of truth for TCP probe + HTTP GET
 * patterns that previously lived duplicated across ~9 LAN features
 * (net_cctv, net_lanrecon, net_tools, net_ssdp, net_wpad, net_attacks,
 * saltyjack_responder, saltyjack_wpad, net_responder).
 *
 * These helpers are deliberately minimal and synchronous — they fit
 * the "one feature is running, nothing else is blocked" reality of
 * POSEIDON. Don't reach for these from an ISR or a background task.
 */
#pragma once

#include <Arduino.h>
#include <IPAddress.h>
#include <WiFiClient.h>

/*
 * Probe whether ip:port accepts a TCP connection. Timeout in ms.
 * Closes the socket immediately on success — nothing read, nothing
 * written. Safe for the naive "is this port open" scanner pattern.
 */
bool net_tcp_open(IPAddress ip, uint16_t port, uint32_t timeout_ms);

/*
 * Minimal HTTP/1.0 GET. Connects, sends the request with a fixed
 * User-Agent + Connection: close, parses the "HTTP/1.x NNN " status
 * line, then drains the remainder of the response into `out_body`
 * (bounded at 512 bytes — cameras and routers churn out big pages
 * we don't care about past a fingerprint).
 *
 * @param auth_header   Optional raw value for Authorization: (e.g.
 *                      "Basic <base64>"); pass nullptr / "" to skip.
 * @param out_body      Optional buffer; body is appended (up to 512 B).
 * @param out_headers   Optional buffer; raw header block (up to 512 B).
 * @return              HTTP status code on success (1xx-5xx),
 *                      0 if we never got a status line.
 */
int net_http_get(IPAddress ip, uint16_t port, const char *path,
                 const char *auth_header,
                 String *out_body, String *out_headers,
                 uint32_t timeout_ms);