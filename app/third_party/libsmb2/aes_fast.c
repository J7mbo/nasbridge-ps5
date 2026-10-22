/* NASBridge - AES-128 for libsmb2's signing and encryption, in place of its lib/aes.c.
 * Copyright (C) 2026 NASBridge contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * libsmb2 signs (AES-CMAC) and encrypts (AES-CCM) SMB 3 messages one 16-byte
 * block at a time through AES128_ECB_encrypt, the only AES call it makes.
 * Its own aes.c does that byte by byte and expands the key again for every
 * block: a NAS that requires signing held a copy to about 38 MB/s, and one
 * that requires encryption to about 19 MB/s. Here the block goes through the
 * CPU's AES instructions (AES-NI: the PS5's CPU has them), and the plain C
 * below is only for a computer without them (the tests on an ARM Mac).
 */

#include <stdint.h>
#include <string.h>

#if defined(__x86_64__)
#include <cpuid.h>
#include <wmmintrin.h>
#endif

void AES128_ECB_encrypt(uint8_t *input, const uint8_t *key, uint8_t *output);

/* ---- plain C (FIPS-197) ---- */

static const uint8_t kSbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16};

static uint8_t xtime(uint8_t x)
{
    return (uint8_t)((x << 1) ^ ((x >> 7) * 0x1b));
}

static void encrypt_plain(const uint8_t *input, const uint8_t *key, uint8_t *output)
{
    uint8_t rk[176], s[16], t[16];
    uint8_t rcon = 1;
    int i, round;
    memcpy(rk, key, 16);
    for (i = 16; i < 176; i += 4)
    {
        uint8_t w[4] = {rk[i - 4], rk[i - 3], rk[i - 2], rk[i - 1]};
        if (i % 16 == 0)
        {
            const uint8_t first = w[0];
            w[0] = (uint8_t)(kSbox[w[1]] ^ rcon);
            w[1] = kSbox[w[2]];
            w[2] = kSbox[w[3]];
            w[3] = kSbox[first];
            rcon = xtime(rcon);
        }
        rk[i] = rk[i - 16] ^ w[0];
        rk[i + 1] = rk[i - 15] ^ w[1];
        rk[i + 2] = rk[i - 14] ^ w[2];
        rk[i + 3] = rk[i - 13] ^ w[3];
    }
    for (i = 0; i < 16; ++i)
        s[i] = input[i] ^ rk[i];
    for (round = 1; round <= 10; ++round)
    {
        /* SubBytes and ShiftRows (the state is column by column) */
        for (i = 0; i < 16; ++i)
            t[i] = kSbox[s[(i + 4 * (i % 4)) % 16]];
        if (round < 10) /* MixColumns */
            for (i = 0; i < 16; i += 4)
            {
                const uint8_t a = t[i], b = t[i + 1], c = t[i + 2], d = t[i + 3];
                const uint8_t all = a ^ b ^ c ^ d;
                t[i] = a ^ all ^ xtime(a ^ b);
                t[i + 1] = b ^ all ^ xtime(b ^ c);
                t[i + 2] = c ^ all ^ xtime(c ^ d);
                t[i + 3] = d ^ all ^ xtime(d ^ a);
            }
        for (i = 0; i < 16; ++i)
            s[i] = t[i] ^ rk[16 * round + i];
    }
    memcpy(output, s, 16);
}

/* ---- AES-NI ---- */

#if defined(__x86_64__)

#define EXPAND(k, rcon)                                                                            \
    do                                                                                             \
    {                                                                                              \
        __m128i assist = _mm_shuffle_epi32(_mm_aeskeygenassist_si128(k, rcon), 0xff);              \
        k = _mm_xor_si128(k, _mm_slli_si128(k, 4));                                                \
        k = _mm_xor_si128(k, _mm_slli_si128(k, 4));                                                \
        k = _mm_xor_si128(k, _mm_slli_si128(k, 4));                                                \
        k = _mm_xor_si128(k, assist);                                                              \
    } while (0)

#define ROUND(rcon)                                                                                \
    do                                                                                             \
    {                                                                                              \
        EXPAND(k, rcon);                                                                           \
        s = _mm_aesenc_si128(s, k);                                                                \
    } while (0)

__attribute__((target("aes,sse2"))) static void encrypt_aesni(const uint8_t *input,
                                                              const uint8_t *key,
                                                              uint8_t *output)
{
    __m128i k = _mm_loadu_si128((const __m128i *)key);
    __m128i s = _mm_xor_si128(_mm_loadu_si128((const __m128i *)input), k);
    /* The round keys as they're needed: no table kept between blocks. */
    ROUND(0x01);
    ROUND(0x02);
    ROUND(0x04);
    ROUND(0x08);
    ROUND(0x10);
    ROUND(0x20);
    ROUND(0x40);
    ROUND(0x80);
    ROUND(0x1b);
    EXPAND(k, 0x36);
    s = _mm_aesenclast_si128(s, k);
    _mm_storeu_si128((__m128i *)output, s);
}

static int have_aesni(void)
{
    static int known = -1; /* the same answer on every thread: no lock needed */
    if (known < 0)
    {
        unsigned a, b, c, d;
        known = __get_cpuid(1, &a, &b, &c, &d) && (c & bit_AES) != 0;
    }
    return known;
}

#endif

void AES128_ECB_encrypt(uint8_t *input, const uint8_t *key, uint8_t *output)
{
#if defined(__x86_64__)
    if (have_aesni())
    {
        encrypt_aesni(input, key, output);
        return;
    }
#endif
    encrypt_plain(input, key, output);
}
