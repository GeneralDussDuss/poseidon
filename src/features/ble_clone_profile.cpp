#include "ble_clone_profile.h"
#include <stdio.h>
#include <string.h>

// --- hex helpers -----------------------------------------------------------

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Parse a hex token into bytes. A lone "-" means an empty value. Returns the
// byte count, or -1 on odd length / invalid char / overflow.
static int parse_val_token(const char *tok, uint8_t *out, int max) {
    if (tok[0] == '-' && tok[1] == 0) return 0;
    int len = (int)strlen(tok);
    if (len % 2 != 0) return -1;
    int n = len / 2;
    if (n > max) return -1;
    for (int i = 0; i < n; ++i) {
        int hi = hex_nibble(tok[2 * i]);
        int lo = hex_nibble(tok[2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return n;
}

// --- serialize -------------------------------------------------------------

int clone_profile_serialize(const clone_profile_t *p, char *out, int out_sz) {
    int off = 0;
#define EMIT(...) do { \
        int _w = snprintf(out + off, out_sz - off, __VA_ARGS__); \
        if (_w < 0 || _w >= out_sz - off) return -1; \
        off += _w; \
    } while (0)

    EMIT("NAME %s\n", p->name);
    EMIT("MAC %02X%02X%02X%02X%02X%02X\n",
         p->mac[0], p->mac[1], p->mac[2], p->mac[3], p->mac[4], p->mac[5]);

    EMIT("ADV ");
    if (p->adv_len == 0) EMIT("-");
    else for (int i = 0; i < p->adv_len; ++i) EMIT("%02X", p->adv[i]);
    EMIT("\n");

    for (int e = 0; e < p->n_entries; ++e) {
        const clone_entry_t *c = &p->entries[e];
        if (c->type == CE_SERVICE) {
            EMIT("S %s\n", c->uuid);
        } else if (c->type == CE_CHAR) {
            EMIT("C %s %02X ", c->uuid, c->props);
            if (c->val_len == 0) EMIT("-");
            else for (int i = 0; i < c->val_len; ++i) EMIT("%02X", c->val[i]);
            EMIT("\n");
        } else { // CE_DESC
            EMIT("D %s ", c->uuid);
            if (c->val_len == 0) EMIT("-");
            else for (int i = 0; i < c->val_len; ++i) EMIT("%02X", c->val[i]);
            EMIT("\n");
        }
    }
#undef EMIT
    return off;
}

// --- deserialize -----------------------------------------------------------

bool clone_profile_deserialize(const char *text, clone_profile_t *out) {
    memset(out, 0, sizeof(*out));
    bool any = false;
    const char *p = text;
    char line[256];

    while (*p) {
        const char *nl = strchr(p, '\n');
        int len = nl ? (int)(nl - p) : (int)strlen(p);
        if (len >= (int)sizeof(line)) len = sizeof(line) - 1;
        memcpy(line, p, len);
        line[len] = 0;
        p = nl ? nl + 1 : p + strlen(p);
        while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == ' ')) line[--len] = 0;
        if (len == 0) continue;

        if (strncmp(line, "NAME ", 5) == 0) {
            snprintf(out->name, sizeof(out->name), "%s", line + 5);
            any = true;
        } else if (strncmp(line, "MAC ", 4) == 0) {
            if (parse_val_token(line + 4, out->mac, 6) != 6) return false;
            any = true;
        } else if (strncmp(line, "ADV ", 4) == 0) {
            int n = parse_val_token(line + 4, out->adv, CLONE_ADV_MAX);
            if (n < 0) return false;
            out->adv_len = (uint8_t)n;
            any = true;
        } else if (line[0] == 'S' && line[1] == ' ') {
            if (out->n_entries >= CLONE_MAX_ENTRIES) return false;
            clone_entry_t *e = &out->entries[out->n_entries++];
            e->type = CE_SERVICE;
            snprintf(e->uuid, sizeof(e->uuid), "%s", line + 2);
            any = true;
        } else if (line[0] == 'C' && line[1] == ' ') {
            if (out->n_entries >= CLONE_MAX_ENTRIES) return false;
            char uuid[64], valtok[96];
            unsigned props = 0;
            int m = sscanf(line + 2, "%63s %x %95s", uuid, &props, valtok);
            if (m < 2) return false;
            clone_entry_t *e = &out->entries[out->n_entries++];
            e->type  = CE_CHAR;
            snprintf(e->uuid, sizeof(e->uuid), "%s", uuid);
            e->props = (uint8_t)props;
            if (m == 3) {
                int n = parse_val_token(valtok, e->val, CLONE_MAX_VALUE);
                if (n < 0) return false;
                e->val_len = (uint8_t)n;
            }
            any = true;
        } else if (line[0] == 'D' && line[1] == ' ') {
            if (out->n_entries >= CLONE_MAX_ENTRIES) return false;
            char uuid[64], valtok[96];
            int m = sscanf(line + 2, "%63s %95s", uuid, valtok);
            if (m < 1) return false;
            clone_entry_t *e = &out->entries[out->n_entries++];
            e->type = CE_DESC;
            snprintf(e->uuid, sizeof(e->uuid), "%s", uuid);
            if (m == 2) {
                int n = parse_val_token(valtok, e->val, CLONE_MAX_VALUE);
                if (n < 0) return false;
                e->val_len = (uint8_t)n;
            }
            any = true;
        } else {
            return false;   // unrecognized line -> not a profile
        }
    }
    return any;
}
