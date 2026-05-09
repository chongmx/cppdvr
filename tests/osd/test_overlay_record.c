/**
 * tests/osd/test_overlay_record.c
 *
 * Records 5 seconds of live video with custom overlay text burned into
 * every frame using the push-based stream_overlay_print() API.
 *
 * The overlay simulates what a robot operator dashboard might show:
 *   - Distance reading (xx.xx m)
 *   - Location label
 *   - Company branding
 *
 * Usage:
 *   test_overlay_record [host [user [password [output.mp4]]]]
 */

#include "cppdvr_api.h"
#include "test_helpers.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static RecorderHandle g_rec = NULL;

/* Feed overlaid JPEG frames to the recorder */
static void on_overlay_jpeg(const uint8_t* jpeg, size_t size,
                             uint32_t frame_id, void* ud) {
    (void)frame_id; (void)ud;
    if (g_rec) recorder_feed_jpeg(g_rec, jpeg, size);
}

int main(int argc, char* argv[]) {
    const char* host    = (argc >= 2) ? argv[1] : dvr_env("DVR_HOST", DVR_DEFAULT_HOST);
    const char* user    = (argc >= 3) ? argv[2] : dvr_env("DVR_USER", DVR_DEFAULT_USER);
    const char* pass    = (argc >= 4) ? argv[3] : dvr_env("DVR_PASS", DVR_DEFAULT_PASS);
    const char* outfile = (argc >= 5) ? argv[4] : "overlay_record.mp4";
    const int   rec_secs = 5;

    printf("=== Overlay Record Test ===\n");
    INFO("Host: %s  User: %s  Output: %s  Duration: %d s",
         host, user, outfile, rec_secs);

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

    /* Configure overlay: scale=4 (32px glyphs), auto bottom-left position */
    stream_overlay_set_scale(sh, 4);
    stream_overlay_set_cursor(sh, -1, -1);

    /* Set initial overlay text — multi-line, simulating robot HUD */
    stream_overlay_print(sh,
        "12.34 m\n"
        "Zone A / Bay 3\n"
        "AcmeCorp Robotics");
    PASS("overlay text set (scale=4, 3 lines)");

    /* Overlay JPEG callback feeds post-overlay frames to recorder */
    stream_set_overlay_jpeg_callback(sh, on_overlay_jpeg, NULL);

    if (!recorder_init_with_stream(rec, sh)) {
        FAIL("recorder_init_with_stream() failed");
        goto cleanup;
    }
    PASS("recorder_init_with_stream OK");

    if (!recorder_start(rec, outfile, RECORDER_FORMAT_MJPEG, 25)) {
        FAIL("recorder_start() failed");
        goto cleanup;
    }
    PASS("recorder_start OK");

    if (!stream_start(sh, "Main")) {
        FAIL("stream_start() failed");
        goto cleanup;
    }
    PASS("stream_start OK");

    INFO("Recording with overlay for %d s ...", rec_secs);
    for (int i = 0; i < rec_secs; ++i) {
        sleep_ms(1000);

        /* Simulate sensor data arriving each second */
        char buf[STREAM_OVERLAY_MAX_TEXT];
        snprintf(buf, sizeof(buf),
            "%.2f m\n"
            "Zone A / Bay 3\n"
            "AcmeCorp Robotics",
            12.34 + i * 0.5);
        stream_overlay_print(sh, buf);

        INFO("[t+%ds]  frames=%zu  dropped=%zu",
             i + 1,
             recorder_frames_recorded(rec),
             recorder_frames_dropped(rec));
    }

    {
        size_t total = recorder_frames_recorded(rec);
        if (total > 0) PASSF("%zu frames recorded with overlay", total);
        else           FAIL("No frames recorded");
    }

    stream_stop(sh);
    PASS("stream_stop OK");

    recorder_save(rec);
    PASS("recorder_save OK");

    {
        FILE* f = fopen(outfile, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fclose(f);
            INFO("Output: %s  size: %ld bytes (%.1f KB)", outfile, sz, sz / 1024.0);
            if (sz >= 8192) PASS("Output file exists and is reasonably sized");
            else            FAILF("File suspiciously small: %ld bytes", sz);
        } else {
            FAIL("Output file not found");
        }
    }

    {
        char cmd[512];
        snprintf(cmd, sizeof(cmd),
            "ffmpeg -loglevel error -i \"%s\" -frames:v 1 -y overlay_frame0.jpg", outfile);
        INFO("[cmd] %s", cmd);
        system(cmd);

        FILE* f = fopen("overlay_frame0.jpg", "rb");
        if (f) {
            uint8_t hdr[2] = {0, 0};
            fread(hdr, 1, 2, f);
            fclose(f);
            if (hdr[0] == 0xFF && hdr[1] == 0xD8) PASS("Extracted frame is valid JPEG");
            else                                    FAIL("Extracted frame not valid JPEG");
        } else {
            FAIL("Could not extract frame — check overlay_frame0.jpg");
        }
    }

cleanup:
    if (rec) { recorder_deinit(rec); recorder_destroy(rec); }
    if (sh)  { stream_destroy(sh); }
    EXIT_SUMMARY();
}
