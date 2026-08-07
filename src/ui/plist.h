/*
 * plist - selection and scroll maths for a windowed list.
 *
 * Pure: no Arduino, no M5, no display. plist_move returns whether the
 * window scrolled, which is what lets a caller repaint only the two rows
 * that changed instead of the whole panel.
 */
#pragma once

struct plist_t {
    int count;   /* total items */
    int rows;    /* visible rows */
    int sel;     /* selected index, 0..count-1 */
    int top;     /* index of first visible row */
};

void plist_init(plist_t *m, int count, int rows);
/* Returns true if `top` changed (repaint everything), false if only `sel`
 * changed (repaint the old and new selected rows only). */
bool plist_move(plist_t *m, int delta);
int  plist_visible_count(const plist_t *m);
