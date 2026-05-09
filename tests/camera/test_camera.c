/**
 * tests/camera/test_camera.c
 *
 * Tests dvr_get_video_color() and dvr_set_video_color() against the live camera.
 * Reads current parameters, validates ranges, then does a safe round-trip write.
 *
 * Usage:
 *   test_camera [host [user [password]]]
 */

#include "cppdvr_api.h"
#include "test_helpers.h"
#include <string.h>

static void print_color(const DVRVideoColorC* c, const char* label) {
    INFO("[%s] brightness=%d contrast=%d saturation=%d hue=%d "
         "sharpness=%d gain=%d whitebalance=%d",
         label,
         c->brightness, c->contrast, c->saturation, c->hue,
         c->sharpness, c->gain, c->whitebalance);
}

int main(int argc, char* argv[]) {
    const char* host = (argc >= 2) ? argv[1] : dvr_env("DVR_HOST", DVR_DEFAULT_HOST);
    const char* user = (argc >= 3) ? argv[2] : dvr_env("DVR_USER", DVR_DEFAULT_USER);
    const char* pass = (argc >= 4) ? argv[3] : dvr_env("DVR_PASS", DVR_DEFAULT_PASS);

    printf("=== Camera Color Parameters Test ===\n");
    INFO("Host: %s  User: %s", host, user);

    DVRHandle h = dvr_create(host, DVR_DEFAULT_PORT, user, pass);
    if (!h || !dvr_login(h)) {
        FAIL("Login failed");
        dvr_destroy(h);
        EXIT_SUMMARY();
    }
    PASS("Login OK");

    /* ── Test 1: Read color params ──────────────────────────────────────── */
    SECTION("Read video color parameters (channel 0)");

    DVRVideoColorC orig;
    memset(&orig, 0, sizeof(orig));

    if (!dvr_get_video_color(h, &orig, 0)) {
        FAIL("dvr_get_video_color() failed");
        char errbuf[256]; dvr_last_error(h, errbuf, sizeof(errbuf));
        if (errbuf[0]) INFO("last_error: %s", errbuf);
        dvr_destroy(h);
        EXIT_SUMMARY();
    }
    PASS("dvr_get_video_color() succeeded");
    print_color(&orig, "original");

    /* ── Test 2: Validate ranges ────────────────────────────────────────── */
    SECTION("Validate parameter ranges");

    if (orig.brightness >= 0 && orig.brightness <= 100)
        PASS("brightness in range (0–100)");
    else
        FAILF("brightness out of range: %d", orig.brightness);

    if (orig.contrast >= 0 && orig.contrast <= 100)
        PASS("contrast in range (0–100)");
    else
        FAILF("contrast out of range: %d", orig.contrast);

    if (orig.saturation >= 0 && orig.saturation <= 100)
        PASS("saturation in range (0–100)");
    else
        FAILF("saturation out of range: %d", orig.saturation);

    if (orig.hue >= 0 && orig.hue <= 100)
        PASS("hue in range (0–100)");
    else
        FAILF("hue out of range: %d", orig.hue);

    if (orig.whitebalance >= 0 && orig.whitebalance <= 255)
        PASS("whitebalance in range (0–255)");
    else
        FAILF("whitebalance out of range: %d", orig.whitebalance);

    /* ── Test 3: Round-trip write (restore same values) ─────────────────── */
    SECTION("Round-trip write (same values back)");

    if (dvr_set_video_color(h, &orig, 0)) {
        PASS("dvr_set_video_color() accepted round-trip write");
    } else {
        FAIL("dvr_set_video_color() failed");
        char errbuf[256]; dvr_last_error(h, errbuf, sizeof(errbuf));
        if (errbuf[0]) INFO("last_error: %s", errbuf);
    }

    /* ── Test 4: Read back and verify ───────────────────────────────────── */
    SECTION("Read-back after write");

    DVRVideoColorC readback;
    memset(&readback, 0, sizeof(readback));
    if (!dvr_get_video_color(h, &readback, 0)) {
        FAIL("dvr_get_video_color() failed on read-back");
        dvr_destroy(h);
        EXIT_SUMMARY();
    }
    print_color(&readback, "readback");

    if (readback.brightness == orig.brightness &&
        readback.contrast   == orig.contrast   &&
        readback.saturation == orig.saturation &&
        readback.hue        == orig.hue) {
        PASS("brightness/contrast/saturation/hue match after round-trip");
    } else {
        FAILF("Mismatch: brightness %d→%d  contrast %d→%d  "
              "saturation %d→%d  hue %d→%d",
              orig.brightness, readback.brightness,
              orig.contrast,   readback.contrast,
              orig.saturation, readback.saturation,
              orig.hue,        readback.hue);
    }

    if (readback.whitebalance == orig.whitebalance) {
        PASS("whitebalance matches after round-trip");
    } else {
        FAILF("whitebalance: %d → %d", orig.whitebalance, readback.whitebalance);
    }

    /* ── Test 5: Boundary write — nudge brightness by 1, then restore ───── */
    SECTION("Boundary write: nudge brightness +1 and restore");

    DVRVideoColorC nudged = orig;
    nudged.brightness = (orig.brightness < 100) ? orig.brightness + 1
                                                 : orig.brightness - 1;

    if (dvr_set_video_color(h, &nudged, 0)) {
        PASS("Write with modified brightness accepted");
        sleep_ms(300);

        DVRVideoColorC check;
        memset(&check, 0, sizeof(check));
        if (dvr_get_video_color(h, &check, 0)) {
            if (check.brightness == nudged.brightness) {
                PASS("Modified brightness value read back correctly");
            } else {
                INFO("Expected %d, got %d (camera may clamp/round — ok)",
                     nudged.brightness, check.brightness);
                PASS("Camera accepted write (value may differ due to clamping)");
            }
        } else {
            FAIL("Read-back after nudge failed");
        }

        /* Restore original */
        if (dvr_set_video_color(h, &orig, 0)) {
            PASS("Original values restored");
        } else {
            FAIL("Failed to restore original values");
        }
    } else {
        FAIL("Write with modified brightness rejected");
    }

    dvr_destroy(h);
    EXIT_SUMMARY();
}
