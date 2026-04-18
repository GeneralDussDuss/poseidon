/*
 * SaltyJack — LAN attack suite for POSEIDON.
 *
 * Heavy homage to @7h30th3r0n3 and the Evil-M5Project.
 * https://github.com/7h30th3r0n3/Evil-M5Project
 * https://github.com/7h30th3r0n3/Raspyjack
 *
 * Every attack module in this directory is a port / derivative of work
 * originally written by 7h30th3r0n3 in the Evil-Cardputer firmware
 * (RaspyJack-adjacent LAN pentest arsenal). POSEIDON reorganizes the
 * code into its own style + menu, fixes the coupling to M5's menu
 * drawing, and wires it all under the SaltyJack submenu.
 *
 * Go say hi to him on Discord if you ship anything on top of this.
 *
 * Attacks bundled:
 *   - DHCP Starvation       — exhaust the DHCP pool with random-MAC Discovers
 *   - Rogue DHCP (STA mode) — race the real server with our own Offer/Ack
 *   - Rogue DHCP (AP mode)  — we ARE the DHCP server for clients on our SSID
 *   - Responder             — LLMNR + NBT-NS + SMB1 with NTLMv2 type-2 builder
 *   - WPAD NTLM harvest     — PAC server + 407 Proxy-Authenticate
 *   - NTLMv2 cracker        — HMAC-MD5 wordlist, runs on-device
 *
 * All attacks operate on whichever L2 lwIP is attached to — WiFi STA for
 * now, W5500 wired once the SPI Ethernet hat is wired up.
 */
#pragma once

#include <Arduino.h>

/* ===== attack feature entry points ===== */

void feat_saltyjack_info(void);         /* homage + help landing page */
void feat_saltyjack_dhcp_starve(void);  /* DHCP Starvation */

/* coming in later commits:
 *   void feat_saltyjack_rogue_dhcp_sta(void);
 *   void feat_saltyjack_rogue_dhcp_ap(void);
 *   void feat_saltyjack_responder(void);
 *   void feat_saltyjack_wpad(void);
 *   void feat_saltyjack_ntlm_crack(void);
 */
