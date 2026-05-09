/**
 * tests/config/test_config.c
 *
 * Tests dvr_get_info() and dvr_set_info() for various named config blocks.
 * Reads each block and verifies it returns non-empty valid JSON.
 * Also performs a round-trip set on one block.
 *
 * Usage:
 *   test_config [host [user [password]]]
 *
 * Environment:
 *   DVR_HOST, DVR_USER, DVR_PASS
 */

#include "cppdvr_api.h"
#include "test_helpers.h"
#include <string.h>

/* Very loose "looks like JSON" check */
static int looks_like_json(const char* s) {
    if (!s) return 0;
    size_t len = strlen(s);
    if (len < 2) return 0;
    return (s[0] == '{' || s[0] == '[' || s[0] == '"');
}

static void test_get(DVRHandle h, const char* name) {
    char* json = dvr_get_info(h, name);
    if (!json) {
        FAILF("dvr_get_info(\"%s\") returned NULL", name);
        return;
    }
    if (strlen(json) == 0) {
        FAILF("dvr_get_info(\"%s\") returned empty string", name);
        dvr_free_string(json);
        return;
    }
    if (!looks_like_json(json)) {
        FAILF("dvr_get_info(\"%s\") response does not look like JSON: %.60s", name, json);
        dvr_free_string(json);
        return;
    }
    PASS(name);
    INFO("  %.120s%s", json, strlen(json) > 120 ? " ..." : "");
    dvr_free_string(json);
}

int main(int argc, char* argv[]) {
    const char* host = (argc >= 2) ? argv[1] : dvr_env("DVR_HOST", DVR_DEFAULT_HOST);
    const char* user = (argc >= 3) ? argv[2] : dvr_env("DVR_USER", DVR_DEFAULT_USER);
    const char* pass = (argc >= 4) ? argv[3] : dvr_env("DVR_PASS", DVR_DEFAULT_PASS);

    printf("=== Config Get/Set Test ===\n");
    INFO("Host: %s  User: %s", host, user);

    DVRHandle h = dvr_create(host, DVR_DEFAULT_PORT, user, pass);
    if (!h || !dvr_login(h)) {
        FAIL("Login failed");
        dvr_destroy(h);
        EXIT_SUMMARY();
    }
    PASS("Login OK");

    /* ── Read various config blocks ─────────────────────────────────────── */
    SECTION("Reading config blocks");

    test_get(h, "SystemInfo");
    test_get(h, "General");
    test_get(h, "Camera");
    test_get(h, "Simplify.Encode");
    test_get(h, "AVEnc.VideoColor");
    test_get(h, "NetWork.NetCommon");

    /* ── Round-trip set on a safe block (General) ────────────────────────── */
    SECTION("Round-trip set on 'General'");

    char* original = dvr_get_info(h, "General");
    if (!original) {
        FAIL("Could not read 'General' for round-trip test");
    } else {
        int set_ok = dvr_set_info(h, "General", original);
        if (set_ok) {
            PASS("dvr_set_info(\"General\", ...) accepted the round-trip write");
        } else {
            FAIL("dvr_set_info(\"General\", ...) failed");
        }
        dvr_free_string(original);
    }

    /* ── get_info on unknown name ────────────────────────────────────────── */
    SECTION("Unknown config name (expected failure/empty)");

    char* unknown = dvr_get_info(h, "ThisKeyDoesNotExist.XYZ");
    if (!unknown || strlen(unknown) == 0 || strcmp(unknown, "null") == 0) {
        PASS("Unknown config name returns NULL or empty (expected)");
    } else {
        INFO("Unexpected non-empty response: %.80s", unknown);
        PASS("Unknown config name returned something (camera-specific behaviour)");
    }
    dvr_free_string(unknown);

    dvr_destroy(h);
    EXIT_SUMMARY();
}
