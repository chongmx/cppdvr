/**
 * tests/osd/test_dual_record.c
 *
 * Records two simultaneous streams from the same camera:
 *
 *   raw_record.mp4     — lossless H.265 raw copy (no overlay)
 *   overlay_record.mp4 — H.264 re-encode with HUD overlay burned in
 *
 * HUD layout (top-right corner, right-aligned, scale=4 / 32px glyphs):
 *   Box 0 — Company name:  "CGS Venture Inc."
 *   Box 1 — Distance:      "12.34 m"          (updated each second)
 *   Box 2 — Address:       "Bandar Puteri, Puchong, Selangor"
 *                           (word-wraps within box_w if needed)
 *
 * Usage:
 *   test_dual_record [host [user [password]]]
 */

#include "cppdvr_api.h"
#include "test_helpers.h"

#include <stdio.h>

/* ── box layout constants ─────────────────────────────────────────────────── */
#define HUD_SCALE    4       /* 32 px glyphs                                  */
#define HUD_LINE_H  32       /* 8 * HUD_SCALE                                 */
#define HUD_BOX_W  700       /* wide enough for the address line              */
#define HUD_PAD_X   16       /* right padding from frame edge                 */
#define HUD_PAD_Y   16       /* top padding from frame edge                   */

/* Box y positions (stacked from top, with a gap between rows) */
#define BOX0_Y   HUD_PAD_Y
#define BOX1_Y  (BOX0_Y + HUD_LINE_H + 8)
#define BOX2_Y  (BOX1_Y + HUD_LINE_H + 8)

static RecorderHandle g_rec_overlay = NULL;

/* Feed post-overlay frames to the recorder; JPEG callback stays free for GUI */
static void on_overlay_jpeg(const uint8_t* jpeg, size_t size,
                             uint32_t frame_id, void* ud) {
    (void)frame_id; (void)ud;
    if (g_rec_overlay) recorder_feed_jpeg(g_rec_overlay, jpeg, size);
}

static void stream_log(const char* msg, void* ud) { (void)ud; INFO("%s", msg); }

static void check_file(const char* path, const char* label) {
    FILE* f = fopen(path, "rb");
    if (!f) { FAILF("%s: file not found", label); return; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);
    INFO("%s: %s  %ld bytes (%.1f KB)", label, path, sz, sz / 1024.0);
    if (sz >= 8192) PASSF("%s is reasonably sized", label);
    else            FAILF("%s suspiciously small: %ld bytes", label, sz);
}

int main(int argc, char* argv[]) {
    const char* host        = (argc >= 2) ? argv[1] : dvr_env("DVR_HOST", DVR_DEFAULT_HOST);
    const char* user        = (argc >= 3) ? argv[2] : dvr_env("DVR_USER", DVR_DEFAULT_USER);
    const char* pass        = (argc >= 4) ? argv[3] : dvr_env("DVR_PASS", DVR_DEFAULT_PASS);
    const char* raw_file    = "raw_record.mp4";
    const char* overlay_file = "overlay_record.mp4";
    const int   rec_secs    = 5;

    printf("=== Dual Recording Test ===\n");
    INFO("Host: %s  User: %s  Duration: %d s", host, user, rec_secs);
    INFO("  raw_record.mp4     — H.265 raw copy, no overlay");
    INFO("  overlay_record.mp4 — H.264 re-encode, HUD overlay top-right");

    StreamHandle   sh          = stream_create(host, DVR_DEFAULT_PORT, user, pass, 0);
    RecorderHandle rec_raw     = recorder_create();
    RecorderHandle rec_overlay = recorder_create();

    if (!sh || !rec_raw || !rec_overlay) {
        FAIL("Failed to create stream/recorders");
        goto cleanup;
    }
    PASS("stream_create + 2x recorder_create OK");

    stream_set_log_callback(sh, stream_log, NULL);

    /* ── Configure HUD overlay boxes ────────────────────────────────────────── */
    /* Box 0 — company name (top line) */
    stream_overlay_box_configure(sh,
        0,
        HUD_PAD_X, BOX0_Y,
        HUD_BOX_W,
        HUD_SCALE,
        STREAM_OVERLAY_ALIGN_RIGHT,
        STREAM_OVERLAY_ANCHOR_TOP_RIGHT);
    stream_overlay_box_print(sh, 0, "CGS Venture Inc.");

    /* Box 1 — live distance reading (updated each second) */
    stream_overlay_box_configure(sh,
        1,
        HUD_PAD_X, BOX1_Y,
        HUD_BOX_W,
        HUD_SCALE,
        STREAM_OVERLAY_ALIGN_RIGHT,
        STREAM_OVERLAY_ANCHOR_TOP_RIGHT);
    stream_overlay_box_print(sh, 1, "0.00 m");

    /* Box 2 — address (may word-wrap within HUD_BOX_W) */
    stream_overlay_box_configure(sh,
        2,
        HUD_PAD_X, BOX2_Y,
        HUD_BOX_W,
        HUD_SCALE,
        STREAM_OVERLAY_ALIGN_RIGHT,
        STREAM_OVERLAY_ANCHOR_TOP_RIGHT);
    stream_overlay_box_print(sh, 2, "Bandar Puteri, Puchong, Selangor");

    PASS("HUD boxes configured (top-right, right-aligned, scale=4)");

    /* Overlay JPEG callback → rec_overlay; regular JPEG callback free for GUI */
    g_rec_overlay = rec_overlay;
    stream_set_overlay_jpeg_callback(sh, on_overlay_jpeg, NULL);

    if (!recorder_init_with_stream(rec_raw, sh)) {
        FAIL("recorder_init_with_stream(rec_raw) failed");
        goto cleanup;
    }
    if (!recorder_init_standalone(rec_overlay)) {
        FAIL("recorder_init_standalone(rec_overlay) failed");
        goto cleanup;
    }
    PASS("recorder init OK (raw=with_stream  overlay=standalone)");

    if (!recorder_start(rec_raw, raw_file, RECORDER_FORMAT_MP4, 25)) {
        FAIL("recorder_start(rec_raw) failed");
        goto cleanup;
    }
    PASS("rec_raw started -> raw_record.mp4 (H.265 copy)");

    if (!recorder_start(rec_overlay, overlay_file, RECORDER_FORMAT_MJPEG, 25)) {
        FAIL("recorder_start(rec_overlay) failed");
        goto cleanup;
    }
    PASS("rec_overlay started -> overlay_record.mp4 (H.264 + HUD)");

    if (!stream_start(sh, "Main")) {
        FAIL("stream_start() failed");
        goto cleanup;
    }
    PASS("stream_start OK");

    INFO("Recording both streams for %d s ...", rec_secs);
    for (int i = 0; i < rec_secs; ++i) {
        sleep_ms(1000);

        /* Simulate distance sensor update (push-based, any thread, any rate) */
        char dist[64];
        snprintf(dist, sizeof(dist), "%.2f m", (double)i * 0.75);
        stream_overlay_box_print(sh, 1, dist);

        INFO("[t+%ds]  raw=%zu frames   overlay=%zu frames",
             i + 1,
             recorder_frames_recorded(rec_raw),
             recorder_frames_recorded(rec_overlay));
    }

    {
        size_t raw_n     = recorder_frames_recorded(rec_raw);
        size_t overlay_n = recorder_frames_recorded(rec_overlay);
        if (raw_n > 0)     PASSF("raw:     %zu frames recorded", raw_n);
        else               FAIL("raw:     no frames recorded");
        if (overlay_n > 0) PASSF("overlay: %zu frames recorded", overlay_n);
        else               FAIL("overlay: no frames recorded");
    }

    stream_stop(sh);
    PASS("stream_stop OK");

    recorder_save(rec_raw);    PASS("rec_raw saved");
    recorder_save(rec_overlay); PASS("rec_overlay saved");

    SECTION("Verify output files");
    check_file(raw_file,     "raw_record.mp4    ");
    check_file(overlay_file, "overlay_record.mp4");

    SECTION("Extract first frame from each recording");
    {
        char cmd[512];
        snprintf(cmd, sizeof(cmd),
            "ffmpeg -loglevel error -i \"%s\" -frames:v 1 -y dual_raw_frame0.jpg",
            raw_file);
        INFO("[cmd] %s", cmd);
        system(cmd);

        snprintf(cmd, sizeof(cmd),
            "ffmpeg -loglevel error -i \"%s\" -frames:v 1 -y dual_overlay_frame0.jpg",
            overlay_file);
        INFO("[cmd] %s", cmd);
        system(cmd);

        FILE* f;
        uint8_t hdr[2];

        f = fopen("dual_raw_frame0.jpg", "rb");
        if (f) {
            fread(hdr, 1, 2, f); fclose(f);
            if (hdr[0] == 0xFF && hdr[1] == 0xD8) PASS("dual_raw_frame0.jpg is valid JPEG");
            else                                    FAIL("dual_raw_frame0.jpg not a valid JPEG");
        } else FAIL("dual_raw_frame0.jpg not found");

        f = fopen("dual_overlay_frame0.jpg", "rb");
        if (f) {
            fread(hdr, 1, 2, f); fclose(f);
            if (hdr[0] == 0xFF && hdr[1] == 0xD8) PASS("dual_overlay_frame0.jpg is valid JPEG");
            else                                    FAIL("dual_overlay_frame0.jpg not a valid JPEG");
        } else FAIL("dual_overlay_frame0.jpg not found");
    }

cleanup:
    if (rec_raw)     { recorder_deinit(rec_raw);     recorder_destroy(rec_raw); }
    if (rec_overlay) { recorder_deinit(rec_overlay); recorder_destroy(rec_overlay); }
    if (sh)          { stream_destroy(sh); }
    EXIT_SUMMARY();
}
