/**
 * tests/osd/test_osd_text_record.c
 *
 * Minimal OSD text-to-video proof:
 *   1. Log in and set a visible channel-title overlay ("HELLO CPPDVR").
 *   2. Record 5 seconds of live video to MP4 while the overlay is active.
 *   3. Save the file and verify it is non-empty.
 *   4. Restore the original title.
 *
 * Usage:
 *   test_osd_text_record [host [user [password [output.mp4]]]]
 */

#include "cppdvr_api.h"
#include "test_helpers.h"

static void stream_log(const char* msg, void* ud) { (void)ud; INFO("%s", msg); }
static void rec_log   (const char* msg, void* ud) { (void)ud; INFO("[rec] %s", msg); }

int main(int argc, char* argv[]) {
    const char* host    = (argc >= 2) ? argv[1] : dvr_env("DVR_HOST", DVR_DEFAULT_HOST);
    const char* user    = (argc >= 3) ? argv[2] : dvr_env("DVR_USER", DVR_DEFAULT_USER);
    const char* pass    = (argc >= 4) ? argv[3] : dvr_env("DVR_PASS", DVR_DEFAULT_PASS);
    const char* outfile = (argc >= 5) ? argv[4] : "osd_text_record.mp4";
    const int   rec_secs = 5;

    printf("=== OSD Text Record Test ===\n");
    INFO("Host: %s  User: %s  Output: %s  Duration: %d s",
         host, user, outfile, rec_secs);

    /* ── Login ─────────────────────────────────────────────────────────────── */
    DVRHandle dh = dvr_create(host, DVR_DEFAULT_PORT, user, pass);
    if (!dh || !dvr_login(dh)) {
        FAIL("Login failed");
        dvr_destroy(dh);
        EXIT_SUMMARY();
    }
    PASS("Login OK");

    /* ── Set text overlay ───────────────────────────────────────────────────── */
    const char* titles[] = { "HELLO CPPDVR" };
    if (dvr_set_channel_titles(dh, titles, 1)) {
        PASS("OSD text set: 'HELLO CPPDVR'");
    } else {
        FAIL("dvr_set_channel_titles() failed");
        char err[256]; dvr_last_error(dh, err, sizeof(err));
        if (err[0]) INFO("error: %s", err);
    }
    sleep_ms(400);   /* let overlay propagate before recording starts */

    /* ── Create stream + recorder ───────────────────────────────────────────── */
    StreamHandle   sh  = stream_create(host, DVR_DEFAULT_PORT, user, pass, 0);
    RecorderHandle rec = recorder_create();
    if (!sh || !rec) {
        FAIL("stream_create or recorder_create returned NULL");
        goto cleanup_dvr;
    }
    PASS("stream_create + recorder_create OK");

    stream_set_log_callback(sh,  stream_log, NULL);
    recorder_set_log_callback(rec, rec_log,  NULL);

    if (!recorder_init_with_stream(rec, sh)) {
        FAIL("recorder_init_with_stream() failed");
        goto cleanup_all;
    }

    /* ── Start recorder then stream ─────────────────────────────────────────── */
    if (!recorder_start(rec, outfile, RECORDER_FORMAT_MP4, 25)) {
        FAIL("recorder_start() failed");
        goto cleanup_all;
    }
    PASS("recorder_start OK");

    if (!stream_start(sh, "Main")) {
        FAIL("stream_start() failed");
        goto cleanup_all;
    }
    PASS("stream_start OK");

    /* ── Record for rec_secs ────────────────────────────────────────────────── */
    INFO("Recording 'HELLO CPPDVR' overlay for %d s ...", rec_secs);
    for (int i = 0; i < rec_secs; ++i) {
        sleep_ms(1000);
        INFO("[t+%ds]  frames=%zu  dropped=%zu",
             i + 1,
             recorder_frames_recorded(rec),
             recorder_frames_dropped(rec));
    }

    size_t total = recorder_frames_recorded(rec);
    if (total > 0)
        PASSF("%zu frames written", total);
    else
        FAIL("No frames written — stream may not have connected");

    /* ── Save ───────────────────────────────────────────────────────────────── */
    recorder_save(rec);
    PASS("recorder_save completed");

    stream_stop(sh);
    PASS("stream_stop completed");

    /* ── Quick file check ───────────────────────────────────────────────────── */
    FILE* f = fopen(outfile, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fclose(f);
        INFO("Output: %s  size: %ld bytes (%.1f KB)", outfile, sz, sz / 1024.0);
        if (sz >= 8192)
            PASS("MP4 file exists and is reasonably sized");
        else
            FAILF("File suspiciously small: %ld bytes", sz);
    } else {
        FAIL("Output file not found");
    }

    /* ── Restore original title ─────────────────────────────────────────────── */
    const char* blank[] = { "" };
    if (dvr_set_channel_titles(dh, blank, 1))
        PASS("Channel title cleared");
    else
        FAIL("Could not clear channel title");

cleanup_all:
    if (rec) { recorder_deinit(rec); recorder_destroy(rec); }
    if (sh)  { stream_destroy(sh); }
cleanup_dvr:
    dvr_destroy(dh);
    EXIT_SUMMARY();
}
