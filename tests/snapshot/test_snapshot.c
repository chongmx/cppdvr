/**
 * tests/snapshot/test_snapshot.c
 *
 * Captures a JPEG snapshot from the DVR, validates the data, and saves it
 * to disk for visual inspection.
 *
 * Usage:
 *   test_snapshot [host [user [password [output.jpg]]]]
 *
 * Environment:
 *   DVR_HOST, DVR_USER, DVR_PASS
 */

#include "cppdvr_api.h"
#include "test_helpers.h"

static int write_file(const char* path, const uint8_t* data, size_t size) {
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    size_t n = fwrite(data, 1, size, f);
    fclose(f);
    return (n == size) ? 1 : 0;
}

int main(int argc, char* argv[]) {
    const char* host    = (argc >= 2) ? argv[1] : dvr_env("DVR_HOST", DVR_DEFAULT_HOST);
    const char* user    = (argc >= 3) ? argv[2] : dvr_env("DVR_USER", DVR_DEFAULT_USER);
    const char* pass    = (argc >= 4) ? argv[3] : dvr_env("DVR_PASS", DVR_DEFAULT_PASS);
    const char* outfile = (argc >= 5) ? argv[4] : "snapshot_test.jpg";

    printf("=== Snapshot Test ===\n");
    INFO("Host: %s  User: %s  Output: %s", host, user, outfile);

    DVRHandle h = dvr_create(host, DVR_DEFAULT_PORT, user, pass);
    if (!h || !dvr_login(h)) {
        FAIL("Login failed");
        dvr_destroy(h);
        EXIT_SUMMARY();
    }
    PASS("Login OK");

    /* ── Test 1: Snapshot channel 0 ────────────────────────────────────── */
    SECTION("Snapshot channel 0");

    size_t sz = 0;
    uint8_t* jpeg = dvr_snapshot(h, &sz, 0);

    if (!jpeg || sz == 0) {
        FAIL("dvr_snapshot() returned empty result");
        char errbuf[512];
        dvr_last_error(h, errbuf, sizeof(errbuf));
        INFO("last_error: %s", errbuf[0] ? errbuf : "(empty)");
        dvr_destroy(h);
        EXIT_SUMMARY();
    }
    PASS("dvr_snapshot() returned data");
    INFO("Size: %zu bytes (%.1f KB)", sz, sz / 1024.0);

    if (jpeg[0] == 0xFF && jpeg[1] == 0xD8) {
        PASS("Data starts with JPEG SOI marker (FF D8)");
    } else {
        FAILF("Expected FF D8, got %02X %02X", jpeg[0], jpeg[1]);
    }

    if (jpeg[sz - 2] == 0xFF && jpeg[sz - 1] == 0xD9) {
        PASS("Data ends with JPEG EOI marker (FF D9)");
    } else {
        INFO("Note: last 2 bytes = %02X %02X (EOI check — some cameras omit it)",
             jpeg[sz - 2], jpeg[sz - 1]);
    }

    if (sz >= 1024) {
        PASS("Snapshot size >= 1 KB (plausible JPEG)");
    } else {
        FAILF("Snapshot unusually small: %zu bytes", sz);
    }

    /* Save to disk */
    if (write_file(outfile, jpeg, sz)) {
        PASS("Snapshot saved to disk");
        INFO("File: %s", outfile);
    } else {
        FAILF("Failed to write %s", outfile);
    }

    dvr_free_buffer(jpeg);

    /* ── Test 2: Second snapshot (verify session stays valid) ───────────── */
    SECTION("Second snapshot (session stability)");

    sleep_ms(500);
    sz = 0;
    jpeg = dvr_snapshot(h, &sz, 0);
    if (jpeg && sz > 0) {
        PASS("Second snapshot succeeded");
        INFO("Size: %zu bytes", sz);
        dvr_free_buffer(jpeg);
    } else {
        FAIL("Second snapshot failed");
    }

    dvr_destroy(h);
    EXIT_SUMMARY();
}
