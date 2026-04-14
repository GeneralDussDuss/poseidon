/*
 * dhcp_cache.cpp — hostname cache + DHCP option 12 parser.
 */
#include "dhcp_cache.h"
#include <string.h>
#include <Arduino.h>

#define DHCP_CACHE_MAX 24
#define HOSTNAME_MAX   24

struct entry_t { uint8_t mac[6]; char name[HOSTNAME_MAX]; uint32_t last_seen; };
static entry_t s_cache[DHCP_CACHE_MAX];
static int     s_n = 0;

static int find(const uint8_t *mac)
{
    for (int i = 0; i < s_n; ++i)
        if (memcmp(s_cache[i].mac, mac, 6) == 0) return i;
    return -1;
}

void dhcp_learn(const uint8_t mac[6], const char *hostname)
{
    if (!hostname || !hostname[0]) return;
    int idx = find(mac);
    if (idx < 0) {
        if (s_n >= DHCP_CACHE_MAX) {
            /* Evict oldest. */
            int oldest = 0;
            for (int i = 1; i < s_n; ++i)
                if (s_cache[i].last_seen < s_cache[oldest].last_seen) oldest = i;
            idx = oldest;
        } else {
            idx = s_n++;
        }
        memcpy(s_cache[idx].mac, mac, 6);
    }
    strncpy(s_cache[idx].name, hostname, HOSTNAME_MAX - 1);
    s_cache[idx].name[HOSTNAME_MAX - 1] = '\0';
    s_cache[idx].last_seen = millis();
}

const char *dhcp_hostname(const uint8_t mac[6])
{
    int i = find(mac);
    if (i < 0) return nullptr;
    return s_cache[i].name;
}

int  dhcp_cache_count(void) { return s_n; }
void dhcp_cache_clear(void) { s_n = 0; }

/* ---- raw 802.11 frame walker ---- */

bool dhcp_try_parse_802_11(const uint8_t *pkt, int len)
{
    if (len < 60) return false;
    uint8_t fc = pkt[0];
    uint8_t type = (fc >> 2) & 0x3;
    if (type != 2) return false;  /* must be data */

    uint8_t subtype = (fc >> 4) & 0xF;
    int hdr = 24;
    if (subtype & 0x8) hdr += 2;  /* QoS adds 2 bytes */

    if (len < hdr + 8) return false;
    /* Must be LLC+SNAP for IPv4 (08 00). */
    const uint8_t *llc = pkt + hdr;
    if (!(llc[0] == 0xAA && llc[1] == 0xAA && llc[2] == 0x03 &&
          llc[6] == 0x08 && llc[7] == 0x00)) return false;

    const uint8_t *ip = llc + 8;
    int ip_len = len - (ip - pkt);
    if (ip_len < 20) return false;

    /* IPv4 header sanity. */
    if ((ip[0] >> 4) != 4) return false;
    int ihl = (ip[0] & 0xF) * 4;
    if (ihl < 20 || ip_len < ihl + 8) return false;
    if (ip[9] != 17) return false;  /* not UDP */

    const uint8_t *udp = ip + ihl;
    uint16_t src_port = ((uint16_t)udp[0] << 8) | udp[1];
    uint16_t dst_port = ((uint16_t)udp[2] << 8) | udp[3];
    /* DHCP client → server on dst 67, or server → client on 68. */
    if (!(dst_port == 67 || src_port == 68 || dst_port == 68 || src_port == 67)) return false;

    const uint8_t *bootp = udp + 8;
    int bootp_len = ip_len - ihl - 8;
    if (bootp_len < 240) return false;  /* need fixed header + magic */

    /* Magic cookie at offset 236. */
    if (!(bootp[236] == 0x63 && bootp[237] == 0x82 &&
          bootp[238] == 0x53 && bootp[239] == 0x63)) return false;

    /* Client MAC is chaddr at offset 28 (16 bytes, first 6 are MAC). */
    uint8_t chaddr[6];
    memcpy(chaddr, bootp + 28, 6);

    /* Walk options starting at offset 240 for option 12 (host name). */
    int opt = 240;
    while (opt < bootp_len) {
        uint8_t code = bootp[opt];
        if (code == 0xFF) break;          /* end */
        if (code == 0x00) { opt++; continue; }  /* pad */
        if (opt + 1 >= bootp_len) break;
        uint8_t olen = bootp[opt + 1];
        if (opt + 2 + olen > bootp_len) break;

        if (code == 12 /* host name */) {
            char host[HOSTNAME_MAX];
            int copy = olen < HOSTNAME_MAX - 1 ? olen : HOSTNAME_MAX - 1;
            /* Sanitize: replace non-printable with '?'. */
            for (int i = 0; i < copy; ++i) {
                uint8_t c = bootp[opt + 2 + i];
                host[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
            }
            host[copy] = '\0';
            dhcp_learn(chaddr, host);
            return true;
        }
        opt += 2 + olen;
    }
    return false;
}
