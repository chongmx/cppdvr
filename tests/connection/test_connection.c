/**
 * tests/connection/test_connection.c
 *
 * Tests DVR connection lifecycle: create, login, reconnect, bad credentials.
 *
 * Usage:
 *   test_connection [host [user [password]]]
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

    printf("=== Connection Test ===\n");
    INFO("Host: %s  User: %s", host, user);

    /* ── Test 1: Basic login ────────────────────────────────────────────── */
    SECTION("Basic login");

    DVRHandle h = dvr_create(host, DVR_DEFAULT_PORT, user, pass);
    if (!h) {
        FAIL("dvr_create() returned NULL");
        EXIT_SUMMARY();
    }
    PASS("dvr_create() succeeded");

    int logged_in = dvr_login(h);
    if (logged_in) {
        PASS("dvr_login() succeeded");
    } else {
        FAIL("dvr_login() failed");
        dvr_destroy(h);
        EXIT_SUMMARY();
    }

    /* ── Test 2: Close and re-login ─────────────────────────────────────── */
    SECTION("Close and re-login");

    dvr_close(h);
    PASS("dvr_close() did not crash");

    int relogin = dvr_login(h);
    if (relogin) {
        PASS("Re-login after close succeeded");
    } else {
        FAIL("Re-login after close failed");
    }

    dvr_destroy(h);
    PASS("dvr_destroy() did not crash");

    /* ── Test 3: Snapshot to confirm session is valid ───────────────────── */
    SECTION("Session validity via snapshot");

    h = dvr_create(host, DVR_DEFAULT_PORT, user, pass);
    if (dvr_login(h)) {
        size_t sz = 0;
        uint8_t* jpeg = dvr_snapshot(h, &sz, 0);
        if (jpeg && sz > 0) {
            INFO("Snapshot returned %zu bytes", sz);
            if (jpeg[0] == 0xFF && jpeg[1] == 0xD8) {
                PASS("Snapshot starts with JPEG magic (FF D8)");
            } else {
                FAILF("Snapshot does not start with FF D8: got %02X %02X", jpeg[0], jpeg[1]);
            }
            dvr_free_buffer(jpeg);
        } else {
            FAIL("Snapshot returned empty buffer");
        }
    } else {
        FAIL("Login failed for snapshot test");
    }
    dvr_destroy(h);

    /* ── Test 4: Wrong password ──────────────────────────────────────────── */
    SECTION("Wrong password (expected login failure)");

    DVRHandle hbad = dvr_create(host, DVR_DEFAULT_PORT, user, "wrong_password_xyz_123");
    if (!hbad) {
        FAIL("dvr_create() returned NULL for bad-password test");
    } else {
        int bad_login = dvr_login(hbad);
        if (!bad_login) {
            PASS("Login correctly rejected with wrong password");
        } else {
            FAIL("Login accepted wrong password (camera may have no auth configured)");
        }
        dvr_destroy(hbad);
    }

    /* ── Test 5: Wrong host (should fail quickly on timeout) ────────────── */
    SECTION("Unreachable host (expected timeout/failure)");

    DVRHandle hoff = dvr_create("192.0.2.1" /* TEST-NET, routable but unresponsive */,
                                DVR_DEFAULT_PORT, user, pass);
    if (!hoff) {
        FAIL("dvr_create() returned NULL for unreachable-host test");
    } else {
        int off_login = dvr_login(hoff);
        if (!off_login) {
            PASS("Login correctly failed for unreachable host");
        } else {
            FAIL("Login succeeded for supposedly unreachable host");
        }
        dvr_destroy(hoff);
    }

    EXIT_SUMMARY();
}
