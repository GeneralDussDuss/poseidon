#pragma once

/*
 * version.h — single source of truth for the POSEIDON build tag.
 * POSEIDON_VERSION + POSEIDON_BUILD_DATE come from -D flags in
 * platformio.ini so a rebuild always reflects the current checkout.
 */

#ifndef POSEIDON_VERSION
#define POSEIDON_VERSION "0.0.0-dev"
#endif

/* __DATE__ can't be passed through a -D flag as a literal, so bake it
 * here at the file that gets (re)compiled via -DPOSEIDON_VERSION
 * trigger. */
#undef POSEIDON_BUILD_DATE
#define POSEIDON_BUILD_DATE __DATE__

inline const char *poseidon_version(void)    { return POSEIDON_VERSION; }
inline const char *poseidon_build_date(void) { return POSEIDON_BUILD_DATE; }
