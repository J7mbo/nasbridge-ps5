/* Minimal SHA-256 (FIPS 180-4), used by the installer to verify every file. */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
  uint32_t state[8];
  uint64_t length;      /* total bytes hashed */
  uint8_t  block[64];
  size_t   used;        /* bytes waiting in `block` */
} sha256_ctx;

void sha256_init(sha256_ctx *ctx);
void sha256_update(sha256_ctx *ctx, const void *data, size_t len);
void sha256_final(sha256_ctx *ctx, uint8_t digest[32]);
