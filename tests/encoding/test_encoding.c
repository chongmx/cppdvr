/**
 * tests/encoding/test_encoding.c
 *
 * Tests dvr_get_encode_config() and dvr_set_encode_config() against
 * the live camera.  Reads current settings, validates fields, then
 * performs a safe round-trip write (same values back).
 *
 * Usage:
 *   test_encoding [host [user [password]]]
 */

#include "cppdvr_api.h"
#include "test_helpers.h"
#include <string.h>

static const char* known_codecs[] = { "H.264", "H.265", "MPEG4", NULL };
static const char* known_res[]    = {
    "5M","4M","3M","2M","1080P","720P","D1","HD1","CIF","QCIF", NULL
};
static const char* known_brc[]    = { "VBR", "CBR", NULL };

static int in_list(const char* val, const char** list) {
    for (int i = 0; list[i]; ++i)
        if (strcmp(val, list[i]) == 0) return 1;
    return 0;
}

static void check_stream(const DVRVideoStreamFormatC* f, const char* label) {
    INFO("[%s] codec=%-6s res=%-6s brc=%-3s bitrate=%d fps=%d gop=%d quality=%d video=%d audio=%d",
         label,
         f->compression, f->resolution, f->bitrate_ctrl,
         f->bitrate, f->fps, f->gop, f->quality,
         f->video_enable, f->audio_enable);

    if (in_list(f->compression, known_codecs)) {
        PASSF("[%s] codec is a known value (%s)", label, f->compression);
    } else {
        FAILF("[%s] unknown codec: '%s'", label, f->compression);
    }

    if (f->resolution[0] != '\0') {
        if (in_list(f->resolution, known_res)) {
            PASSF("[%s] resolution is a known value (%s)", label, f->resolution);
        } else {
            INFO("[%s] resolution '%s' not in known list — camera-specific (ok)", label, f->resolution);
            PASSF("[%s] resolution field is non-empty", label);
        }
    } else {
        FAILF("[%s] resolution field is empty", label);
    }

    if (in_list(f->bitrate_ctrl, known_brc)) {
        PASSF("[%s] bitrate control is valid (%s)", label, f->bitrate_ctrl);
    } else {
        FAILF("[%s] unexpected bitrate_ctrl: '%s'", label, f->bitrate_ctrl);
    }

    if (f->bitrate > 0 && f->bitrate <= 65536) {
        PASSF("[%s] bitrate is in range (%d kbps)", label, f->bitrate);
    } else {
        FAILF("[%s] bitrate out of range: %d", label, f->bitrate);
    }

    if (f->fps >= 1 && f->fps <= 120) {
        PASSF("[%s] FPS is valid (%d)", label, f->fps);
    } else {
        FAILF("[%s] FPS out of range: %d", label, f->fps);
    }

    if (f->gop >= 1 && f->gop <= 10) {
        PASSF("[%s] GOP is in range (%d)", label, f->gop);
    } else {
        FAILF("[%s] GOP out of range: %d", label, f->gop);
    }

    if (f->quality >= 1 && f->quality <= 6) {
        PASSF("[%s] quality is in range (%d)", label, f->quality);
    } else {
        FAILF("[%s] quality out of range: %d", label, f->quality);
    }
}

int main(int argc, char* argv[]) {
    const char* host = (argc >= 2) ? argv[1] : dvr_env("DVR_HOST", DVR_DEFAULT_HOST);
    const char* user = (argc >= 3) ? argv[2] : dvr_env("DVR_USER", DVR_DEFAULT_USER);
    const char* pass = (argc >= 4) ? argv[3] : dvr_env("DVR_PASS", DVR_DEFAULT_PASS);

    printf("=== Encoding Settings Test ===\n");
    INFO("Host: %s  User: %s", host, user);

    DVRHandle h = dvr_create(host, DVR_DEFAULT_PORT, user, pass);
    if (!h || !dvr_login(h)) {
        FAIL("Login failed");
        dvr_destroy(h);
        EXIT_SUMMARY();
    }
    PASS("Login OK");

    /* ── Test 1: Read encode config ─────────────────────────────────────── */
    SECTION("Read encode config (channel 0)");

    DVREncodeConfigC cfg;
    memset(&cfg, 0, sizeof(cfg));

    if (!dvr_get_encode_config(h, &cfg, 0)) {
        FAIL("dvr_get_encode_config() failed");
        char errbuf[256]; dvr_last_error(h, errbuf, sizeof(errbuf));
        if (errbuf[0]) INFO("last_error: %s", errbuf);
        dvr_destroy(h);
        EXIT_SUMMARY();
    }
    PASS("dvr_get_encode_config() succeeded");

    /* ── Test 2: Validate main stream ───────────────────────────────────── */
    SECTION("Validate main stream fields");
    check_stream(&cfg.main, "Main");

    /* ── Test 3: Validate extra stream ──────────────────────────────────── */
    SECTION("Validate extra stream fields");
    check_stream(&cfg.extra, "Extra");

    /* ── Test 4: Round-trip write ───────────────────────────────────────── */
    SECTION("Round-trip write (same values back)");

    if (dvr_set_encode_config(h, &cfg, 0)) {
        PASS("dvr_set_encode_config() accepted round-trip write");
    } else {
        FAIL("dvr_set_encode_config() failed");
        char errbuf[256]; dvr_last_error(h, errbuf, sizeof(errbuf));
        if (errbuf[0]) INFO("last_error: %s", errbuf);
    }

    /* ── Test 5: Read back and compare codec/resolution ─────────────────── */
    SECTION("Read-back after write");

    DVREncodeConfigC cfg2;
    memset(&cfg2, 0, sizeof(cfg2));
    if (dvr_get_encode_config(h, &cfg2, 0)) {
        if (strcmp(cfg.main.compression, cfg2.main.compression) == 0 &&
            strcmp(cfg.main.resolution,  cfg2.main.resolution)  == 0) {
            PASS("Main stream codec/resolution unchanged after round-trip");
        } else {
            FAILF("Main stream changed: was %s/%s, now %s/%s",
                  cfg.main.compression, cfg.main.resolution,
                  cfg2.main.compression, cfg2.main.resolution);
        }
        if (strcmp(cfg.extra.compression, cfg2.extra.compression) == 0 &&
            strcmp(cfg.extra.resolution,  cfg2.extra.resolution)  == 0) {
            PASS("Extra stream codec/resolution unchanged after round-trip");
        } else {
            FAILF("Extra stream changed: was %s/%s, now %s/%s",
                  cfg.extra.compression, cfg.extra.resolution,
                  cfg2.extra.compression, cfg2.extra.resolution);
        }
    } else {
        FAIL("dvr_get_encode_config() failed on read-back");
    }

    dvr_destroy(h);
    EXIT_SUMMARY();
}
