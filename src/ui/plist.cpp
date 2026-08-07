#include "plist.h"

void plist_init(plist_t *m, int count, int rows) {
    m->count = count < 0 ? 0 : count;
    m->rows  = rows  < 1 ? 1 : rows;
    m->sel   = 0;
    m->top   = 0;
}

int plist_visible_count(const plist_t *m) {
    return m->count < m->rows ? m->count : m->rows;
}

bool plist_move(plist_t *m, int delta) {
    if (m->count <= 0) { return false; }

    int sel = m->sel + delta;
    bool wrapped = false;
    while (sel < 0)         { sel += m->count; wrapped = true; }
    while (sel >= m->count) { sel -= m->count; wrapped = true; }
    m->sel = sel;

    int top = m->top;
    if (m->count <= m->rows) {
        top = 0;
    } else if (sel < top) {
        top = sel;
    } else if (sel >= top + m->rows) {
        top = sel - m->rows + 1;
    }
    if (top > m->count - m->rows) { top = m->count - m->rows; }
    if (top < 0)                  { top = 0; }

    bool scrolled = (top != m->top) || wrapped;
    m->top = top;
    return scrolled;
}
