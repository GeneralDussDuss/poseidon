#pragma once
#include "kerb_crypto.h"
#define KW_HANDLE_LEN 60   // iv(12) + tag(16) + ct(32)
int kw_wrap(const kerb_crypto_t *cy, const uint8_t devkey[32], const uint8_t priv[32],
            const uint8_t appid[32], uint8_t *handle, size_t *handle_len);
int kw_unwrap(const kerb_crypto_t *cy, const uint8_t devkey[32], const uint8_t *handle,
              size_t handle_len, const uint8_t appid[32], uint8_t priv[32]);
