/**
 * @file platform_crypto_windows.cpp
 * @brief Windows implementation of platform_crypto.h
 * 
 * Uses Windows CryptoAPI for MD5 hashing.
 */

#include "platform_crypto.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>

#pragma comment(lib, "crypt32.lib")

/**
 * @brief Internal MD5 context structure wrapping Windows CryptoAPI handles
 */
struct MD5ContextInternal {
    HCRYPTPROV hProv;
    HCRYPTHASH hHash;
};

// ============================================================================
// MD5 Context Management
// ============================================================================

int platform_md5_init(PlatformMD5Context* ctx) {
    if (!ctx) {
        return -1;
    }

    MD5ContextInternal* internal = new MD5ContextInternal;
    if (!internal) {
        return -1;
    }

    internal->hProv = 0;
    internal->hHash = 0;

    if (!CryptAcquireContextW(&internal->hProv, nullptr, nullptr,
                              PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        delete internal;
        return -1;
    }

    if (!CryptCreateHash(internal->hProv, CALG_MD5, 0, 0, &internal->hHash)) {
        CryptReleaseContext(internal->hProv, 0);
        delete internal;
        return -1;
    }

    *ctx = static_cast<PlatformMD5Context>(internal);
    return 0;
}

int platform_md5_update(PlatformMD5Context ctx, const void* data, int len) {
    if (!ctx || !data || len < 0) {
        return -1;
    }

    MD5ContextInternal* internal = static_cast<MD5ContextInternal*>(ctx);
    if (!internal || !internal->hHash) {
        return -1;
    }

    // CryptHashData fails with cbData=0; skip it — the hash state is already
    // the MD5 of empty input, which is the correct result for an empty password.
    if (len == 0) return 0;

    if (!CryptHashData(internal->hHash, (const BYTE*)data, static_cast<DWORD>(len), 0)) {
        return -1;
    }

    return 0;
}

int platform_md5_final(PlatformMD5Context ctx, uint8_t* digest, int& digest_len) {
    if (!ctx || !digest) {
        return -1;
    }

    MD5ContextInternal* internal = static_cast<MD5ContextInternal*>(ctx);
    if (!internal || !internal->hHash) {
        return -1;
    }

    DWORD actual_digest_len = PLATFORM_MD5_DIGEST_SIZE;
    if (!CryptGetHashParam(internal->hHash, HP_HASHVAL, digest, &actual_digest_len, 0)) {
        CryptDestroyHash(internal->hHash);
        CryptReleaseContext(internal->hProv, 0);
        delete internal;
        return -1;
    }

    CryptDestroyHash(internal->hHash);
    CryptReleaseContext(internal->hProv, 0);
    delete internal;

    digest_len = static_cast<int>(actual_digest_len);
    return 0;
}

// ============================================================================
// One-Shot MD5
// ============================================================================

int platform_md5(const void* data, int len, uint8_t* digest) {
    if (!data || !digest || len < 0) {
        return -1;
    }

    PlatformMD5Context ctx = nullptr;
    if (platform_md5_init(&ctx) != 0) {
        return -1;
    }

    if (platform_md5_update(ctx, data, len) != 0) {
        return -1;
    }

    int digest_len = 0;
    if (platform_md5_final(ctx, digest, digest_len) != 0) {
        return -1;
    }

    return 0;
}
