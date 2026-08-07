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
/* Returns true when the caller must repaint the whole list: either the
 * window scrolled, or the selection wrapped around an end (which is a
 * large visual jump even when the window itself did not move). Returns
 * false when only the selection moved within the visible window, in
 * which case repainting just the old and new selected rows is enough. */
bool plist_move(plist_t *m, int delta);
int  plist_visible_count(const plist_t *m);
