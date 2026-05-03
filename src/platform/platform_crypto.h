/**
 * @file platform_crypto.h
 * @brief Cross-platform cryptographic abstraction layer
 * 
 * This header defines a platform-agnostic crypto API that abstracts
 * the differences between Windows CryptoAPI and OpenSSL (Linux).
 * Currently supports MD5 hashing for DVRIP authentication.
 */

#pragma once

#include <cstdint>
#include <cstring>

/**
 * @brief MD5 digest size in bytes
 */
constexpr int PLATFORM_MD5_DIGEST_SIZE = 16;

/**
 * @brief Opaque MD5 context handle
 * Windows: Wrapper around HCRYPTHASH
 * POSIX: Wrapper around EVP_MD_CTX
 */
using PlatformMD5Context = void*;

/**
 * @brief Initialize an MD5 hash context
 * 
 * Must be called once before any data is hashed.
 * 
 * @param ctx Output: MD5 context pointer
 * @return 0 on success, non-zero on error
 */
int platform_md5_init(PlatformMD5Context* ctx);

/**
 * @brief Update MD5 hash with data
 * 
 * Can be called multiple times to hash data in chunks.
 * 
 * @param ctx MD5 context from platform_md5_init()
 * @param data Data to hash
 * @param len Length of data in bytes
 * @return 0 on success, non-zero on error
 */
int platform_md5_update(PlatformMD5Context ctx, const void* data, int len);

/**
 * @brief Finalize MD5 hash and get digest
 * 
 * After calling this, the context is invalid and should not be used.
 * 
 * @param ctx MD5 context from platform_md5_init()
 * @param digest Output buffer: must be at least PLATFORM_MD5_DIGEST_SIZE bytes
 * @param digest_len Input: size of digest buffer; Output: actual bytes written
 * @return 0 on success, non-zero on error
 */
int platform_md5_final(PlatformMD5Context ctx, uint8_t* digest, int& digest_len);

/**
 * @brief One-shot MD5 hash function
 * 
 * Convenience function that initializes, updates, and finalizes in one call.
 * 
 * @param data Data to hash
 * @param len Length of data in bytes
 * @param digest Output buffer: must be at least PLATFORM_MD5_DIGEST_SIZE bytes
 * @return 0 on success, non-zero on error
 */
int platform_md5(const void* data, int len, uint8_t* digest);
