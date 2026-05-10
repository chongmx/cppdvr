/**
 * tests/perf/bench_jpeg_backends.c
 *
 * Benchmarks the three JPEG decode/encode backends against each other using
 * a real frame grabbed from the connected DVR camera.
 *
 * What it does
 * ────────────
 *   1. Connects to the camera and grabs one JPEG frame.
 *   2. Disconnects (stream is stopped; all benchmarking runs in isolation).
 *   3. For each backend that is available in this build:
 *        a. BENCH_WARMUP un-timed iterations (prime CPU/GPU caches).
 *        b. BENCH_ITERS timed iterations of: decode → encode.
 *        c. Records min / avg / max for decode, encode, round-trip.
 *   4. Prints a comparison table and switches the active backend back to STB.
 *
 * Build target: bench_jpeg_backends
 * Usage:        bench_jpeg_backends [host [user [password]]]
 */

#include "cppdvr_api.h"
#include "test_helpers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <float.h>

/* ── Portable high-resolution timer ─────────────────────────────────────── */
#ifdef _WIN32
#  include <windows.h>
static double now_ms(void) {
    LARGE_INTEGER freq, cnt;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&cnt);
    return (double)cnt.QuadPart * 1000.0 / (double)freq.QuadPart;
}
#else
#  include <time.h>
static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}
#endif

/* ── Benchmark parameters ────────────────────────────────────────────────── */
#define BENCH_WARMUP   5    /* un-timed warm-up iterations                    */
#define BENCH_ITERS  100    /* timed iterations per backend                   */
#define JPEG_QUALITY  90    /* encode quality 1–100                           */
#define FRAME_WAIT_S  15    /* seconds to wait for first camera frame         */

/* ── Simple stats accumulator ───────────────────────────────────────────── */
typedef struct { double min_ms, max_ms, sum_ms; int n; } Stats;

static void stats_reset(Stats* s) {
    s->min_ms = DBL_MAX; s->max_ms = 0.0; s->sum_ms = 0.0; s->n = 0;
}
static void stats_add(Stats* s, double ms) {
    if (ms < s->min_ms) s->min_ms = ms;
    if (ms > s->max_ms) s->max_ms = ms;
    s->sum_ms += ms; s->n++;
}
static double stats_avg(const Stats* s) {
    return s->n > 0 ? s->sum_ms / s->n : 0.0;
}

/* ── Per-backend result ──────────────────────────────────────────────────── */
typedef struct {
    int    backend;
    int    available;
    int    ok;          /* 1 if benchmark ran without errors */
    Stats  dec, enc, rtt;
} BenchResult;

/* ── Run benchmark for one backend ─────────────────────────────────────── */
static void run_bench(int backend,
                      const uint8_t* jpeg, size_t jpeg_size,
                      BenchResult* out) {
    out->backend   = backend;
    out->available = cppdvr_jpeg_backend_available(backend);
    out->ok        = 0;
    stats_reset(&out->dec);
    stats_reset(&out->enc);
    stats_reset(&out->rtt);

    if (!out->available) return;
    cppdvr_set_jpeg_backend(backend);

    /* Warm-up */
    for (int i = 0; i < BENCH_WARMUP; ++i) {
        int w = 0, h = 0;
        uint8_t* rgb = cppdvr_jpeg_decode(jpeg, jpeg_size, &w, &h);
        if (!rgb) return;
        size_t esz = 0;
        uint8_t* enc = cppdvr_jpeg_encode(rgb, w, h, JPEG_QUALITY, &esz);
        cppdvr_jpeg_free(rgb);
        cppdvr_jpeg_free(enc);
    }

    /* Timed iterations */
    for (int i = 0; i < BENCH_ITERS; ++i) {
        int w = 0, h = 0;

        double t0 = now_ms();
        uint8_t* rgb = cppdvr_jpeg_decode(jpeg, jpeg_size, &w, &h);
        double t1 = now_ms();

        if (!rgb) return;

        size_t esz = 0;
        uint8_t* enc = cppdvr_jpeg_encode(rgb, w, h, JPEG_QUALITY, &esz);
        double t2 = now_ms();

        cppdvr_jpeg_free(rgb);
        cppdvr_jpeg_free(enc);

        if (!enc) return;

        double dec_ms = t1 - t0;
        double enc_ms = t2 - t1;
        stats_add(&out->dec, dec_ms);
        stats_add(&out->enc, enc_ms);
        stats_add(&out->rtt, dec_ms + enc_ms);
    }
    out->ok = 1;
}

/* ── Backend display names ───────────────────────────────────────────────── */
static const char* backend_name(int b) {
    switch (b) {
        case CPPDVR_JPEG_BACKEND_STB:           return "stb_image";
        case CPPDVR_JPEG_BACKEND_LIBJPEG_TURBO: return "libjpeg-turbo";
        case CPPDVR_JPEG_BACKEND_NVJPEG:        return "nvJPEG (GPU)";
        default:                                 return "unknown";
    }
}
static const char* backend_note(int b) {
    switch (b) {
        case CPPDVR_JPEG_BACKEND_STB:           return "portable, zero deps";
        case CPPDVR_JPEG_BACKEND_LIBJPEG_TURBO: return "SIMD (SSE/AVX/NEON)";
        case CPPDVR_JPEG_BACKEND_NVJPEG:        return "NVIDIA GPU";
        default:                                 return "";
    }
}

/* ── Main ────────────────────────────────────────────────────────────────── */
int main(int argc, char* argv[]) {
    const char* host = (argc >= 2) ? argv[1] : dvr_env("DVR_HOST", DVR_DEFAULT_HOST);
    const char* user = (argc >= 3) ? argv[2] : dvr_env("DVR_USER", DVR_DEFAULT_USER);
    const char* pass = (argc >= 4) ? argv[3] : dvr_env("DVR_PASS", DVR_DEFAULT_PASS);

    printf("=== JPEG Backend Benchmark ===\n");
    INFO("Host: %s  User: %s", host, user);
    INFO("Warm-up: %d  Timed iterations: %d  Encode quality: %d",
         BENCH_WARMUP, BENCH_ITERS, JPEG_QUALITY);

    /* ── Available backends in this build ─────────────────────────────── */
    SECTION("Build info");
    {
        int backends[] = { CPPDVR_JPEG_BACKEND_STB,
                           CPPDVR_JPEG_BACKEND_LIBJPEG_TURBO,
                           CPPDVR_JPEG_BACKEND_NVJPEG };
        for (int i = 0; i < 3; ++i) {
            int b = backends[i];
            INFO("  %-18s [%s]  %s",
                 backend_name(b),
                 cppdvr_jpeg_backend_available(b) ? "available" : "not available",
                 backend_note(b));
        }
    }

    /* ── Grab one frame from the camera ──────────────────────────────── */
    SECTION("Grab camera frame");
    StreamHandle sh = stream_create(host, DVR_DEFAULT_PORT, user, pass, 0);
    if (!sh) { FAIL("stream_create returned NULL"); EXIT_SUMMARY(); }

    if (!stream_start(sh, "Main")) {
        FAIL("stream_start() failed");
        stream_destroy(sh);
        EXIT_SUMMARY();
    }
    PASS("stream_start OK — waiting for first frame");

    uint8_t* frame_buf  = NULL;
    size_t   frame_size = 0;
    for (int i = 0; i < FRAME_WAIT_S * 10 && !frame_buf; ++i) {
        sleep_ms(100);
        frame_buf = stream_get_frame(sh, &frame_size);
    }

    stream_stop(sh);
    stream_destroy(sh);

    if (!frame_buf || frame_size == 0) {
        FAIL("No frame received within 15 s");
        EXIT_SUMMARY();
    }

    /* Report frame dimensions */
    {
        int w = 0, h = 0;
        uint8_t* rgb = cppdvr_jpeg_decode(frame_buf, frame_size, &w, &h);
        if (rgb) {
            PASSF("Frame: %d x %d  JPEG size: %.1f KB", w, h, frame_size / 1024.0);
            cppdvr_jpeg_free(rgb);
        } else {
            FAIL("Could not decode grabbed frame");
            stream_free_frame(frame_buf);
            EXIT_SUMMARY();
        }
    }

    /* ── Run benchmarks ───────────────────────────────────────────────── */
    SECTION("Benchmark");

    int backend_list[] = {
        CPPDVR_JPEG_BACKEND_STB,
        CPPDVR_JPEG_BACKEND_LIBJPEG_TURBO,
        CPPDVR_JPEG_BACKEND_NVJPEG,
    };
    int n_backends = (int)(sizeof(backend_list) / sizeof(backend_list[0]));
    BenchResult results[3];

    for (int i = 0; i < n_backends; ++i) {
        int b = backend_list[i];
        if (cppdvr_jpeg_backend_available(b))
            INFO("Benchmarking %s (%d warm-up + %d timed) ...",
                 backend_name(b), BENCH_WARMUP, BENCH_ITERS);
        run_bench(b, frame_buf, frame_size, &results[i]);
        if (results[i].available && results[i].ok) ++g_pass;
        else if (results[i].available)              ++g_fail;
    }

    /* ── Comparison table ─────────────────────────────────────────────── */
    printf("\n");
    printf("  %-18s  %10s  %10s  %10s  %9s\n",
           "Backend", "Decode avg", "Encode avg", "RTT avg", "Eff. FPS");
    printf("  %-18s  %10s  %10s  %10s  %9s\n",
           "------------------", "----------", "----------",
           "----------", "---------");

    for (int i = 0; i < n_backends; ++i) {
        BenchResult* r = &results[i];
        if (!r->available) {
            printf("  %-18s  %s\n", backend_name(r->backend),
                   r->backend == CPPDVR_JPEG_BACKEND_NVJPEG
                       ? "not available (no NVIDIA GPU or not compiled)"
                       : "not available (not compiled in)");
            continue;
        }
        if (!r->ok) {
            printf("  %-18s  FAILED\n", backend_name(r->backend));
            continue;
        }
        double rtt = stats_avg(&r->rtt);
        double fps = rtt > 0.0 ? 1000.0 / rtt : 0.0;
        printf("  %-18s  %7.2f ms  %7.2f ms  %7.2f ms  %6.1f fps\n",
               backend_name(r->backend),
               stats_avg(&r->dec),
               stats_avg(&r->enc),
               rtt, fps);
    }

    /* ── Min/max detail ───────────────────────────────────────────────── */
    printf("\n");
    printf("  %-18s  %22s  %22s\n",
           "Backend", "Decode min–max (ms)", "Encode min–max (ms)");
    printf("  %-18s  %22s  %22s\n",
           "------------------", "----------------------",
           "----------------------");
    for (int i = 0; i < n_backends; ++i) {
        BenchResult* r = &results[i];
        if (!r->available || !r->ok) continue;
        printf("  %-18s  %8.2f – %8.2f    %8.2f – %8.2f\n",
               backend_name(r->backend),
               r->dec.min_ms, r->dec.max_ms,
               r->enc.min_ms, r->enc.max_ms);
    }

    /* ── Speedup over STB ─────────────────────────────────────────────── */
    if (results[0].ok) {
        double stb_rtt = stats_avg(&results[0].rtt);
        printf("\n");
        printf("  Speedup over stb_image (round-trip):\n");
        for (int i = 1; i < n_backends; ++i) {
            BenchResult* r = &results[i];
            if (!r->available || !r->ok) continue;
            double rtt = stats_avg(&r->rtt);
            printf("    %-18s  %.2fx faster\n",
                   backend_name(r->backend),
                   stb_rtt / rtt);
        }
    }

    /* Restore default backend */
    cppdvr_set_jpeg_backend(CPPDVR_JPEG_BACKEND_STB);
    stream_free_frame(frame_buf);
    printf("\n");
    EXIT_SUMMARY();
}
