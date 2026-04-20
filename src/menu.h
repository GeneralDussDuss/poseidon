/*
 * menu — hierarchical menu with letter mnemonics.
 *
 * Each menu item has a single-letter hotkey and either a submenu or
 * an action. Press the letter = jump to that item. This is the whole
 * point of POSEIDON's UX: no scrolling through flat lists, no searching
 * to find things. You know W = WiFi, W→S = WiFi Scan. Two keystrokes.
 *
 * Items are declared with MENU_* macros in menu.cpp; the tree is
 * compile-time static.
 */
#pragma once

#include "app.h"

struct menu_node_t;

typedef void (*menu_action_fn)(void);

struct menu_node_t {
    char        hotkey;          /* single printable char, lowercase */
    const char *label;           /* human-readable name */
    const char *hint;            /* short description shown in footer */
    const menu_node_t *children; /* array of children (terminated by {0}) */
    menu_action_fn     action;   /* if non-null, leaf node fires this */
    const char *info;            /* long-form info shown on ?-key press */
};

/* Enter the main menu loop. Returns when user quits (rare). */
void menu_run(void);

/* Push a named menu as an overlay (used by back-from-feature returns).
 * Not strictly needed in the MVP — menu_run's own stack handles it. */
extern const menu_node_t MENU_ROOT;

/* Set by the menu runtime right before it invokes a feature's action.
 * Features can bind '?' to ui_show_current_help() to render their own
 * long-form info paragraph on demand instead of making the user ESC out
 * to the menu and press '=' there. */
extern const menu_node_t *g_current_feature_item;

/* Render the long-form help for the currently-running feature and wait
 * for any key press. Safe to call even if g_current_feature_item is
 * null — shows a generic "no help available" panel. */
void ui_show_current_help(void);
