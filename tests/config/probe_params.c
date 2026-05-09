/**
 * tests/config/probe_params.c
 *
 * Connects to the DVR camera and prints the full JSON for:
 *   - Simplify.Encode
 *   - AVEnc.VideoColor
 */

#include "cppdvr_api.h"
#include "test_helpers.h"

int main(void) {
    const char* host = dvr_env("DVR_HOST", DVR_DEFAULT_HOST);
    const char* user = dvr_env("DVR_USER", DVR_DEFAULT_USER);
    const char* pass = dvr_env("DVR_PASS", DVR_DEFAULT_PASS);

    DVRHandle h = dvr_create(host, DVR_DEFAULT_PORT, user, pass);
    if (!h || !dvr_login(h)) {
        printf("ERROR: Login failed to %s\n", host);
        dvr_destroy(h);
        return 1;
    }
    printf("Connected to %s as %s\n\n", host, user);

    const char* keys[] = { "Simplify.Encode", "AVEnc.VideoColor" };
    for (int i = 0; i < 2; i++) {
        printf("=== %s ===\n", keys[i]);
        char* json = dvr_get_info(h, keys[i]);
        if (json) {
            printf("%s\n\n", json);
            dvr_free_string(json);
        } else {
            printf("(null — dvr_get_info returned NULL)\n\n");
        }
    }

    dvr_destroy(h);
    return 0;
}
