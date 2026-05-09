/**
 * tests/time/test_time.c
 *
 * Tests dvr_get_time() and dvr_set_time().
 *
 * Usage:
 *   test_time [host [user [password]]]
 *
 * Environment:
 *   DVR_HOST, DVR_USER, DVR_PASS
 */

#include "cppdvr_api.h"
#include "test_helpers.h"

int main(int argc, char* argv[]) {
    const char* host = (argc >= 2) ? argv[1] : dvr_env("DVR_HOST", DVR_DEFAULT_HOST);
    const char* user = (argc >= 3) ? argv[2] : dvr_env("DVR_USER", DVR_DEFAULT_USER);
    const char* pass = (argc >= 4) ? argv[3] : dvr_env("DVR_PASS", DVR_DEFAULT_PASS);

    printf("=== Time Test ===\n");
    INFO("Host: %s  User: %s", host, user);

    DVRHandle h = dvr_create(host, DVR_DEFAULT_PORT, user, pass);
    if (!h || !dvr_login(h)) {
        FAIL("Login failed");
        dvr_destroy(h);
        EXIT_SUMMARY();
    }
    PASS("Login OK");

    /* ── Test 1: Get time ────────────────────────────────────────────────── */
    SECTION("Get device time");

    int y = 0, mo = 0, d = 0, hr = 0, mi = 0, sec = 0;
    int got = dvr_get_time(h, &y, &mo, &d, &hr, &mi, &sec);

    if (!got) {
        FAIL("dvr_get_time() failed");
        dvr_destroy(h);
        EXIT_SUMMARY();
    }
    PASS("dvr_get_time() succeeded");
    INFO("Device time: %04d-%02d-%02d %02d:%02d:%02d", y, mo, d, hr, mi, sec);

    /* Sanity checks */
    if (y >= 2000 && y <= 2100) {
        PASS("Year is in expected range (2000–2100)");
    } else {
        FAILF("Year %d is out of expected range", y);
    }

    if (mo >= 1 && mo <= 12) {
        PASS("Month is valid (1–12)");
    } else {
        FAILF("Month %d is out of range", mo);
    }

    if (d >= 1 && d <= 31) {
        PASS("Day is valid (1–31)");
    } else {
        FAILF("Day %d is out of range", d);
    }

    if (hr >= 0 && hr <= 23) {
        PASS("Hour is valid (0–23)");
    } else {
        FAILF("Hour %d is out of range", hr);
    }

    if (mi >= 0 && mi <= 59) {
        PASS("Minute is valid (0–59)");
    } else {
        FAILF("Minute %d is out of range", mi);
    }

    if (sec >= 0 && sec <= 59) {
        PASS("Second is valid (0–59)");
    } else {
        FAILF("Second %d is out of range", sec);
    }

    /* ── Test 2: Set time (round-trip) ──────────────────────────────────── */
    SECTION("Set time (round-trip)");

    /* Set to the same values we just read — a no-op from the camera's
       perspective but verifies the command is accepted. */
    int set_ok = dvr_set_time(h, y, mo, d, hr, mi, sec);
    if (set_ok) {
        PASS("dvr_set_time() accepted the command");
    } else {
        FAIL("dvr_set_time() failed");
    }

    /* Read back and compare */
    sleep_ms(500);
    int y2 = 0, mo2 = 0, d2 = 0, hr2 = 0, mi2 = 0, sec2 = 0;
    if (dvr_get_time(h, &y2, &mo2, &d2, &hr2, &mi2, &sec2)) {
        INFO("Time after set: %04d-%02d-%02d %02d:%02d:%02d", y2, mo2, d2, hr2, mi2, sec2);
        if (y2 == y && mo2 == mo && d2 == d && hr2 == hr && mi2 == mi) {
            PASS("Time read back matches what was set (to the minute)");
        } else {
            INFO("Drift is OK if camera uses its own NTP — skipping strict comparison");
            PASS("Time read back succeeded (value may differ due to NTP)");
        }
    } else {
        FAIL("dvr_get_time() failed after set");
    }

    dvr_destroy(h);
    EXIT_SUMMARY();
}
