/**
 * tests/osd/test_overlay_only.c
 *
 * Records 10 s of video with the HUD overlay burned in.
 * Single recording stream — the pattern to copy into an application that needs
 * overlay recording without a separate raw copy.
 *
 * Output: overlay_only.mp4
 *
 * How it works
 * ────────────
 *   stream_set_overlay_jpeg_callback() fires after the overlay pipeline
 *   (decode → draw boxes → re-encode) and feeds the result to the recorder.
 *   stream_set_jpeg_callback() is left unset, so there is no second consumer
 *   competing for the same frame.
 *
 * Performance note
 * ────────────────
 *   Overlay recording requires a full JPEG decode + re-encode every camera
 *   frame.  On a 2880×1616 stream at 25 fps this is the dominant CPU cost.
 *   If the application display slows down while recording is active, the
 *   bottleneck is the overlay pipeline, not the recorder.  Options:
 *     • Remove overlay boxes if real-time overlay is not needed for recording.
 *     • Use raw recording (recorder_init_with_stream) for the stored copy and
 *       burn the overlay only for display — zero decode/re-encode overhead.
 *
 * Usage:
 *   test_overlay_only [host [user [password]]]
 */

#include "cppdvr_api.h"
#include "test_helpers.h"

#include <stdio.h>

static RecorderHandle g_rec = NULL;

static void on_overlay_jpeg(const uint8_t* jpeg, size_t size,
                             uint32_t frame_id, void* ud) {
    (void)frame_id; (void)ud;
    if (g_rec) recorder_feed_jpeg(g_rec, jpeg, size);
}

static void stream_log(const char* msg, void* ud) { (void)ud; INFO("%s", msg); }

int main(int argc, char* argv[]) {
    const char* host     = (argc >= 2) ? argv[1] : dvr_env("DVR_HOST", DVR_DEFAULT_HOST);
    const char* user     = (argc >= 3) ? argv[2] : dvr_env("DVR_USER", DVR_DEFAULT_USER);
    const char* pass     = (argc >= 4) ? argv[3] : dvr_env("DVR_PASS", DVR_DEFAULT_PASS);
    const char* outfile  = "overlay_only.mp4";
    const int   rec_secs = 10;

    printf("=== Overlay-Only Recording Test (10 s) ===\n");
    INFO("Host: %s  User: %s  Output: %s", host, user, outfile);
    INFO("Single stream: overlay burned in, no raw copy");

    StreamHandle   sh  = stream_create(host, DVR_DEFAULT_PORT, user, pass, 0);
    RecorderHandle rec = recorder_create();
    if (!sh || !rec) {
        FAIL("stream_create or recorder_create returned NULL");
        if (rec) recorder_destroy(rec);
        if (sh)  stream_destroy(sh);
        EXIT_SUMMARY();
    }
    g_rec = rec;
    PASS("stream_create + recorder_create OK");

    stream_set_log_callback(sh, stream_log, NULL);

    /* ── HUD boxes: top-right corner, right-aligned, 32 px glyphs ─────────── */
    stream_overlay_box_configure(sh, 0,
        /*x_pad=*/16, /*y_pad=*/16, /*box_w=*/700, /*scale=*/4,
        STREAM_OVERLAY_ALIGN_RIGHT, STREAM_OVERLAY_ANCHOR_TOP_RIGHT);
    stream_overlay_box_print(sh, 0, "CGS Venture Inc.");

    stream_overlay_box_configure(sh, 1, 16, 56, 700, 4,
        STREAM_OVERLAY_ALIGN_RIGHT, STREAM_OVERLAY_ANCHOR_TOP_RIGHT);
    stream_overlay_box_print(sh, 1, "0.00 m");

    stream_overlay_box_configure(sh, 2, 16, 96, 700, 4,
        STREAM_OVERLAY_ALIGN_RIGHT, STREAM_OVERLAY_ANCHOR_TOP_RIGHT);
    stream_overlay_box_print(sh, 2, "Bandar Puteri, Puchong, Selangor");

    PASS("HUD boxes configured (top-right, right-aligned, scale=4)");

    /* Post-overlay JPEG callback feeds the recorder */
    stream_set_overlay_jpeg_callback(sh, on_overlay_jpeg, NULL);

    if (!recorder_init_standalone(rec)) {
        FAIL("recorder_init_standalone() failed");
        goto cleanup;
    }
    PASS("recorder_init_standalone OK");

    if (!recorder_start(rec, outfile, RECORDER_FORMAT_MJPEG, 25)) {
        FAIL("recorder_start() failed");
        goto cleanup;
    }
    PASS("recorder_start OK -> overlay_only.mp4");

    if (!stream_start(sh, "Main")) {
        FAIL("stream_start() failed");
        goto cleanup;
    }
    PASS("stream_start OK");

    INFO("Recording for %d s ...", rec_secs);
    for (int i = 0; i < rec_secs; ++i) {
        sleep_ms(1000);

        /* Push a sensor update each second — safe to call from any thread */
        char dist[64];
        snprintf(dist, sizeof(dist), "%.2f m", (double)i * 0.75);
        stream_overlay_box_print(sh, 1, dist);

        INFO("[t+%2ds]  recorded=%zu  dropped=%zu",
             i + 1,
             recorder_frames_recorded(rec),
             recorder_frames_dropped(rec));
    }

    {
        size_t n = recorder_frames_recorded(rec);
        if (n > 0) PASSF("%zu frames recorded with overlay", n);
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
            "ffmpeg -loglevel error -i \"%s\" -frames:v 1 -y overlay_only_frame0.jpg",
            outfile);
        INFO("[cmd] %s", cmd);
        system(cmd);

        FILE* f = fopen("overlay_only_frame0.jpg", "rb");
        if (f) {
            uint8_t hdr[2] = {0};
            fread(hdr, 1, 2, f);
            fclose(f);
            if (hdr[0] == 0xFF && hdr[1] == 0xD8) PASS("Extracted frame is valid JPEG");
            else                                    FAIL("Extracted frame not valid JPEG");
        } else {
            FAIL("Could not extract frame — check overlay_only_frame0.jpg");
        }
    }

cleanup:
    if (rec) { recorder_deinit(rec); recorder_destroy(rec); }
    if (sh)  { stream_destroy(sh); }
    EXIT_SUMMARY();
}
