/**
 * tests/osd/test_raw_only.c
 *
 * Records 10 s of raw H.265/H.264 video with no overlay and no JPEG
 * decode/re-encode step.
 *
 * Output: raw_only.mp4
 *
 * How it works
 * ────────────
 *   recorder_init_with_stream() hooks the raw NAL callback so H.265/H.264
 *   frames from the camera are piped directly into the MP4 container via
 *   ffmpeg -c:v copy.  No overlay is configured, so the overlay pipeline
 *   (decode → draw → re-encode) is bypassed entirely — the jpeg_callback
 *   fires with the original decoded JPEG at zero extra decode/encode cost.
 *
 * Performance note
 * ────────────────
 *   This path has the lowest possible CPU overhead for recording.
 *   The application's JPEG display callback continues to run at full rate
 *   because recording does not share the same thread or buffer.
 *   Use this pattern when overlay text is needed only on the live display,
 *   not in the stored file.
 *
 * Usage:
 *   test_raw_only [host [user [password]]]
 */

#include "cppdvr_api.h"
#include "test_helpers.h"

#include <stdio.h>

static void stream_log(const char* msg, void* ud) { (void)ud; INFO("%s", msg); }

int main(int argc, char* argv[]) {
    const char* host     = (argc >= 2) ? argv[1] : dvr_env("DVR_HOST", DVR_DEFAULT_HOST);
    const char* user     = (argc >= 3) ? argv[2] : dvr_env("DVR_USER", DVR_DEFAULT_USER);
    const char* pass     = (argc >= 4) ? argv[3] : dvr_env("DVR_PASS", DVR_DEFAULT_PASS);
    const char* outfile  = "raw_only.mp4";
    const int   rec_secs = 10;

    printf("=== Raw-Only Recording Test (10 s) ===\n");
    INFO("Host: %s  User: %s  Output: %s", host, user, outfile);
    INFO("Single stream: H.265 stream-copy, no overlay, no decode/re-encode");

    StreamHandle   sh  = stream_create(host, DVR_DEFAULT_PORT, user, pass, 0);
    RecorderHandle rec = recorder_create();
    if (!sh || !rec) {
        FAIL("stream_create or recorder_create returned NULL");
        if (rec) recorder_destroy(rec);
        if (sh)  stream_destroy(sh);
        EXIT_SUMMARY();
    }
    PASS("stream_create + recorder_create OK");

    stream_set_log_callback(sh, stream_log, NULL);

    /* No overlay configured — the overlay pipeline is skipped every frame */

    if (!recorder_init_with_stream(rec, sh)) {
        FAIL("recorder_init_with_stream() failed");
        goto cleanup;
    }
    PASS("recorder_init_with_stream OK  (hooks raw NAL callback)");

    if (!recorder_start(rec, outfile, RECORDER_FORMAT_MP4, 25)) {
        FAIL("recorder_start() failed");
        goto cleanup;
    }
    PASS("recorder_start OK -> raw_only.mp4  (H.265 stream-copy, -c:v copy)");

    if (!stream_start(sh, "Main")) {
        FAIL("stream_start() failed");
        goto cleanup;
    }
    PASS("stream_start OK");

    INFO("Recording for %d s ...", rec_secs);
    for (int i = 0; i < rec_secs; ++i) {
        sleep_ms(1000);
        INFO("[t+%2ds]  recorded=%zu  dropped=%zu",
             i + 1,
             recorder_frames_recorded(rec),
             recorder_frames_dropped(rec));
    }

    {
        size_t n = recorder_frames_recorded(rec);
        if (n > 0) PASSF("%zu frames recorded (raw NAL)", n);
        else       FAIL("No frames recorded");
    }

    stream_stop(sh);
    PASS("stream_stop OK");

    recorder_save(rec);
    PASS("recorder_save OK");

    {
        FILE* f = fopen(outfile, "rb");
        if (!f) { FAIL("Output file not found"); goto cleanup; }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fclose(f);
        INFO("Output: %s  %ld bytes (%.1f KB)", outfile, sz, sz / 1024.0);
        if (sz >= 8192) PASSF("File size OK (%ld bytes)", sz);
        else            FAILF("File suspiciously small (%ld bytes)", sz);
    }

    SECTION("Extract first frame");
    {
        char cmd[512];
        snprintf(cmd, sizeof(cmd),
            "ffmpeg -loglevel error -i \"%s\" -frames:v 1 -y raw_only_frame0.jpg",
            outfile);
        INFO("[cmd] %s", cmd);
        system(cmd);

        FILE* f = fopen("raw_only_frame0.jpg", "rb");
        if (f) {
            uint8_t hdr[2] = {0};
            fread(hdr, 1, 2, f);
            fclose(f);
            if (hdr[0] == 0xFF && hdr[1] == 0xD8) PASS("Extracted frame is valid JPEG");
            else                                    FAIL("Extracted frame not valid JPEG");
        } else {
            FAIL("Could not extract frame — check raw_only_frame0.jpg");
        }
    }

cleanup:
    if (rec) { recorder_deinit(rec); recorder_destroy(rec); }
    if (sh)  { stream_destroy(sh); }
    EXIT_SUMMARY();
}
