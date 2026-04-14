/*
 * proto.c — pack/unpack + send wrappers for the POSEIDON wire proto.
 */
#include "proto.h"
#include <string.h>
#include "esp_now.h"

static const uint8_t BROADCAST[6] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };

void proto_init_msg(posei_msg_t *m, uint8_t type)
{
    memset(m, 0, sizeof(*m));
    m->magic   = POSEI_MAGIC;
    m->version = POSEI_VERSION;
    m->type    = type;
}

void proto_send_broadcast(const posei_msg_t *m)
{
    esp_now_send(BROADCAST, (const uint8_t *)m, sizeof(*m));
}

void proto_send_to(const uint8_t mac[6], const posei_msg_t *m)
{
    esp_now_send(mac, (const uint8_t *)m, sizeof(*m));
}

int proto_validate(const uint8_t *data, int len, posei_msg_t *out)
{
    if (len < (int)sizeof(posei_msg_t)) return 0;
    memcpy(out, data, sizeof(*out));
    if (out->magic != POSEI_MAGIC) return 0;
    if (out->version != POSEI_VERSION) return 0;
    return 1;
}
