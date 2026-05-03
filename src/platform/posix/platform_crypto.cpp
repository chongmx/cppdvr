/**
 * @file platform_crypto_posix.cpp
 * @brief POSIX/Linux implementation of platform_crypto.h
 *
 * Self-contained MD5 per RFC 1321 — no external dependencies.
 */

#include "platform_crypto.h"
#include <cstring>
#include <cstdint>

// ============================================================================
// RFC 1321 MD5
// ============================================================================

static const uint32_t K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee,
    0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
    0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
    0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
    0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
    0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
    0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039,
    0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
    0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391
};

static const int S[64] = {
    7, 12, 17, 22,  7, 12, 17, 22,  7, 12, 17, 22,  7, 12, 17, 22,
    5,  9, 14, 20,  5,  9, 14, 20,  5,  9, 14, 20,  5,  9, 14, 20,
    4, 11, 16, 23,  4, 11, 16, 23,  4, 11, 16, 23,  4, 11, 16, 23,
    6, 10, 15, 21,  6, 10, 15, 21,  6, 10, 15, 21,  6, 10, 15, 21
};

struct MD5ContextInternal {
    uint32_t state[4];
    uint64_t byte_count;
    uint8_t  buf[64];
    int      buf_used;
};

static inline uint32_t rotl32(uint32_t x, int n) {
    return (x << n) | (x >> (32 - n));
}

static void md5_compress(uint32_t state[4], const uint8_t block[64]) {
    uint32_t M[16];
    for (int i = 0; i < 16; i++) {
        M[i] = (uint32_t)block[i*4 + 0]        |
               (uint32_t)block[i*4 + 1] <<  8  |
               (uint32_t)block[i*4 + 2] << 16  |
               (uint32_t)block[i*4 + 3] << 24;
    }

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];

    for (int i = 0; i < 64; i++) {
        uint32_t F; int g;
        if      (i < 16) { F = (b & c) | (~b & d); g = i; }
        else if (i < 32) { F = (d & b) | (~d & c); g = (5*i + 1) % 16; }
        else if (i < 48) { F = b ^ c ^ d;           g = (3*i + 5) % 16; }
        else             { F = c ^ (b | ~d);         g = (7*i)     % 16; }

        F += a + K[i] + M[g];
        a = d; d = c; c = b;
        b += rotl32(F, S[i]);
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
}

// ============================================================================
// PlatformMD5 API
// ============================================================================

int platform_md5_init(PlatformMD5Context* ctx) {
    if (!ctx) return -1;
    auto* s = new MD5ContextInternal;
    s->state[0] = 0x67452301u;
    s->state[1] = 0xefcdab89u;
    s->state[2] = 0x98badcfeu;
    s->state[3] = 0x10325476u;
    s->byte_count = 0;
    s->buf_used   = 0;
    *ctx = static_cast<PlatformMD5Context>(s);
    return 0;
}

int platform_md5_update(PlatformMD5Context ctx, const void* data, int len) {
    if (!ctx || !data || len < 0) return -1;
    auto* s = static_cast<MD5ContextInternal*>(ctx);
    const uint8_t* p = static_cast<const uint8_t*>(data);
    int remaining = len;

    while (remaining > 0) {
        int space = 64 - s->buf_used;
        int take  = remaining < space ? remaining : space;
        std::memcpy(s->buf + s->buf_used, p, take);
        s->buf_used   += take;
        s->byte_count += take;
        p             += take;
        remaining     -= take;

        if (s->buf_used == 64) {
            md5_compress(s->state, s->buf);
            s->buf_used = 0;
        }
    }
    return 0;
}

int platform_md5_final(PlatformMD5Context ctx, uint8_t* digest, int& digest_len) {
    if (!ctx || !digest) return -1;
    auto* s = static_cast<MD5ContextInternal*>(ctx);

    // Padding
    uint64_t bit_count = s->byte_count * 8;
    s->buf[s->buf_used++] = 0x80;
    if (s->buf_used > 56) {
        std::memset(s->buf + s->buf_used, 0, 64 - s->buf_used);
        md5_compress(s->state, s->buf);
        s->buf_used = 0;
    }
    std::memset(s->buf + s->buf_used, 0, 56 - s->buf_used);
    // Append bit length little-endian
    for (int i = 0; i < 8; i++)
        s->buf[56 + i] = static_cast<uint8_t>(bit_count >> (i * 8));
    md5_compress(s->state, s->buf);

    // Output little-endian state
    for (int i = 0; i < 4; i++) {
        digest[i*4 + 0] = static_cast<uint8_t>(s->state[i]);
        digest[i*4 + 1] = static_cast<uint8_t>(s->state[i] >>  8);
        digest[i*4 + 2] = static_cast<uint8_t>(s->state[i] >> 16);
        digest[i*4 + 3] = static_cast<uint8_t>(s->state[i] >> 24);
    }
    digest_len = 16;

    delete s;
    return 0;
}

int platform_md5(const void* data, int len, uint8_t* digest) {
    if (!data || !digest || len < 0) return -1;
    PlatformMD5Context ctx = nullptr;
    if (platform_md5_init(&ctx)              != 0) return -1;
    if (platform_md5_update(ctx, data, len)  != 0) return -1;
    int digest_len = 0;
    return platform_md5_final(ctx, digest, digest_len);
}
