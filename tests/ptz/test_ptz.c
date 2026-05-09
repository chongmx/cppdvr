/**
 * tests/ptz/test_ptz.c
 *
 * Sends each PTZ command and verifies the camera accepts it (return code 100/515).
 * The test does NOT verify physical movement — only that the protocol exchange
 * succeeds.  The camera should move briefly between each command if connected.
 *
 * Usage:
 *   test_ptz [host [user [password [channel]]]]
 *
 * Environment:
 *   DVR_HOST, DVR_USER, DVR_PASS
 */

#include "cppdvr_api.h"
#include "test_helpers.h"

static void test_cmd(DVRHandle h, const char* cmd, int step, int preset, int channel) {
    int ok = dvr_ptz(h, cmd, step, preset, channel);
    if (ok) {
        PASS(cmd);
    } else {
        FAILF("dvr_ptz(\"%s\") failed", cmd);
    }
    sleep_ms(300);
}

int main(int argc, char* argv[]) {
    const char* host = (argc >= 2) ? argv[1] : dvr_env("DVR_HOST", DVR_DEFAULT_HOST);
    const char* user = (argc >= 3) ? argv[2] : dvr_env("DVR_USER", DVR_DEFAULT_USER);
    const char* pass = (argc >= 4) ? argv[3] : dvr_env("DVR_PASS", DVR_DEFAULT_PASS);
    int channel      = (argc >= 5) ? atoi(argv[4]) : 0;

    printf("=== PTZ Test ===\n");
    INFO("Host: %s  User: %s  Channel: %d", host, user, channel);
    INFO("NOTE: Camera may move. Keep clear of any pan/tilt limits.");

    DVRHandle h = dvr_create(host, DVR_DEFAULT_PORT, user, pass);
    if (!h || !dvr_login(h)) {
        FAIL("Login failed");
        dvr_destroy(h);
        EXIT_SUMMARY();
    }
    PASS("Login OK");

    /* ── Cardinal directions ────────────────────────────────────────────── */
    SECTION("Cardinal directions");
    test_cmd(h, "DirectionUp",    3, -1, channel);
    test_cmd(h, "DirectionDown",  3, -1, channel);
    test_cmd(h, "DirectionLeft",  3, -1, channel);
    test_cmd(h, "DirectionRight", 3, -1, channel);

    /* ── Diagonal directions ────────────────────────────────────────────── */
    SECTION("Diagonal directions");
    test_cmd(h, "DirectionLeftUp",    3, -1, channel);
    test_cmd(h, "DirectionRightUp",   3, -1, channel);
    test_cmd(h, "DirectionLeftDown",  3, -1, channel);
    test_cmd(h, "DirectionRightDown", 3, -1, channel);

    /* ── Zoom ───────────────────────────────────────────────────────────── */
    SECTION("Zoom");
    test_cmd(h, "ZoomTile", 3, -1, channel);
    test_cmd(h, "ZoomWide", 3, -1, channel);

    /* ── Focus ──────────────────────────────────────────────────────────── */
    SECTION("Focus");
    test_cmd(h, "FocusNear", 2, -1, channel);
    test_cmd(h, "FocusFar",  2, -1, channel);

    /* ── Iris ───────────────────────────────────────────────────────────── */
    SECTION("Iris");
    test_cmd(h, "IrisSmall", 2, -1, channel);
    test_cmd(h, "IrisLarge", 2, -1, channel);

    /* ── Presets ────────────────────────────────────────────────────────── */
    SECTION("Presets");
    test_cmd(h, "SetPreset",  1, 1, channel);
    sleep_ms(200);
    test_cmd(h, "GotoPreset", 1, 1, channel);
    sleep_ms(500);
    test_cmd(h, "ClearPreset", 1, 1, channel);

    /* ── Tour ───────────────────────────────────────────────────────────── */
    SECTION("Tour");
    test_cmd(h, "StartTour", 1, -1, channel);
    sleep_ms(1000);
    test_cmd(h, "StopTour",  1, -1, channel);

    dvr_destroy(h);
    EXIT_SUMMARY();
}
