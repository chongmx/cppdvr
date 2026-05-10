/**
 * tests/streaming/test_streaming.c
 *
 * Tests the full StreamServer pipeline (DVR → DVRIP → ffmpeg → JPEG).
 * Mirrors the logic in demo/test_frame.cpp but uses the C API.
 *
 * Usage:
 *   test_streaming [host [user [password [stream [seconds]]]]]
 *
 * Environment:
 *   DVR_HOST, DVR_USER, DVR_PASS, DVR_STREAM
 */

#include "cppdvr_api.h"
#include "test_helpers.h"

#include <stdint.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
typedef volatile LONG atomic_int_t;
#  define ATOMIC_INC(x) InterlockedIncrement(&(x))
#  define ATOMIC_LOAD(x) ((x))
#else
#  include <stdatomic.h>
typedef atomic_int    atomic_int_t;
#  define ATOMIC_INC(x) atomic_fetch_add(&(x), 1)
#  define ATOMIC_LOAD(x) atomic_load(&(x))
#endif

static void log_to_stderr(const char* msg, void* userdata) {
    (void)userdata;
    fprintf(stderr, "[DVR] %s\n", msg);
    fflush(stderr);
}

typedef struct {
    atomic_int_t  frame_count;
    /* First-frame capture (written once, then never updated) */
    uint8_t*      first_jpeg;
    size_t        first_size;
    uint32_t      first_id;
    int           got_first;
} StreamState;

static void on_jpeg(const uint8_t* jpeg, size_t size,
                    uint32_t frame_id, void* userdata) {
    StreamState* s = (StreamState*)userdata;
    ATOMIC_INC(s->frame_count);

    if (!s->got_first && size > 0) {
        s->first_jpeg = (uint8_t*)malloc(size);
        if (s->first_jpeg) {
            memcpy(s->first_jpeg, jpeg, size);
            s->first_size = size;
            s->first_id   = frame_id;
        }
        s->got_first = 1;
    }
}

static int write_file(const char* path, const uint8_t* data, size_t size) {
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    size_t n = fwrite(data, 1, size, f);
    fclose(f);
    return (n == size) ? 1 : 0;
}

int main(int argc, char* argv[]) {
    const char* host   = (argc >= 2) ? argv[1] : dvr_env("DVR_HOST",   DVR_DEFAULT_HOST);
    const char* user   = (argc >= 3) ? argv[2] : dvr_env("DVR_USER",   DVR_DEFAULT_USER);
    const char* pass   = (argc >= 4) ? argv[3] : dvr_env("DVR_PASS",   DVR_DEFAULT_PASS);
    const char* stream = (argc >= 5) ? argv[4] : dvr_env("DVR_STREAM", DVR_DEFAULT_STREAM);
    int seconds        = (argc >= 6) ? atoi(argv[5]) : 5;

    printf("=== Streaming Test (StreamServer + ffmpeg) ===\n");
    INFO("Host: %s  User: %s  Stream: %s  Duration: %d s", host, user, stream, seconds);

    /* ── Create stream server ──────────────────────────────────────────────── */
    SECTION("Create and start stream server");

    StreamHandle h = stream_create(host, DVR_DEFAULT_PORT, user, pass, 0);
    if (!h) {
        FAIL("stream_create() returned NULL");
        EXIT_SUMMARY();
    }
    PASS("stream_create() succeeded");

    StreamState state;
    memset(&state, 0, sizeof(state));

    stream_set_log_callback(h, log_to_stderr, NULL);
    stream_set_jpeg_callback(h, on_jpeg, &state);

    if (!stream_start(h, stream)) {
        FAIL("stream_start() returned 0 (is ffmpeg in PATH?)");
        stream_destroy(h);
        EXIT_SUMMARY();
    }
    PASS("stream_start() succeeded");

    /* ── Wait for first JPEG (up to 15 s) ─────────────────────────────────── */
    SECTION("First JPEG frame arrival");

    int waited = 0;
    while (!state.got_first && waited < 15000) {
        sleep_ms(100);
        waited += 100;
    }

    if (!state.got_first) {
        FAIL("No JPEG frame received within 15 seconds");
        INFO("Check: is ffmpeg.exe in PATH?  Is the camera reachable?");
        stream_stop(h);
        stream_destroy(h);
        EXIT_SUMMARY();
    }
    PASS("First JPEG frame received");
    INFO("Frame ID: %u", state.first_id);
    INFO("Size:     %zu bytes  (%.1f KB)", state.first_size, state.first_size / 1024.0);

    /* ── Validate JPEG structure ────────────────────────────────────────────── */
    SECTION("JPEG validation");

    if (state.first_size < 4) {
        FAILF("Frame too small: %zu bytes", state.first_size);
    } else {
        const uint8_t* j = state.first_jpeg;
        size_t         sz = state.first_size;

        if (j[0] == 0xFF && j[1] == 0xD8) {
            PASS("Starts with JPEG SOI marker (FF D8)");
        } else {
            FAILF("Expected FF D8, got %02X %02X", j[0], j[1]);
        }

        if (j[sz-2] == 0xFF && j[sz-1] == 0xD9) {
            PASS("Ends with JPEG EOI marker (FF D9)");
        } else {
            INFO("Last 2 bytes: %02X %02X (EOI — some encoders omit it)", j[sz-2], j[sz-1]);
        }

        if (sz >= 1024) {
            PASS("Frame size >= 1 KB (plausible JPEG)");
        } else {
            FAILF("Frame unusually small: %zu bytes", sz);
        }

        /* Save to disk for visual inspection */
        if (write_file("streaming_test.jpg", j, sz)) {
            PASS("Saved to streaming_test.jpg");
        } else {
            FAIL("Could not write streaming_test.jpg");
        }
    }
    free(state.first_jpeg);
    state.first_jpeg = NULL;

    /* ── Count frames over the configured window ───────────────────────────── */
    SECTION("Frame count over duration");

    int before = (int)ATOMIC_LOAD(state.frame_count);
    sleep_ms(seconds * 1000);
    int after    = (int)ATOMIC_LOAD(state.frame_count);
    int received = after - before;

    INFO("Frames received in %d s: %d", seconds, received);
    INFO("Total frames since start: %d", after);

    if (received >= seconds) {  /* at least 1 fps */
        PASS("Frame rate is at least 1 fps");
    } else {
        FAILF("Too few frames: %d in %d s", received, seconds);
    }

    /* ── Stop ───────────────────────────────────────────────────────────────── */
    SECTION("Stop stream server");
    stream_stop(h);
    PASS("stream_stop() completed");

    stream_destroy(h);
    EXIT_SUMMARY();
}
