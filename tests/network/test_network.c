/**
 * tests/network/test_network.c
 *
 * Tests dvr_get_network_info() and dvr_set_network_info() (round-trip).
 * The set call writes back the same values that were read — it changes nothing
 * on the device but verifies the write command is accepted.
 *
 * Usage:
 *   test_network [host [user [password]]]
 *
 * Environment:
 *   DVR_HOST, DVR_USER, DVR_PASS
 */

#include "cppdvr_api.h"
#include "test_helpers.h"
#include <string.h>

/* Basic IPv4 dot-notation check: at least 7 chars, contains dots */
static int looks_like_ip(const char* s) {
    if (!s || strlen(s) < 7) return 0;
    int dots = 0;
    for (const char* p = s; *p; p++) {
        if (*p == '.') dots++;
        else if (*p < '0' || *p > '9') return 0;
    }
    return dots == 3;
}

int main(int argc, char* argv[]) {
    const char* host = (argc >= 2) ? argv[1] : dvr_env("DVR_HOST", DVR_DEFAULT_HOST);
    const char* user = (argc >= 3) ? argv[2] : dvr_env("DVR_USER", DVR_DEFAULT_USER);
    const char* pass = (argc >= 4) ? argv[3] : dvr_env("DVR_PASS", DVR_DEFAULT_PASS);

    printf("=== Network Settings Test ===\n");
    INFO("Host: %s  User: %s", host, user);

    DVRHandle h = dvr_create(host, DVR_DEFAULT_PORT, user, pass);
    if (!h || !dvr_login(h)) {
        FAIL("Login failed");
        dvr_destroy(h);
        EXIT_SUMMARY();
    }
    PASS("Login OK");

    /* ── Test 1: Get network info ────────────────────────────────────────── */
    SECTION("Get network info");

    DVRNetworkInfoC net;
    memset(&net, 0, sizeof(net));
    int ok = dvr_get_network_info(h, &net);

    if (!ok) {
        FAIL("dvr_get_network_info() failed");
        dvr_destroy(h);
        EXIT_SUMMARY();
    }
    PASS("dvr_get_network_info() succeeded");

    INFO("IP:       %s", net.ip);
    INFO("Mask:     %s", net.mask);
    INFO("Gateway:  %s", net.gateway);
    INFO("DNS:      %s", net.dns);
    INFO("Hostname: %s", net.hostname);
    INFO("MAC:      %s", net.mac);
    INFO("TCP port: %d", net.tcp_port);
    INFO("HTTP port:%d", net.http_port);
    INFO("DHCP:     %s", net.dhcp ? "yes" : "no");

    /* Validate IP fields */
    SECTION("Field validation");

    if (looks_like_ip(net.ip)) {
        PASS("IP field is a valid IPv4 address");
    } else {
        FAILF("IP field '%s' does not look like an IPv4 address", net.ip);
    }

    if (looks_like_ip(net.mask)) {
        PASS("Mask field is a valid IPv4 address");
    } else {
        FAILF("Mask field '%s' does not look like a subnet mask", net.mask);
    }

    if (net.tcp_port > 0 && net.tcp_port <= 65535) {
        PASS("TCP port is in range");
    } else {
        FAILF("TCP port %d is out of range", net.tcp_port);
    }

    /* The reported IP should match the host we connected to */
    if (strcmp(net.ip, host) == 0) {
        PASS("Reported IP matches the connected host");
    } else {
        INFO("Reported IP '%s' differs from connection host '%s'", net.ip, host);
        INFO("(May be expected if DNS/hostname used to connect)");
    }

    /* ── Test 2: Set network info (round-trip) ──────────────────────────── */
    SECTION("Set network info (round-trip — no change)");

    int set_ok = dvr_set_network_info(h, &net);
    if (set_ok) {
        PASS("dvr_set_network_info() accepted the round-trip write");
    } else {
        FAIL("dvr_set_network_info() failed on round-trip write");
    }

    /* Read back and compare */
    sleep_ms(500);
    DVRNetworkInfoC net2;
    memset(&net2, 0, sizeof(net2));
    if (dvr_get_network_info(h, &net2)) {
        if (strcmp(net.ip, net2.ip) == 0 && strcmp(net.mask, net2.mask) == 0) {
            PASS("Network info unchanged after round-trip write");
        } else {
            FAILF("Network info changed: IP %s→%s  mask %s→%s",
                  net.ip, net2.ip, net.mask, net2.mask);
        }
    } else {
        FAIL("dvr_get_network_info() failed after set");
    }

    dvr_destroy(h);
    EXIT_SUMMARY();
}
