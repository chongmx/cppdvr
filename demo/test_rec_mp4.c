/**
 * test_rec_mp4.c — Integration test: record live DVR stream to MP4
 *
 * Connects to a real DVR camera, records for a configurable duration,
 * saves the MP4, verifies the file, prints ffprobe metadata, and
 * extracts the first frame as JPEG to confirm the container is valid.
 *
 * Usage:
 *   test_rec_mp4 [host [port [user [password [stream [seconds [output]]]]]]]
 *
 * All arguments are optional — unspecified args fall back to environment
 * variables, then to compiled-in defaults:
 *
 *   DVR_HOST     192.168.1.10
 *   DVR_PORT     0          (0 = library default 34567)
 *   DVR_USER     admin
 *   DVR_PASS     (empty string)
 *   DVR_STREAM   Main       ("Main" or "Extra")
 *   REC_SECONDS  8
 *   REC_FILE     test_rec_out.mp4
 */

#include "cppdvr_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  define sleep_ms(ms) Sleep(ms)
#  define popen  _popen
#  define pclose _pclose
#else
#  include <unistd.h>
#  define sleep_ms(ms) usleep((ms) * 1000)
#endif

/* ── helpers ───────────────────────────────────────────────────────────────── */

static int g_pass = 0;
static int g_fail = 0;

#define PASS(msg) do { printf("[PASS] %s\n", msg); fflush(stdout); ++g_pass; } while(0)
#define FAIL(msg) do { printf("[FAIL] %s\n", msg); fflush(stdout); ++g_fail; } while(0)
#define FAILF(fmt,...) do { printf("[FAIL] " fmt "\n", __VA_ARGS__); fflush(stdout); ++g_fail; } while(0)
#define INFO(fmt,...) do { printf("       " fmt "\n", ##__VA_ARGS__); fflush(stdout); } while(0)
#define BANNER(msg) do { printf("\n=== %s ===\n", msg); fflush(stdout); } while(0)

static void stream_log(const char* msg, void* ud) {
    (void)ud;
    printf("[stream] %s\n", msg); fflush(stdout);
}
static void rec_log(const char* msg, void* ud) {
    (void)ud;
    printf("[rec]    %s\n", msg); fflush(stdout);
}

/* Run a shell command; print its stdout.  Returns the exit code. */
static int run_print(const char* cmd) {
    printf("[cmd] %s\n", cmd); fflush(stdout);
    FILE* fp = popen(cmd, "r");
    if (!fp) { printf("       popen failed\n"); return -1; }
    char line[1024];
    while (fgets(line, sizeof(line), fp)) printf("       %s", line);
    int rc = pclose(fp);
#ifdef _WIN32
    return rc;   /* MSVC pclose returns exit code directly */
#else
    return WEXITSTATUS(rc);
#endif
}

/* Return file size in bytes, or -1 if file not found. */
static long file_size(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);
    return sz;
}

/* Get config value: argv[idx] > env var > compiled default */
static const char* cfg(int argc, char** argv, int idx,
                        const char* env_var, const char* def) {
    if (argc > idx && argv[idx] && argv[idx][0]) return argv[idx];
    const char* e = getenv(env_var);
    return (e && e[0]) ? e : def;
}

/* ── main ──────────────────────────────────────────────────────────────────── */

int main(int argc, char** argv)
{
    /* ── Configuration ──────────────────────────────────────────────────────── */
    const char* dvr_host  = cfg(argc, argv, 1, "DVR_HOST",    "192.168.1.10");
    const char* dvr_port_s= cfg(argc, argv, 2, "DVR_PORT",    "0");
    const char* dvr_user  = cfg(argc, argv, 3, "DVR_USER",    "admin");
    const char* dvr_pass  = cfg(argc, argv, 4, "DVR_PASS",    "");
    const char* dvr_stream= cfg(argc, argv, 5, "DVR_STREAM",  "Main");
    const char* rec_secs_s= cfg(argc, argv, 6, "REC_SECONDS", "8");
    const char* rec_file  = cfg(argc, argv, 7, "REC_FILE",    "test_rec_out.mp4");
    const char* frame_jpg = "test_rec_frame0.jpg";

    int dvr_port = atoi(dvr_port_s);   /* 0 → library default 34567 */
    int rec_secs = atoi(rec_secs_s);
    if (rec_secs < 2) rec_secs = 2;

    printf("========================================\n");
    printf("  test_rec_mp4  —  DVR recording test\n");
    printf("========================================\n");
    INFO("host    : %s", dvr_host);
    INFO("port    : %s", dvr_port ? dvr_port_s : "0 (default 34567)");
    INFO("user    : %s", dvr_user);
    INFO("stream  : %s", dvr_stream);
    INFO("duration: %d s", rec_secs);
    INFO("output  : %s", rec_file);
    printf("\n");

    StreamHandle   stream = NULL;
    RecorderHandle rec    = NULL;
    int            ok;

    /* ── Step 1: create stream server ──────────────────────────────────────── */
    BANNER("Step 1: create stream server");

    stream = stream_create(dvr_host, dvr_port, dvr_user, dvr_pass, 0 /*http_port*/);
    if (!stream) { FAIL("stream_create() returned NULL"); goto done; }
    PASS("stream_create");

    stream_set_log_callback(stream, stream_log, NULL);

    /* ── Step 2: create and init recorder ──────────────────────────────────── */
    BANNER("Step 2: create and init recorder");

    rec = recorder_create();
    if (!rec) { FAIL("recorder_create() returned NULL"); goto done; }
    PASS("recorder_create");

    recorder_set_log_callback(rec, rec_log, NULL);

    ok = recorder_init_with_stream(rec, stream);
    if (!ok) { FAIL("recorder_init_with_stream() failed"); goto done; }
    PASS("recorder_init_with_stream");

    /* ── Step 3: start recording (before stream so no frames are missed) ───── */
    BANNER("Step 3: start recording");

    ok = recorder_start(rec, rec_file, RECORDER_FORMAT_MP4, 25);
    if (!ok) { FAIL("recorder_start() failed (already recording or not init'd?)"); goto done; }
    PASS("recorder_start");

    /* ── Step 4: start the stream (connects to DVR, starts ffmpeg pipeline) ── */
    BANNER("Step 4: start DVR stream");

    ok = stream_start(stream, dvr_stream);
    if (!ok) {
        FAIL("stream_start() failed — check DVR host/port/credentials");
        goto done;
    }
    PASS("stream_start");

    /* ── Step 5: let it record ─────────────────────────────────────────────── */
    BANNER("Step 5: recording ...");

    INFO("waiting %d seconds for frames ...", rec_secs);
    for (int i = 0; i < rec_secs; ++i) {
        sleep_ms(1000);
        size_t written = recorder_frames_recorded(rec);
        size_t dropped = recorder_frames_dropped(rec);
        int    state   = recorder_state(rec);
        const char* state_s = (state == RECORDER_STATE_RECORDING) ? "recording"
                            : (state == RECORDER_STATE_PAUSED)    ? "paused"
                                                                   : "idle";
        INFO("[t+%ds]  state=%s  written=%zu  dropped=%zu",
             i + 1, state_s, written, dropped);
    }

    size_t final_written = recorder_frames_recorded(rec);
    size_t final_dropped = recorder_frames_dropped(rec);
    INFO("total: written=%zu  dropped=%zu", final_written, final_dropped);

    if (final_written == 0) {
        FAIL("no frames were written to ffmpeg — stream may not have started");
        /* still try to save so we get ffmpeg error output */
    } else {
        PASS("frames written to ffmpeg");
    }

    /* ── Step 6: save ──────────────────────────────────────────────────────── */
    BANNER("Step 6: save recording");

    INFO("calling recorder_save() — waits up to 10 s for ffmpeg to finalise ...");
    recorder_save(rec);   /* blocks */
    PASS("recorder_save returned");

    /* ── Step 7: stop stream ───────────────────────────────────────────────── */
    BANNER("Step 7: stop stream");

    stream_stop(stream);
    PASS("stream_stop");

    /* ── Step 8: verify output file ────────────────────────────────────────── */
    BANNER("Step 8: verify output file");

    long sz = file_size(rec_file);
    if (sz < 0) {
        FAIL("output file does not exist");
        goto done;
    }
    INFO("file size: %ld bytes (%.1f KB)", sz, sz / 1024.0);
    if (sz < 8192) {
        FAILF("file is suspiciously small (< 8 KB): %ld bytes", sz);
    } else {
        PASS("output file exists and is reasonably sized");
    }

    /* ── Step 9: ffprobe metadata ──────────────────────────────────────────── */
    BANNER("Step 9: ffprobe metadata");
    {
        char cmd[1024];
        snprintf(cmd, sizeof(cmd),
                 "ffprobe -v quiet -show_streams -print_format flat \"%s\"",
                 rec_file);
        int rc = run_print(cmd);
        if (rc != 0) FAIL("ffprobe exited with non-zero status");
        else          PASS("ffprobe metadata OK");
    }

    /* ── Step 10: extract first frame, validate JPEG magic ─────────────────── */
    BANNER("Step 10: extract first frame");
    {
        char cmd[1024];
        snprintf(cmd, sizeof(cmd),
                 "ffmpeg -loglevel error -i \"%s\" -frames:v 1 -y \"%s\"",
                 rec_file, frame_jpg);
        int rc = run_print(cmd);
        if (rc != 0) {
            FAIL("ffmpeg frame extraction failed");
        } else {
            FILE* jf = fopen(frame_jpg, "rb");
            if (!jf) {
                FAIL("extracted JPEG file not found after ffmpeg extraction");
            } else {
                unsigned char magic[2] = {0, 0};
                fread(magic, 1, 2, jf);
                long jsz = (fseek(jf, 0, SEEK_END), ftell(jf));
                fclose(jf);
                INFO("JPEG magic: %02X %02X   size: %ld bytes", magic[0], magic[1], jsz);
                if (magic[0] == 0xFF && magic[1] == 0xD8)
                    PASS("first frame is a valid JPEG (FF D8)");
                else
                    FAIL("first frame does not have JPEG magic bytes (FF D8)");
            }
        }
    }

done:
    /* ── Cleanup ────────────────────────────────────────────────────────────── */
    if (rec) {
        recorder_deinit(rec);
        recorder_destroy(rec);
    }
    if (stream) {
        stream_destroy(stream);
    }

    printf("\n========================================\n");
    printf("  Results: %d passed,  %d failed\n", g_pass, g_fail);
    printf("========================================\n\n");
    return (g_fail == 0) ? 0 : 1;
}
