/**
 * tests/osd/test_osd_record.c
 *
 * Proof-of-OSD test: sets a visible channel title, records 8 s of live video
 * to MP4 while the overlay is active, saves the file, and verifies the
 * container is valid with ffprobe.
 *
 * The recorded file can be inspected visually to confirm the OSD is burned
 * into the stream (camera-dependent — not all cameras overlay on IP stream).
 *
 * Usage:
 *   test_osd_record [host [user [password [output.mp4]]]]
 */

#include "cppdvr_api.h"
#include "test_helpers.h"

#ifdef _WIN32
#  define popen  _popen
#  define pclose _pclose
#endif

static int run_print(const char* cmd) {
    printf("       [cmd] %s\n", cmd); fflush(stdout);
    FILE* fp = popen(cmd, "r");
    if (!fp) { printf("       popen failed\n"); return -1; }
    char line[1024];
    while (fgets(line, sizeof(line), fp)) printf("       %s", line);
    int rc = pclose(fp);
#ifdef _WIN32
    return rc;
#else
    return WEXITSTATUS(rc);
#endif
}

static long file_bytes(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);
    return sz;
}

static void stream_log(const char* msg, void* ud) { (void)ud; INFO("%s", msg); }
static void rec_log  (const char* msg, void* ud) { (void)ud; INFO("[rec] %s", msg); }

int main(int argc, char* argv[]) {
    const char* host    = (argc >= 2) ? argv[1] : dvr_env("DVR_HOST", DVR_DEFAULT_HOST);
    const char* user    = (argc >= 3) ? argv[2] : dvr_env("DVR_USER", DVR_DEFAULT_USER);
    const char* pass    = (argc >= 4) ? argv[3] : dvr_env("DVR_PASS", DVR_DEFAULT_PASS);
    const char* outfile = (argc >= 5) ? argv[4] : "osd_overlay_test.mp4";
    int rec_secs = 8;

    printf("=== OSD Overlay + Recording Test ===\n");
    INFO("Host: %s  User: %s  Output: %s  Duration: %d s",
         host, user, outfile, rec_secs);

    /* ── Login ──────────────────────────────────────────────────────────── */
    SECTION("Login");

    DVRHandle dh = dvr_create(host, DVR_DEFAULT_PORT, user, pass);
    if (!dh || !dvr_login(dh)) {
        FAIL("Login failed");
        dvr_destroy(dh);
        EXIT_SUMMARY();
    }
    PASS("Login OK");

    /* ── Set channel title overlay ──────────────────────────────────────── */
    SECTION("Set channel title overlay");

    const char* titles[] = { "OSD TEST ACTIVE" };
    if (dvr_set_channel_titles(dh, titles, 1)) {
        PASS("Channel title set to 'OSD TEST ACTIVE'");
    } else {
        FAIL("dvr_set_channel_titles() failed");
        char errbuf[256]; dvr_last_error(dh, errbuf, sizeof(errbuf));
        if (errbuf[0]) INFO("last_error: %s", errbuf);
    }

    /* Allow overlay to propagate before recording */
    sleep_ms(500);

    /* ── Create StreamServer + Recorder ─────────────────────────────────── */
    SECTION("Create stream server and recorder");

    StreamHandle   sh  = stream_create(host, DVR_DEFAULT_PORT, user, pass, 0);
    RecorderHandle rec = recorder_create();

    if (!sh)  { FAIL("stream_create() returned NULL");  goto cleanup_dvr; }
    if (!rec) { FAIL("recorder_create() returned NULL"); goto cleanup_stream; }
    PASS("stream_create + recorder_create OK");

    stream_set_log_callback(sh,  stream_log, NULL);
    recorder_set_log_callback(rec, rec_log,  NULL);

    if (!recorder_init_with_stream(rec, sh)) {
        FAIL("recorder_init_with_stream() failed");
        goto cleanup_all;
    }
    PASS("recorder_init_with_stream OK");

    /* ── Start recording first, then stream ─────────────────────────────── */
    SECTION("Start recording");

    if (!recorder_start(rec, outfile, RECORDER_FORMAT_MP4, 25)) {
        FAIL("recorder_start() failed");
        goto cleanup_all;
    }
    PASS("recorder_start OK");

    SECTION("Start DVR stream");

    if (!stream_start(sh, "Main")) {
        FAIL("stream_start() failed (check DVR reachable + ffmpeg in PATH)");
        goto cleanup_all;
    }
    PASS("stream_start OK");

    /* ── Record for rec_secs ─────────────────────────────────────────────── */
    SECTION("Recording with OSD overlay active");

    INFO("Streaming with 'OSD TEST ACTIVE' title for %d s ...", rec_secs);
    for (int i = 0; i < rec_secs; ++i) {
        sleep_ms(1000);
        INFO("[t+%ds]  frames written=%zu  dropped=%zu",
             i + 1,
             recorder_frames_recorded(rec),
             recorder_frames_dropped(rec));
    }

    size_t frames_written = recorder_frames_recorded(rec);
    if (frames_written > 0) {
        PASSF("%zu frames written to MP4", frames_written);
    } else {
        FAIL("No frames were written — stream may not have connected");
    }

    /* ── Save + stop ─────────────────────────────────────────────────────── */
    SECTION("Save and stop");

    INFO("Finalising MP4 (waiting for ffmpeg muxer) ...");
    recorder_save(rec);
    PASS("recorder_save completed");

    stream_stop(sh);
    PASS("stream_stop completed");

    /* ── Restore original channel title ─────────────────────────────────── */
    SECTION("Restore channel title");

    const char* blank[] = { "" };
    if (dvr_set_channel_titles(dh, blank, 1)) {
        PASS("Channel title cleared");
    } else {
        FAIL("Could not clear channel title");
    }

    /* ── Verify output MP4 ───────────────────────────────────────────────── */
    SECTION("Verify output file");

    long sz = file_bytes(outfile);
    if (sz < 0) {
        FAIL("Output file does not exist");
        goto cleanup_all;
    }
    INFO("File: %s  size: %ld bytes (%.1f KB)", outfile, sz, sz / 1024.0);
    if (sz >= 8192) {
        PASS("Output file exists and is reasonably sized");
    } else {
        FAILF("File suspiciously small: %ld bytes", sz);
    }

    /* ── ffprobe ─────────────────────────────────────────────────────────── */
    SECTION("ffprobe metadata");
    {
        char cmd[512];
        snprintf(cmd, sizeof(cmd),
                 "ffprobe -v quiet -show_streams -print_format flat \"%s\"",
                 outfile);
        if (run_print(cmd) == 0) {
            PASS("ffprobe metadata OK");
        } else {
            FAIL("ffprobe exited with non-zero status");
        }
    }

    /* ── Extract first frame ─────────────────────────────────────────────── */
    SECTION("Extract first frame from MP4");
    {
        char cmd[512];
        const char* frame_jpg = "osd_overlay_frame0.jpg";
        snprintf(cmd, sizeof(cmd),
                 "ffmpeg -loglevel error -i \"%s\" -frames:v 1 -y \"%s\"",
                 outfile, frame_jpg);
        if (run_print(cmd) == 0) {
            FILE* jf = fopen(frame_jpg, "rb");
            if (jf) {
                unsigned char magic[2] = {0, 0};
                long jsz;
                fread(magic, 1, 2, jf);
                fseek(jf, 0, SEEK_END); jsz = ftell(jf);
                fclose(jf);
                INFO("Frame JPEG magic: %02X %02X  size: %ld bytes",
                     magic[0], magic[1], jsz);
                if (magic[0] == 0xFF && magic[1] == 0xD8)
                    PASS("Extracted frame is a valid JPEG (FF D8)");
                else
                    FAIL("Extracted frame does not start with JPEG SOI");
            } else {
                FAIL("Extracted JPEG file not found");
            }
        } else {
            FAIL("ffmpeg frame extraction failed");
        }
    }

cleanup_all:
    if (rec) { recorder_deinit(rec); recorder_destroy(rec); rec = NULL; }
cleanup_stream:
    if (sh)  { stream_destroy(sh); sh = NULL; }
cleanup_dvr:
    dvr_destroy(dh);
    EXIT_SUMMARY();
}
