/**
 * tests/discovery/test_discovery.c
 *
 * Verifies that DVRIPCam::discover() finds at least one device on the local
 * network and that the known camera (DVR_HOST) appears in the result list.
 *
 * Usage:
 *   test_discovery [timeout_ms]
 *
 * Environment:
 *   DVR_HOST  — expected camera IP (default 172.20.80.12)
 */

#include "cppdvr_api.h"
#include "test_helpers.h"

int main(int argc, char* argv[]) {
    const char* expected_ip = dvr_env("DVR_HOST", DVR_DEFAULT_HOST);
    int timeout_ms = (argc >= 2) ? atoi(argv[1]) : 3000;

    printf("=== Discovery Test ===\n");
    INFO("Broadcast timeout: %d ms", timeout_ms);
    INFO("Expected camera IP: %s", expected_ip);

    SECTION("UDP broadcast discovery");

    DVRDiscoveredDeviceC* devs = NULL;
    int count = 0;
    int ok = dvr_discover(timeout_ms, &devs, &count);

    if (!ok) {
        FAIL("dvr_discover() returned error");
        EXIT_SUMMARY();
    }
    PASS("dvr_discover() returned without error");

    INFO("Devices found: %d", count);

    if (count == 0) {
        FAIL("No devices discovered on the network");
        EXIT_SUMMARY();
    }
    PASS("At least one device discovered");

    /* Print all discovered devices */
    SECTION("Discovered devices");
    for (int i = 0; i < count; i++) {
        INFO("[%d] IP=%-16s  MAC=%-18s  host=%-20s  sn=%s  tcp=%d  http=%d",
             i,
             devs[i].ip,
             devs[i].mac,
             devs[i].hostname,
             devs[i].sn,
             devs[i].tcp_port,
             devs[i].http_port);
    }

    /* Verify expected camera is in the list */
    SECTION("Known camera presence");
    int found_expected = 0;
    for (int i = 0; i < count; i++) {
        if (strcmp(devs[i].ip, expected_ip) == 0) {
            found_expected = 1;
            INFO("Found expected camera at %s (host=%s, sn=%s)",
                 devs[i].ip, devs[i].hostname, devs[i].sn);
            break;
        }
    }

    if (found_expected) {
        PASS("Expected camera IP found in discovery results");
    } else {
        FAILF("Expected camera %s not in discovery results", expected_ip);
    }

    /* Verify field sanity for all devices */
    SECTION("Field sanity checks");
    int sanity_ok = 1;
    for (int i = 0; i < count; i++) {
        if (devs[i].ip[0] == '\0') {
            FAILF("Device[%d] has empty IP", i);
            sanity_ok = 0;
        }
        if (devs[i].tcp_port <= 0 || devs[i].tcp_port > 65535) {
            FAILF("Device[%d] tcp_port=%d out of range", i, devs[i].tcp_port);
            sanity_ok = 0;
        }
    }
    if (sanity_ok) PASS("All device fields pass sanity checks");

    dvr_free_discovered(devs);
    EXIT_SUMMARY();
}
