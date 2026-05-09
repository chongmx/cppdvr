/**
 * tests/osd/test_osd.c
 *
 * Tests channel title text overlay and 1-bpp bitmap overlay.
 *
 * Usage:
 *   test_osd [host [user [password]]]
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

    printf("=== OSD Test ===\n");
    INFO("Host: %s  User: %s", host, user);

    DVRHandle h = dvr_create(host, DVR_DEFAULT_PORT, user, pass);
    if (!h || !dvr_login(h)) {
        FAIL("Login failed");
        dvr_destroy(h);
        EXIT_SUMMARY();
    }
    PASS("Login OK");

    /* ── Test 1: Single channel title ───────────────────────────────────── */
    SECTION("Single channel title");

    const char* t1[] = { "Test Camera 1" };
    int ok1 = dvr_set_channel_titles(h, t1, 1);
    if (ok1) {
        PASS("Set single channel title");
    } else {
        FAIL("dvr_set_channel_titles() failed for single title");
    }
    sleep_ms(300);

    /* ── Test 2: Multiple channel titles ────────────────────────────────── */
    SECTION("Multiple channel titles");

    const char* t4[] = { "Front Door", "Backyard", "Garage", "Driveway" };
    int ok4 = dvr_set_channel_titles(h, t4, 4);
    if (ok4) {
        PASS("Set four channel titles");
    } else {
        FAIL("dvr_set_channel_titles() failed for four titles");
    }
    sleep_ms(300);

    /* ── Test 3: Empty title (clears label) ─────────────────────────────── */
    SECTION("Empty channel title (clear)");

    const char* tempty[] = { "" };
    int okclear = dvr_set_channel_titles(h, tempty, 1);
    if (okclear) {
        PASS("Set empty channel title (clear)");
    } else {
        FAIL("dvr_set_channel_titles() failed with empty title");
    }
    sleep_ms(300);

    /* ── Test 4: Bitmap overlay — minimal 8x8 all-black ─────────────────── */
    SECTION("Bitmap overlay (8x8 all-black)");

    /* 8x8 monochrome bitmap: 8 pixels wide = 1 byte/row, 8 rows = 8 bytes */
    const uint8_t bmp8[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    int okbmp = dvr_set_channel_bitmap(h, 8, 8, bmp8, sizeof(bmp8));
    if (okbmp) {
        PASS("Set 8x8 bitmap overlay");
    } else {
        FAIL("dvr_set_channel_bitmap() failed for 8x8 all-black");
    }
    sleep_ms(300);

    /* ── Test 5: Bitmap overlay — 16x8 checkerboard ─────────────────────── */
    SECTION("Bitmap overlay (16x8 checkerboard)");

    /* 16x8 bitmap: 16 pixels wide = 2 bytes/row, 8 rows = 16 bytes */
    /* Alternating 0xAA 0x55 per row creates a checkerboard */
    const uint8_t bmp16[] = {
        0xAA, 0x55,  0x55, 0xAA,  0xAA, 0x55,  0x55, 0xAA,
        0xAA, 0x55,  0x55, 0xAA,  0xAA, 0x55,  0x55, 0xAA,
    };
    int okbmp16 = dvr_set_channel_bitmap(h, 16, 8, bmp16, sizeof(bmp16));
    if (okbmp16) {
        PASS("Set 16x8 checkerboard bitmap overlay");
    } else {
        FAIL("dvr_set_channel_bitmap() failed for 16x8 checkerboard");
    }

    dvr_destroy(h);
    EXIT_SUMMARY();
}
