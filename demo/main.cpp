/**
 * demo/main.cpp — DVR → ffmpeg → JPEG → UDP stream diagnostic tool
 *
 * Build:  cmake --build . --config Release --target cppdvr_demo
 * Run:    x64\Release\cppdvr_demo.exe  [dvr_host]  [user]  [password]  [stream]  [--run]
 *
 * Pipeline:
 *   DVRIPCam (DVRIP)
 *     └─► StreamServer (ffmpeg decodes H264/H265 → real JPEG)
 *              ├─► HTTP  http://localhost:8080/stream   (browser debug view)
 *              └─► UdpStreamServer → Quest 3 headset    (live UDP JPEG stream)
 *                        └─► receives Quest 3 controller input
 *
 * Root cause of "no image on Quest":
 *   The previous version sent raw H264/H265 NAL bytes as UDP "JPEG" chunks.
 *   The Quest JPEG decoder rejected them.  This version wires StreamServer's
 *   ffmpeg output (real JPEG bytes starting with FF D8) directly into
 *   UdpStreamServer::sendJpeg().
 *
 * Modes:
 *   default  — diagnostic run for 30 s, then print summary and exit.
 *   --run    — stream indefinitely; status line every 5 s.
 *              Type  q <Enter>  to stop cleanly.
 *
 * Hard-coded network config (edit constants below):
 *   LOCAL_IP  — NIC to bind the UDP socket ("0.0.0.0" = all interfaces)
 *   TARGET_IP — Quest 3 IP  ("" = auto-detect from first inbound packet)
 *   RX_PORT   — port this PC listens on (headset sends here)
 *   TX_PORT   — port the headset listens on (we send here)
 *   HTTP_PORT — local MJPEG server for browser debug view
 *
 * Expected healthy output:
 *   [    0 ms] StreamServer starting...
 *   [  133 ms] DVR: login OK  session=0x00001234  stream=Main
 *   [  214 ms] ffmpeg: codec=h265  832x1616 @ 25 fps
 *   [  350 ms] ffmpeg: first JPEG frame ready — streaming
 *   [  351 ms] JPEG #1    size=18432 B  KB_total=18  target=(waiting for Quest)
 *   [  500 ms] INPUT #1   seq=42  RIGHT active=1  trig=0.00  grip=0.00
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include "stream_server.h"
#include "udp_stream_server.h"

// ── Hard-coded network config ──────────────────────────────────────────────────
static constexpr const char* LOCAL_IP  = "0.0.0.0";  // bind all NICs for UDP RX
static constexpr const char* TARGET_IP = "";          // "" = auto-discover Quest IP
static constexpr int         RX_PORT   = 9000;        // we listen here
static constexpr int         TX_PORT   = 9001;        // Quest listens here
static constexpr int         HTTP_PORT = 8080;        // browser debug: /stream

// ── Helpers ───────────────────────────────────────────────────────────────────

static auto g_t0 = std::chrono::steady_clock::now();

static long long ms_now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - g_t0).count();
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

// ── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    // ── Parse arguments ───────────────────────────────────────────────────────
    // cppdvr_demo.exe  [host]  [user]  [password]  [stream]  [--run]
    // --run may appear at any position.
    bool        run_forever = false;
    const char* host        = "172.20.80.12";
    const char* user        = "admin";
    const char* password    = "";
    const char* stream      = "Main";

    int positional = 0;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--run") == 0) {
            run_forever = true;
        } else {
            switch (positional++) {
                case 0: host     = argv[i]; break;
                case 1: user     = argv[i]; break;
                case 2: password = argv[i]; break;
                case 3: stream   = argv[i]; break;
            }
        }
    }

    std::printf("=== cppdvr demo ===\n");
    std::printf("DVR   host=%s  user=%s  stream=%s\n", host, user, stream);
    std::printf("UDP   local=%s  target=%s  rx=%d  tx=%d\n",
                LOCAL_IP, *TARGET_IP ? TARGET_IP : "(auto)", RX_PORT, TX_PORT);
    std::printf("HTTP  http://localhost:%d/stream  (browser debug)\n", HTTP_PORT);
    if (run_forever)
        std::printf("Mode: CONTINUOUS — type  q <Enter>  to quit\n");
    else
        std::printf("Mode: DIAGNOSTIC — 30 s timed run  (add --run for continuous)\n");
    std::printf("\n");
    std::fflush(stdout);

    // ── 1. UDP stream server ──────────────────────────────────────────────────
    cppdvr::UdpStreamServer udp;
    udp.setLocalIP(LOCAL_IP);
    udp.setTargetIP(TARGET_IP);
    udp.setRXPort(RX_PORT);
    udp.setTXPort(TX_PORT);
    udp.setLocalhostDebug(false);

    udp.setLogCallback([](const char* msg) {
        print("%s", msg);
    });

    // Controller input — print Quest 3 state
    std::atomic<int> input_count{0};
    udp.setOnInputCallback([&](const cppdvr::UdpInputEvent& e) {
        int n = ++input_count;
        // Full detail for first 5, then every 50th
        if (n > 5 && n % 50 != 0) return;

        const auto& L = e.left;
        const auto& R = e.right;
        std::lock_guard<std::mutex> lk(g_print_mtx);
        std::printf("[%6lld ms] INPUT #%-4d  seq=%u\n", ms_now(), n, e.seq);
        std::printf("  LEFT   A=%d B=%d menu=%d  "
                    "trig=%.2f(%d) grip=%.2f(%d)  "
                    "stick=(%.2f,%.2f)  active=%d\n",
                    L.primary_button, L.secondary_button, L.menu_button,
                    L.trigger_value,  L.trigger_click,
                    L.grip_value,     L.grip_click,
                    L.thumbstick_x,   L.thumbstick_y, L.active);
        std::printf("  RIGHT  A=%d B=%d menu=%d  "
                    "trig=%.2f(%d) grip=%.2f(%d)  "
                    "stick=(%.2f,%.2f)  active=%d\n",
                    R.primary_button, R.secondary_button, R.menu_button,
                    R.trigger_value,  R.trigger_click,
                    R.grip_value,     R.grip_click,
                    R.thumbstick_x,   R.thumbstick_y, R.active);
        std::printf("  GUI    [%d %d %d %d %d %d %d %d]\n",
                    e.gui[0], e.gui[1], e.gui[2], e.gui[3],
                    e.gui[4], e.gui[5], e.gui[6], e.gui[7]);
        std::fflush(stdout);
    });

    // Inbound JPEG from Quest (its own camera view, optional)
    udp.setOnJpegCallback([](uint32_t fid, const uint8_t* /*data*/, size_t size) {
        print("UDP RX: Quest JPEG  frame_id=%u  size=%zu", fid, size);
    });

    if (!udp.init()) {
        print("UDP init FAILED — port %d in use?", RX_PORT);
        return 1;
    }
    if (!udp.start()) {
        print("UDP start FAILED");
        udp.deinit();
        return 1;
    }

    // ── 2. Stream server: DVR → ffmpeg → real JPEG ────────────────────────────
    cppdvr::StreamServerConfig cfg;
    cfg.dvr_host     = host;
    cfg.dvr_user     = user;
    cfg.dvr_password = password;
    cfg.stream_type  = stream;
    cfg.http_port    = HTTP_PORT;
    cfg.jpeg_quality = 1;   // ffmpeg -q:v  (1=best/largest … 31=smallest)

    cppdvr::StreamServer srv(cfg);

    srv.set_log_callback([](const char* msg) {
        print("%s", msg);
    });

    // ── JPEG-ready callback ────────────────────────────────────────────────────
    // Fired by the ffmpeg pipeline thread for every decoded JPEG.
    // This is the only correct place to get real JPEG bytes — NOT from the
    // DVRIPCam frame callback which delivers raw H264/H265 NAL data.
    std::atomic<int>    jpeg_count{0};
    std::atomic<size_t> jpeg_bytes_total{0};

    srv.set_jpeg_callback([&](const uint8_t* data, size_t size) {
        int n = ++jpeg_count;
        jpeg_bytes_total += size;

        // Log first 5, then every 100th
        if (n <= 5 || n % 100 == 0)
            print("JPEG #%-4d  size=%zu B  KB_total=%zu  target=%s",
                  n, size, jpeg_bytes_total.load() / 1024,
                  udp.targetIP().empty() ? "(waiting for Quest)" : udp.targetIP().c_str());

        // Send the real JPEG to the Quest headset over UDP
        udp.sendJpeg(data, size, static_cast<uint32_t>(n));
    });

    print("StreamServer starting...");
    if (!srv.start()) {
        print("StreamServer start FAILED");
        udp.stop();
        udp.deinit();
        return 1;
    }
    print("HTTP debug stream: http://localhost:%d/stream", HTTP_PORT);

    // ── 3. Keyboard thread (--run mode only) ──────────────────────────────────
    std::atomic<bool> quit_flag{false};
    std::thread kbd_thread;

    if (run_forever) {
        std::printf("--- streaming indefinitely ---  type  q <Enter>  to quit\n");
        std::fflush(stdout);

        kbd_thread = std::thread([&quit_flag]() {
            char line[64];
            while (std::fgets(line, sizeof(line), stdin)) {
                if (line[0] == 'q' || line[0] == 'Q') {
                    quit_flag.store(true);
                    break;
                }
            }
            quit_flag.store(true);  // also fires on EOF / pipe close
        });
    }

    // ── 4. Status ticker ──────────────────────────────────────────────────────
    // Diagnostic: 1 s ticks, stop at 30 s.
    // Continuous:  5 s ticks, stop on 'q'.
    const int tick_sec  = run_forever ? 5  : 1;
    const int max_ticks = run_forever ? INT_MAX : 30;

    for (int tick = 1; tick <= max_ticks; ++tick) {
        // Sleep in 200 ms slices to react to quit_flag quickly
        for (int ms = 0; ms < tick_sec * 1000; ms += 200) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (quit_flag.load() || !srv.is_running()) goto done_ticking;
        }

        {
            std::lock_guard<std::mutex> lk(g_print_mtx);
            std::printf("[%6lld ms] -- %s %d s --  "
                        "jpegs=%d  KB_sent=%zu  "
                        "input_pkts=%d  target=%s\n",
                        ms_now(),
                        run_forever ? "uptime" : "tick",
                        run_forever ? (tick * tick_sec) : tick,
                        jpeg_count.load(),
                        jpeg_bytes_total.load() / 1024,
                        input_count.load(),
                        udp.targetIP().empty() ? "(none yet)" : udp.targetIP().c_str());
            std::fflush(stdout);
        }
    }
    done_ticking:;

    // ── 5. Shutdown ───────────────────────────────────────────────────────────
    if (kbd_thread.joinable()) {
        quit_flag.store(true);
        kbd_thread.detach();
    }

    print("Stopping StreamServer ...");
    srv.stop();

    print("Stopping UDP server ...");
    udp.stop();
    udp.deinit();

    std::printf("\n=== RESULT ===\n");
    std::printf("JPEG frames sent  : %d\n",  jpeg_count.load());
    std::printf("KB sent over UDP  : %zu\n", jpeg_bytes_total.load() / 1024);
    std::printf("Quest inputs recv : %d\n",  input_count.load());
    std::printf("result            : %s\n",
                jpeg_count.load() > 0
                    ? "OK — real JPEG frames streamed to Quest"
                    : "FAIL — no JPEG frames (is ffmpeg in PATH?)");

    return jpeg_count.load() > 0 ? 0 : 1;
}
