/**
 * demo/main.cpp — DVR connection + frame-reception diagnostic tool
 *
 * Build:  cmake --build . --config Release --target cppdvr_demo
 * Run:    x64\Release\cppdvr_demo.exe  [host]  [user]  [password]
 *
 * Connects directly to the DVR camera using the DVRIPCam C++ class
 * (bypasses StreamServer entirely) and prints every received frame
 * with its type, size, and first bytes.  Useful for verifying that
 * the DVRIP → frame pipeline works before involving ffmpeg/HTTP.
 *
 * Expected healthy output:
 *   [  0 ms] LOGIN OK   session=0x00001234  alive=20s
 *   [  8 ms] CLAIM OK   Ret=100
 *   [  9 ms] START sent
 *   [189 ms] FRAME #1   type=h264  frame=I  500x300@25fps  size=45678
 *              bytes: 00 00 00 01 67 ...
 *   [222 ms] FRAME #2   type=h264  frame=P  size=1234
 *   ...
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>    // timeGetTime

#include <atomic>
#include <chrono>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "dvrip.h"

// ── helpers ───────────────────────────────────────────────────────────────────

static auto g_t0 = std::chrono::steady_clock::now();

static long long ms_now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - g_t0)
        .count();
}

static std::mutex g_print_mtx;

template<typename... Args>
static void print(const char* fmt, Args... args) {
    char buf[512];
    std::snprintf(buf, sizeof(buf), fmt, args...);
    std::lock_guard<std::mutex> lk(g_print_mtx);
    std::printf("[%6lld ms] %s\n", ms_now(), buf);
    std::fflush(stdout);
}

static void hex8(const uint8_t* d, size_t n) {
    std::printf("              bytes:");
    for (size_t i = 0; i < n && i < 16; ++i)
        std::printf(" %02x", d[i]);
    if (n > 16) std::printf(" ...");
    std::printf("\n");
    std::fflush(stdout);
}

// ── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    const char* host     = (argc > 1) ? argv[1] : "172.20.80.12";
    const char* user     = (argc > 2) ? argv[2] : "admin";
    const char* password = (argc > 3) ? argv[3] : "";
    const char* stream   = (argc > 4) ? argv[4] : "Main";

    std::printf("=== cppdvr diagnostic ===\n");
    std::printf("host=%s  user=%s  stream=%s\n\n", host, user, stream);
    std::fflush(stdout);

    // ── 1. Create camera ──────────────────────────────────────────────────────
    cppdvr::DVRIPCam cam(host, user, password);

    // Forward DVR-internal log messages (Claim/Start/first-frame)
    cam.set_log_callback([](const char* msg) {
        print("DVR-LOG: %s", msg);
    });

    // ── 2. Login ──────────────────────────────────────────────────────────────
    print("Attempting login ...");
    if (!cam.login()) {
        print("LOGIN FAILED: %s", cam.last_error().c_str());
        return 1;
    }
    print("LOGIN OK   session=0x%08X", cam.session_id());

    // ── 3. Start monitor and collect frames ───────────────────────────────────
    std::atomic<int> total_frames{0};
    std::map<std::string, int> type_counts;
    std::map<std::string, int> frame_counts;
    std::mutex counts_mtx;
    bool first_iframe_seen = false;

    auto frame_cb = [&](const uint8_t*             data,
                         size_t                     size,
                         const cppdvr::FrameMeta&   meta)
    {
        int n = ++total_frames;

        {
            std::lock_guard<std::mutex> lk(counts_mtx);
            type_counts[meta.type.empty() ? "(empty)" : meta.type]++;
            frame_counts[meta.frame.empty() ? "(empty)" : meta.frame]++;
        }

        // Print first 30 frames in full detail, then every 100th
        bool print_detail = (n <= 30) || (n % 100 == 0);

        if (print_detail) {
            std::lock_guard<std::mutex> lk(g_print_mtx);
            std::printf("[%6lld ms] FRAME #%-4d  type=%-8s  frame=%s"
                        "  %dx%d@%dfps  size=%zu\n",
                ms_now(), n,
                meta.type.empty()  ? "(empty)" : meta.type.c_str(),
                meta.frame.empty() ? "(empty)" : meta.frame.c_str(),
                meta.width, meta.height, meta.fps,
                size);
            if (size > 0) hex8(data, size);
            std::fflush(stdout);
        }

        if (!first_iframe_seen && meta.frame == "I") {
            first_iframe_seen = true;
            print("*** FIRST I-FRAME at %lld ms  type=%s  size=%zu ***",
                  ms_now(), meta.type.c_str(), size);
        }
    };

    print("Starting monitor (stream=%s) ...", stream);
    if (!cam.start_monitor(frame_cb, stream)) {
        print("start_monitor FAILED: %s", cam.last_error().c_str());
        return 1;
    }
    print("Monitor thread launched — waiting up to 30 s for frames ...");

    // ── 4. Status ticker ──────────────────────────────────────────────────────
    for (int sec = 1; sec <= 30; ++sec) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        std::lock_guard<std::mutex> lk(g_print_mtx);
        std::printf("[%6lld ms] -- tick %2d s --  total_frames=%d",
                    ms_now(), sec, total_frames.load());
        {
            std::lock_guard<std::mutex> lk2(counts_mtx);
            for (auto& [k, v] : type_counts)
                std::printf("  %s=%d", k.c_str(), v);
        }
        std::printf("\n");
        std::fflush(stdout);

        if (total_frames.load() > 200) break;  // got plenty, stop early
    }

    // ── 5. Stop and report ────────────────────────────────────────────────────
    print("Stopping monitor ...");
    cam.stop_monitor();

    std::printf("\n=== RESULT ===\n");
    std::printf("total_frames : %d\n", total_frames.load());
    {
        std::lock_guard<std::mutex> lk(counts_mtx);
        std::printf("by type:\n");
        for (auto& [k, v] : type_counts)
            std::printf("  %-10s : %d\n", k.c_str(), v);
        std::printf("by frame kind:\n");
        for (auto& [k, v] : frame_counts)
            std::printf("  %-10s : %d\n", k.c_str(), v);
    }
    std::printf("first I-frame: %s\n", first_iframe_seen ? "YES" : "NO — bug!");

    return first_iframe_seen ? 0 : 1;
}
