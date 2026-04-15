#pragma once

/*
 * version.h — single source of truth for the POSEIDON build tag.
 * POSEIDON_VERSION + POSEIDON_BUILD_DATE come from -D flags in
 * platformio.ini so a rebuild always reflects the current checkout.
 */

#ifndef POSEIDON_VERSION
#define POSEIDON_VERSION "0.0.0-dev"
#endif

#ifndef POSEIDON_BUILD_DATE
#define POSEIDON_BUILD_DATE "unknown"
#endif

inline const char *poseidon_version(void)    { return POSEIDON_VERSION; }
inline const char *poseidon_build_date(void) { return POSEIDON_BUILD_DATE; }
