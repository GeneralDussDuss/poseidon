#include "cred_store.h"
#include <string.h>

#define MEM_MAX 16
struct mem_store {
    cred_store  base;
    cred_record recs[MEM_MAX];
    int         n;
};
static mem_store g_mem;

static int mem_add(cred_store *s, const cred_record *r) {
    mem_store *m = (mem_store *)s;
    if (m->n >= MEM_MAX) return -1;
    m->recs[m->n++] = *r;
    return 0;
}
static int mem_find(cred_store *s, const uint8_t rp[32], cred_record *out, int index, int *total) {
    mem_store *m = (mem_store *)s;
    int count = 0, ret = -1;
    for (int i = 0; i < m->n; i++) {
        if (memcmp(m->recs[i].rpIdHash, rp, 32) == 0) {
            if (count == index) { *out = m->recs[i]; ret = 0; }
            count++;
        }
    }
    if (total) *total = count;
    return ret;
}
static int mem_update(cred_store *s, const uint8_t id[32], uint32_t nc) {
    mem_store *m = (mem_store *)s;
    for (int i = 0; i < m->n; i++)
        if (memcmp(m->recs[i].id, id, 32) == 0) { m->recs[i].signCount = nc; return 0; }
    return -1;
}
static void mem_wipe(cred_store *s) { ((mem_store *)s)->n = 0; }

cred_store *cred_store_mem(void) {
    g_mem.base.add            = mem_add;
    g_mem.base.find_by_rp     = mem_find;
    g_mem.base.update_counter = mem_update;
    g_mem.base.wipe           = mem_wipe;
    g_mem.n = 0;
    return &g_mem.base;
}
