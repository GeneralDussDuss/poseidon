#pragma once
#include "kerb_crypto.h"

// mbedtls-backed implementation of the KERBEROS core crypto interface.
const kerb_crypto_t *kerb_mbedtls_crypto(void);
