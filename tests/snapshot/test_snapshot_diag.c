/**
 * tests/snapshot/test_snapshot_diag.c
 *
 * Diagnostic: tries get_info("OPSNAP") and dvr_snapshot() with verbose output.
 * Useful for understanding the camera's exact OPSNAP response format.
 *
 * Usage:
 *   test_snapshot_diag [host [user [password]]]
 */

#include "cppdvr_api.h"
#include "test_helpers.h"
#include <string.h>

int main(int argc, char* argv[]) {
    const char* host = (argc >= 2) ? argv[1] : dvr_env("DVR_HOST", DVR_DEFAULT_HOST);
    const char* user = (argc >= 3) ? argv[2] : dvr_env("DVR_USER", DVR_DEFAULT_USER);
    const char* pass = (argc >= 4) ? argv[3] : dvr_env("DVR_PASS", DVR_DEFAULT_PASS);

    printf("=== Snapshot Diagnostic ===\n");
    INFO("Host: %s  User: %s", host, user);

    DVRHandle h = dvr_create(host, DVR_DEFAULT_PORT, user, pass);
    if (!h || !dvr_login(h)) {
        FAIL("Login failed");
        dvr_destroy(h);
        EXIT_SUMMARY();
    }
    PASS("Login OK");

    /* ── Probe 1: get_info("OPSNAP") ─────────────────────────────────── */
    SECTION("get_info(\"OPSNAP\")");

    char* info = dvr_get_info(h, "OPSNAP");
    if (info) {
        INFO("Response: %.400s", info);
        dvr_free_string(info);
    } else {
        INFO("get_info returned NULL");
    }

    char errbuf[512];
    dvr_last_error(h, errbuf, sizeof(errbuf));
    if (errbuf[0]) INFO("last_error: %s", errbuf);

    /* ── Probe 2: dvr_snapshot() ─────────────────────────────────────── */
    SECTION("dvr_snapshot()");

    size_t sz = 0;
    uint8_t* jpeg = dvr_snapshot(h, &sz, 0);

    dvr_last_error(h, errbuf, sizeof(errbuf));

    if (jpeg && sz > 0) {
        PASS("Got data");
        INFO("Size: %zu bytes", sz);
        INFO("First 8 bytes: %02X %02X %02X %02X %02X %02X %02X %02X",
             jpeg[0], jpeg[1], jpeg[2], jpeg[3],
             jpeg[4], jpeg[5], jpeg[6], jpeg[7]);
        dvr_free_buffer(jpeg);
    } else {
        FAIL("No data");
        INFO("last_error: %s", errbuf[0] ? errbuf : "(empty)");
    }

    dvr_destroy(h);
    EXIT_SUMMARY();
}
