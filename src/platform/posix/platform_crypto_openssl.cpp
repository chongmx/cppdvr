/**
 * @file platform_crypto_openssl.cpp
 * @brief POSIX/Linux implementation of platform_crypto.h using OpenSSL EVP.
 *
 * Selected when CPPDVR_CRYPTO=OPENSSL or CPPDVR_CRYPTO=FETCH.
 */

#include "platform_crypto.h"

#include <openssl/evp.h>
#include <cstring>

struct MD5ContextInternal {
    EVP_MD_CTX* ctx;
};

int platform_md5_init(PlatformMD5Context* ctx) {
    if (!ctx) return -1;
    auto* internal = new MD5ContextInternal;
    internal->ctx = EVP_MD_CTX_new();
    if (!internal->ctx) { delete internal; return -1; }
    if (!EVP_DigestInit_ex(internal->ctx, EVP_md5(), nullptr)) {
        EVP_MD_CTX_free(internal->ctx);
        delete internal;
        return -1;
    }
    *ctx = static_cast<PlatformMD5Context>(internal);
    return 0;
}

int platform_md5_update(PlatformMD5Context ctx, const void* data, int len) {
    if (!ctx || !data || len < 0) return -1;
    auto* s = static_cast<MD5ContextInternal*>(ctx);
    return EVP_DigestUpdate(s->ctx, data, len) ? 0 : -1;
}

int platform_md5_final(PlatformMD5Context ctx, uint8_t* digest, int& digest_len) {
    if (!ctx || !digest) return -1;
    auto* s = static_cast<MD5ContextInternal*>(ctx);
    unsigned int len = 0;
    int ok = EVP_DigestFinal_ex(s->ctx, digest, &len);
    EVP_MD_CTX_free(s->ctx);
    delete s;
    if (!ok) return -1;
    digest_len = static_cast<int>(len);
    return 0;
}

int platform_md5(const void* data, int len, uint8_t* digest) {
    if (!data || !digest || len < 0) return -1;
    PlatformMD5Context ctx = nullptr;
    if (platform_md5_init(&ctx) != 0) return -1;
    if (platform_md5_update(ctx, data, len) != 0) return -1;
    int digest_len = 0;
    return platform_md5_final(ctx, digest, digest_len);
}
