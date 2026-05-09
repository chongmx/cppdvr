#pragma once
/**
 * tests/common/test_helpers.h
 * Shared macros, defaults, and helpers for all DVR integration tests.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  define sleep_ms(ms) Sleep(ms)
#else
#  include <unistd.h>
#  define sleep_ms(ms) usleep((useconds_t)(ms) * 1000u)
#endif

/* ── Default camera config ────────────────────────────────────────────────────
   Override at runtime via environment variables:
     DVR_HOST, DVR_PORT, DVR_USER, DVR_PASS, DVR_STREAM
   Or pass on the command line as positional arguments (test-specific). */
#define DVR_DEFAULT_HOST   "172.20.80.12"
#define DVR_DEFAULT_PORT   0              /* 0 = library default 34567 */
#define DVR_DEFAULT_USER   "admin"
#define DVR_DEFAULT_PASS   ""
#define DVR_DEFAULT_STREAM "Main"

static inline const char* dvr_env(const char* var, const char* fallback) {
    const char* v = getenv(var);
    return (v && v[0]) ? v : fallback;
}

/* ── Pass/Fail accounting ─────────────────────────────────────────────────── */
static int g_pass = 0;
static int g_fail = 0;

#define PASS(msg) \
    do { printf("[PASS] %s\n", msg); fflush(stdout); ++g_pass; } while(0)

#define PASSF(fmt, ...) \
    do { printf("[PASS] " fmt "\n", __VA_ARGS__); fflush(stdout); ++g_pass; } while(0)

#define FAIL(msg) \
    do { printf("[FAIL] %s\n", msg); fflush(stdout); ++g_fail; } while(0)

#define FAILF(fmt, ...) \
    do { printf("[FAIL] " fmt "\n", __VA_ARGS__); fflush(stdout); ++g_fail; } while(0)

#define INFO(fmt, ...) \
    do { printf("       " fmt "\n", ##__VA_ARGS__); fflush(stdout); } while(0)

#define SKIP(msg) \
    do { printf("[SKIP] %s\n", msg); fflush(stdout); } while(0)

#define SECTION(name) \
    do { printf("\n--- %s ---\n", name); fflush(stdout); } while(0)

/* ── Summary and exit ─────────────────────────────────────────────────────── */
#define PRINT_SUMMARY() \
    do { \
        printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail); \
        fflush(stdout); \
    } while(0)

#define EXIT_SUMMARY() \
    do { \
        PRINT_SUMMARY(); \
        return (g_fail == 0) ? 0 : 1; \
    } while(0)
