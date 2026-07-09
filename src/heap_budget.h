#pragma once
#include <stddef.h>

// Test seam: inject heap-query functions on host. On device the defaults
// (heap_caps_*) are installed by hb_install_esp_query() at boot.
void   hb_set_query(size_t (*free_fn)(void), size_t (*largest_fn)(void));
void   hb_test_reset(void);   // reset min-ever + registry (tests only)

size_t heap_free_internal(void);
size_t heap_largest_internal(void);
size_t heap_min_ever_internal(void);
void   heap_report(const char *tag);
