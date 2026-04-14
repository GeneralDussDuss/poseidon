/*
 * dhcp_cache — shared (MAC → hostname) table.
 *
 * Populated by any feature that sees a DHCP DISCOVER/REQUEST:
 *   - wifi_portal.cpp (we're the DHCP server; plaintext)
 *   - wifi_clients / wifi_clients_all (open-network promiscuous)
 *   - anywhere we parse raw WiFi frames and hit option 12
 *
 * Consulted by client display code to upgrade MAC → real device name.
 */
#pragma once
#include <stdint.h>

/* Learn a (mac, hostname) pair. mac is 6 bytes in the order they
 * appear in 802.11 headers (display / big-endian). hostname is
 * null-terminated but may be truncated. Safe to call from an ISR or
 * WiFi callback — data is stored in a fixed-size array with simple
 * replacement. */
void dhcp_learn(const uint8_t mac[6], const char *hostname);

/* Look up a previously-seen hostname. Returns nullptr if not cached. */
const char *dhcp_hostname(const uint8_t mac[6]);

/* Try to decode a DHCP Option 12 hostname out of a raw 802.11 data
 * frame payload (the whole promisc pkt.payload + sig_len). If the frame
 * is a DHCP DISCOVER/REQUEST, learns the hostname and returns true.
 *
 * This is for use inside promisc callbacks — it's cheap: bails as soon
 * as the frame fails a check. Safe on encrypted networks (encrypted
 * payload will fail the LLC/SNAP sanity check and return fast). */
bool dhcp_try_parse_802_11(const uint8_t *pkt, int len);

/* Stats. */
int  dhcp_cache_count(void);
void dhcp_cache_clear(void);
