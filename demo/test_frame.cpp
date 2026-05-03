/**
 * demo/test_frame.cpp — Read exactly one JPEG frame from the DVR and report its size.
 *
 * Build:  cmake --build . --config Release --target cppdvr_test_frame
 * Run:    x64\Release\cppdvr_test_frame.exe  [dvr_host]  [user]  [password]  [stream]
 *
 * Exits 0 if a valid JPEG frame was received, 1 otherwise.
 * Timeout: 15 seconds.
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "stream_server.h"

int main(int argc, char* argv[]) {
    const char* host     = "172.20.80.12";
    const char* user     = "admin";
    const char* password = "";
    const char* stream   = "Main";

    int positional = 0;
    for (int i = 1; i < argc; ++i) {
        switch (positional++) {
            case 0: host     = argv[i]; break;
            case 1: user     = argv[i]; break;
            case 2: password = argv[i]; break;
            case 3: stream   = argv[i]; break;
        }
    }

    std::printf("DVR  host=%s  user=%s  stream=%s\n", host, user, stream);
    std::printf("Waiting for first JPEG frame (timeout 15 s)...\n\n");
    std::fflush(stdout);

    // ── Sync primitives ──────────────────────────────────────────────────────────
    std::mutex              mtx;
    std::condition_variable cv;
    std::vector<uint8_t>    captured_frame;
    std::atomic<bool>       got_frame{false};

    // ── StreamServer ─────────────────────────────────────────────────────────────
    cppdvr::StreamServerConfig cfg;
    cfg.dvr_host     = host;
    cfg.dvr_user     = user;
    cfg.dvr_password = password;
    cfg.stream_type  = stream;
    cfg.http_port    = 0;          // disable HTTP server for this test
    cfg.jpeg_quality = 1;          // best quality

    cppdvr::StreamServer srv(cfg);

    srv.set_log_callback([](const char* msg) {
        std::printf("  [log] %s\n", msg);
        std::fflush(stdout);
    });

    srv.set_jpeg_callback([&](const uint8_t* data, size_t size) {
        if (got_frame.exchange(true)) return;   // capture only the first frame

        {
            std::lock_guard<std::mutex> lk(mtx);
            captured_frame.assign(data, data + size);
        }
        cv.notify_one();
    });

    if (!srv.start()) {
        std::printf("FAIL: StreamServer::start() returned false\n");
        return 1;
    }

    // ── Wait up to 15 s ──────────────────────────────────────────────────────────
    {
        std::unique_lock<std::mutex> lk(mtx);
        const bool ok = cv.wait_for(lk, std::chrono::seconds(15),
                                    [&]{ return !captured_frame.empty(); });
        if (!ok) {
            srv.stop();
            std::printf("FAIL: timed out — no JPEG frame received in 15 s\n");
            std::printf("      (is ffmpeg in PATH?  is DVR reachable?)\n");
            return 1;
        }
    }

    srv.stop();

    // ── Report ───────────────────────────────────────────────────────────────────
    const size_t sz = captured_frame.size();
    const bool   is_jpeg = sz >= 4
                        && captured_frame[0] == 0xFF
                        && captured_frame[1] == 0xD8
                        && captured_frame[sz-2] == 0xFF
                        && captured_frame[sz-1] == 0xD9;

    // Save to disk
    const char* out_path = "test_frame.jpg";
    FILE* f = std::fopen(out_path, "wb");
    bool saved = false;
    if (f) {
        saved = (std::fwrite(captured_frame.data(), 1, sz, f) == sz);
        std::fclose(f);
    }

    std::printf("\n=== RESULT ===\n");
    std::printf("Frame size   : %zu bytes  (%.1f KB)\n", sz, sz / 1024.0);
    std::printf("JPEG magic   : %s  (first bytes: %02X %02X, last: %02X %02X)\n",
                is_jpeg ? "OK (FF D8 ... FF D9)" : "BAD",
                captured_frame[0], captured_frame[1],
                captured_frame[sz-2], captured_frame[sz-1]);
    std::printf("Saved to     : %s  (%s)\n",
                out_path, saved ? "OK" : "FAILED — check write permissions");
    std::printf("Result       : %s\n", is_jpeg ? "OK" : "FAIL — not a valid JPEG");

    return is_jpeg ? 0 : 1;
}
