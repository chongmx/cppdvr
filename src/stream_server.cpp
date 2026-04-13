/**
 * stream_server.cpp — DVR-to-MJPEG HTTP streaming server
 *
 * C++ port of py_sample/stream_server.py.
 *
 * Pipeline:
 *   camera_thread   : DVRIP → raw H264/H265 NAL frames
 *   pipeline_thread : ffmpeg (pipe stdin→stdout) → MJPEG JPEG frames
 *   http_thread     : simple HTTP server serving /  /stream  /snapshot
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#pragma comment(lib, "ws2_32.lib")

#include "stream_server.h"
#include "dvrip.h"

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace cppdvr {

// ════════════════════════════════════════════════════════════════════════════════
// Network helper — ask Windows which local address routes to a destination
// Handles multi-NIC hosts (e.g., WiFi + wired): ensures the TCP connection
// to the DVR goes out on the interface that is on the same subnet.
// ════════════════════════════════════════════════════════════════════════════════
static std::string find_outbound_local_ip(const std::string& dest_ip, int dest_port) {
    // SIO_ROUTING_INTERFACE_QUERY returns the local sockaddr Windows would use
    // to reach the given destination — without actually connecting.
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return "";

    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(static_cast<u_short>(dest_port));
    inet_pton(AF_INET, dest_ip.c_str(), &dst.sin_addr);

    sockaddr_in src{};
    DWORD returned = 0;
    bool ok = (WSAIoctl(s, SIO_ROUTING_INTERFACE_QUERY,
                        &dst, static_cast<DWORD>(sizeof(dst)),
                        &src, static_cast<DWORD>(sizeof(src)),
                        &returned, nullptr, nullptr) == 0);
    ::closesocket(s);
    if (!ok) return "";

    char buf[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &src.sin_addr, buf, sizeof(buf));
    // 0.0.0.0 means "any interface" — not useful for explicit binding
    if (std::strcmp(buf, "0.0.0.0") == 0) return "";
    return buf;
}

// ════════════════════════════════════════════════════════════════════════════════
// Codec detection — inspect Annex-B NAL units (mirrors _detect_codec_from_bitstream)
// ════════════════════════════════════════════════════════════════════════════════
static std::string detect_codec(const uint8_t* data, size_t len) {
    if (len < 5) return "";

    uint8_t nal_byte = 0;
    if (data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1)
        nal_byte = data[4];
    else if (data[0] == 0 && data[1] == 0 && data[2] == 1)
        nal_byte = data[3];
    else
        return "";

    int h265_type = (nal_byte >> 1) & 0x3F;
    if (h265_type >= 16 && h265_type <= 40) return "h265";

    int h264_type = nal_byte & 0x1F;
    if (h264_type == 5 || h264_type == 6 ||
        h264_type == 7 || h264_type == 8) return "h264";

    return "";
}

// ════════════════════════════════════════════════════════════════════════════════
// Pub-sub frame queue (single producer / single consumer per queue)
// ════════════════════════════════════════════════════════════════════════════════
struct RawFrame {
    std::vector<uint8_t> data;
    FrameMeta            meta;
};

struct FrameQueue {
    static constexpr size_t kMaxSize = 60;

    std::mutex              mtx;
    std::condition_variable cv;
    std::queue<RawFrame>    q;
    bool                    closed{false};

    void push(RawFrame f) {
        std::lock_guard<std::mutex> lk(mtx);
        if (q.size() >= kMaxSize) q.pop();  // drop oldest
        q.push(std::move(f));
        cv.notify_one();
    }

    // Returns false if queue is closed and empty (producer done).
    bool pop(RawFrame& out, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lk(mtx);
        if (!cv.wait_for(lk, timeout, [this]{ return !q.empty() || closed; }))
            return false;  // timed out
        if (q.empty()) return false;  // closed
        out = std::move(q.front());
        q.pop();
        return true;
    }

    void close() {
        std::lock_guard<std::mutex> lk(mtx);
        closed = true;
        cv.notify_all();
    }
};

// ════════════════════════════════════════════════════════════════════════════════
// Latest JPEG frame — shared between pipeline and HTTP consumers
// ════════════════════════════════════════════════════════════════════════════════
struct LatestJpeg {
    mutable std::mutex              mtx;
    std::condition_variable         cv;
    std::vector<uint8_t>            data;

    void update(std::vector<uint8_t> jpeg) {
        std::lock_guard<std::mutex> lk(mtx);
        data = std::move(jpeg);
        cv.notify_all();
    }

    std::vector<uint8_t> get() const {
        std::lock_guard<std::mutex> lk(mtx);
        return data;
    }

    // Wait up to 'ms' for a new frame; returns copy (empty if timed out).
    std::vector<uint8_t> wait(std::chrono::milliseconds ms) {
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait_for(lk, ms, [this]{ return !data.empty(); });
        return data;
    }
};

// ════════════════════════════════════════════════════════════════════════════════
// ffmpeg process wrapper (Windows CreateProcess)
// ════════════════════════════════════════════════════════════════════════════════
struct FfmpegProc {
    HANDLE hStdinWrite  {INVALID_HANDLE_VALUE};
    HANDLE hStdoutRead  {INVALID_HANDLE_VALUE};
    HANDLE hStderrRead  {INVALID_HANDLE_VALUE};
    HANDLE hProcess     {INVALID_HANDLE_VALUE};
    HANDLE hThread_proc {INVALID_HANDLE_VALUE};

    bool start(const std::string& codec, int jpeg_quality,
               int jpeg_scale_w = 0, int jpeg_scale_h = 0) {
        // Map DVRIP codec name to ffmpeg format name
        std::string fmt = "h264";
        if (codec == "h265") fmt = "hevc";
        else if (codec == "mpeg4") fmt = "mpeg4";

        // Scale filter: use cfg.scale_width/height to resize before JPEG encode.
        // -1 on one dimension keeps the aspect ratio (e.g. "scale=416:-1").
        // Leave both 0 to pass the native camera resolution through unchanged.
        std::string scale_filter;
        if (jpeg_scale_w > 0 || jpeg_scale_h > 0) {
            int w = jpeg_scale_w > 0 ? jpeg_scale_w : -1;
            int h = jpeg_scale_h > 0 ? jpeg_scale_h : -1;
            scale_filter = std::string(" -vf scale=") + std::to_string(w)
                         + ":" + std::to_string(h);
        }

        std::string cmd =
            "ffmpeg -loglevel warning"
            " -f " + fmt + " -i pipe:0"
            + scale_filter +
            " -f mjpeg -q:v " + std::to_string(jpeg_quality) +
            " pipe:1";

        SECURITY_ATTRIBUTES sa{};
        sa.nLength        = sizeof(sa);
        sa.bInheritHandle = TRUE;

        // Create pipes
        HANDLE hStdinR, hStdoutW, hStderrW;
        if (!CreatePipe(&hStdinR,   &hStdinWrite,  &sa, 0))    return false;
        if (!CreatePipe(&hStdoutRead, &hStdoutW,   &sa, 65536)) { close_handles(); return false; }
        if (!CreatePipe(&hStderrRead, &hStderrW,   &sa, 4096))  { close_handles(); return false; }

        // Don't inherit our ends
        SetHandleInformation(hStdinWrite,   HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(hStdoutRead,   HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(hStderrRead,   HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW si{};
        si.cb          = sizeof(si);
        si.hStdInput   = hStdinR;
        si.hStdOutput  = hStdoutW;
        si.hStdError   = hStderrW;
        si.dwFlags     = STARTF_USESTDHANDLES;

        PROCESS_INFORMATION pi{};

        // Convert to wide string
        std::wstring wcmd(cmd.begin(), cmd.end());

        BOOL ok = CreateProcessW(
            nullptr, wcmd.data(), nullptr, nullptr,
            TRUE,                   // inherit handles
            CREATE_NO_WINDOW,
            nullptr, nullptr, &si, &pi
        );

        // Close child-side handles in parent
        CloseHandle(hStdinR);
        CloseHandle(hStdoutW);
        CloseHandle(hStderrW);

        if (!ok) { close_handles(); return false; }

        hProcess      = pi.hProcess;
        hThread_proc  = pi.hThread;
        return true;
    }

    bool write(const uint8_t* data, size_t len) {
        if (hStdinWrite == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        return WriteFile(hStdinWrite, data, static_cast<DWORD>(len),
                         &written, nullptr) && written == len;
    }

    // Read up to 'buf_size' bytes from stdout (non-blocking peek).
    int read(uint8_t* buf, size_t buf_size) {
        if (hStdoutRead == INVALID_HANDLE_VALUE) return -1;
        DWORD avail = 0;
        if (!PeekNamedPipe(hStdoutRead, nullptr, 0, nullptr, &avail, nullptr))
            return -1;
        if (avail == 0) return 0;
        DWORD to_read = (avail < static_cast<DWORD>(buf_size))
                      ? avail : static_cast<DWORD>(buf_size);
        DWORD nread = 0;
        if (!ReadFile(hStdoutRead, buf, to_read, &nread, nullptr)) return -1;
        return static_cast<int>(nread);
    }

    void terminate() {
        if (hProcess != INVALID_HANDLE_VALUE) {
            TerminateProcess(hProcess, 0);
            WaitForSingleObject(hProcess, 2000);
        }
        close_handles();
    }

    void close_handles() {
        auto close_if = [](HANDLE& h) {
            if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); h = INVALID_HANDLE_VALUE; }
        };
        close_if(hStdinWrite);
        close_if(hStdoutRead);
        close_if(hStderrRead);
        close_if(hProcess);
        close_if(hThread_proc);
    }
};

// ════════════════════════════════════════════════════════════════════════════════
// Minimal HTTP server (one thread per client)
// ════════════════════════════════════════════════════════════════════════════════
static const char k_index_html[] =
    "<!DOCTYPE html><html lang=\"en\"><head>"
    "<meta charset=\"UTF-8\"><title>DVR Live Stream</title>"
    "<style>"
    "body{margin:0;background:#111;display:flex;flex-direction:column;"
    "align-items:center;justify-content:center;min-height:100vh;}"
    "img{max-width:100%;max-height:95vh;border:2px solid #333;}"
    "p{color:#888;font-family:sans-serif;font-size:.85em;margin-top:8px;}"
    "a{color:#aaa;}"
    "</style></head><body>"
    "<img src=\"/stream\" alt=\"DVR stream\">"
    "<p><a href=\"/snapshot\">snapshot</a></p>"
    "</body></html>";

static void send_all(SOCKET s, const char* data, size_t len) {
    while (len > 0) {
        int r = ::send(s, data, static_cast<int>(len), 0);
        if (r == SOCKET_ERROR) break;
        data += r;
        len  -= r;
    }
}

static void http_serve_client(SOCKET client, LatestJpeg* latest,
                               std::atomic<bool>* running) {
    // Read request line (enough to identify path)
    char req_buf[1024] = {};
    int  nread = ::recv(client, req_buf, sizeof(req_buf) - 1, 0);
    if (nread <= 0) { ::closesocket(client); return; }

    std::string req(req_buf, static_cast<size_t>(nread));
    bool is_stream   = req.find("GET /stream")   != std::string::npos;
    bool is_snapshot = req.find("GET /snapshot") != std::string::npos;

    if (is_stream) {
        // Multipart MJPEG stream
        const char* hdr =
            "HTTP/1.1 200 OK\r\n"
            "Cache-Control: no-cache\r\n"
            "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
            "\r\n";
        send_all(client, hdr, strlen(hdr));

        while (running->load()) {
            auto frame = latest->wait(std::chrono::milliseconds(5000));
            if (frame.empty()) continue;

            // Build multipart part header
            char part_hdr[128];
            int part_len = std::snprintf(part_hdr, sizeof(part_hdr),
                "--frame\r\nContent-Type: image/jpeg\r\n"
                "Content-Length: %zu\r\n\r\n",
                frame.size());

            send_all(client, part_hdr, static_cast<size_t>(part_len));
            send_all(client, reinterpret_cast<const char*>(frame.data()), frame.size());
            send_all(client, "\r\n", 2);
        }

    } else if (is_snapshot) {
        auto frame = latest->get();
        if (frame.empty()) {
            const char* err =
                "HTTP/1.1 503 Service Unavailable\r\n"
                "Content-Length: 22\r\n\r\n"
                "No frame available yet";
            send_all(client, err, strlen(err));
        } else {
            char hdr[256];
            int hlen = std::snprintf(hdr, sizeof(hdr),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: image/jpeg\r\n"
                "Content-Length: %zu\r\n"
                "Cache-Control: no-cache\r\n\r\n",
                frame.size());
            send_all(client, hdr, static_cast<size_t>(hlen));
            send_all(client, reinterpret_cast<const char*>(frame.data()), frame.size());
        }

    } else {
        // Serve index HTML
        char hdr[256];
        int hlen = std::snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html; charset=utf-8\r\n"
            "Content-Length: %zu\r\n\r\n",
            sizeof(k_index_html) - 1);
        send_all(client, hdr, static_cast<size_t>(hlen));
        send_all(client, k_index_html, sizeof(k_index_html) - 1);
    }

    ::closesocket(client);
}

// ════════════════════════════════════════════════════════════════════════════════
// StreamServer::Impl
// ════════════════════════════════════════════════════════════════════════════════
struct StreamServer::Impl {
    StreamServerConfig     cfg;
    std::atomic<bool>      running{false};

    FrameQueue             raw_queue;   // camera → pipeline
    LatestJpeg             latest_jpeg; // pipeline → HTTP/getFrame

    std::thread            camera_thread;
    std::thread            pipeline_thread;
    std::thread            http_thread;
    SOCKET                 http_server_sock{INVALID_SOCKET};

    // ── Log callback ──────────────────────────────────────────────────────────
    std::mutex             log_mutex;
    StreamServer::LogFn    log_fn;

    // ── JPEG-ready callback ───────────────────────────────────────────────────
    std::mutex               jpeg_cb_mutex;
    StreamServer::JpegReadyFn jpeg_cb;

    // ── Raw-frame callback (for recording) ────────────────────────────────────
    std::mutex                raw_frame_cb_mutex;
    StreamServer::RawFrameFn  raw_frame_cb;

    // ── FPS measurement (video frames only, 100-frame sliding window) ─────────
    // Timestamps of the start and most-recent frame in the current window.
    // Reset on each camera reconnect so a fresh measurement begins per session.
    std::chrono::steady_clock::time_point fps_window_start;
    size_t                                fps_frame_count {0};

    void log(const char* fmt, ...) {
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        std::lock_guard<std::mutex> lk(log_mutex);
        if (log_fn) log_fn(buf);
    }

    // ── Camera thread: connect DVR, stream raw frames ─────────────────────────
    void run_camera() {
        // Determine which local IP to use for the DVR connection once.
        // On a multi-NIC host (WiFi + wired) the OS may otherwise route
        // through the wrong interface.  SIO_ROUTING_INTERFACE_QUERY asks
        // Windows "what local address would you use to reach this dest?"
        const std::string local_ip =
            find_outbound_local_ip(cfg.dvr_host, cfg.dvr_port);
        if (!local_ip.empty())
            log("DVR: outbound local IP = %s (binding socket)", local_ip.c_str());
        else
            log("DVR: outbound local IP auto-detect failed — OS will choose");

        while (running.load()) {
            log("DVR: connecting to %s:%d ...", cfg.dvr_host.c_str(), cfg.dvr_port);

            auto cam = std::make_unique<DVRIPCam>(
                cfg.dvr_host, cfg.dvr_user, cfg.dvr_password,
                "tcp", cfg.dvr_port
            );

            // Bind to the correct NIC so traffic goes via the wired adapter.
            if (!local_ip.empty())
                cam->set_bind_ip(local_ip);

            // Forward DVR-internal log messages (Claim/Start/first-frame) to
            // the stream-server log so they appear in the UI terminal.
            cam->set_log_callback([this](const char* msg) { log("%s", msg); });

            if (!cam->login()) {
                log("DVR: login failed (%s) — retrying in 5 s",
                    cam->last_error().c_str());
                std::this_thread::sleep_for(std::chrono::seconds(5));
                continue;
            }

            log("DVR: login OK  session=0x%08X  stream=%s",
                cam->session_id(), cfg.stream_type.c_str());

            raw_queue.closed = false;
            fps_frame_count  = 0;   // reset FPS measurement for this session

            cam->start_monitor(
                [this](const uint8_t* data, size_t size, const FrameMeta& meta) {
                    if (size == 0) return;
                    if (meta.frame.empty() && meta.type.empty()) return;

                    // Fire raw-frame callback for video frames only (skip audio/info).
                    // Used by VideoRecorder for lossless MP4 recording.
                    // Note: meta.type may be empty on some cameras even for valid
                    // video frames — only exclude known non-video types.
                    if (meta.type != "g711a" && meta.type != "info") {
                        // ── FPS measurement: 100-frame sliding window ─────────
                        auto now = std::chrono::steady_clock::now();
                        if (fps_frame_count == 0)
                            fps_window_start = now;
                        ++fps_frame_count;
                        if (fps_frame_count % 100 == 0) {
                            double elapsed = std::chrono::duration<double>(
                                now - fps_window_start).count();
                            // 99 inter-frame intervals span 100 frames
                            double fps = (elapsed > 0.0) ? 99.0 / elapsed : 0.0;
                            log("DVR: measured FPS = %.2f  "
                                "(100 frames in %.3f s, total=%zu)",
                                fps, elapsed, fps_frame_count);
                            fps_window_start = now;   // slide window forward
                        }

                        std::lock_guard<std::mutex> lk(raw_frame_cb_mutex);
                        if (raw_frame_cb)
                            raw_frame_cb(data, size, meta.type, meta.frame == "I");
                    }

                    RawFrame f;
                    f.data.assign(data, data + size);
                    f.meta = meta;
                    raw_queue.push(std::move(f));
                },
                cfg.stream_type
            );

            // Block until the monitor exits — either because:
            //   a) running was set to false (server stopping), or
            //   b) the camera dropped the connection (recv_failed in monitor_loop)
            // Do NOT call stop_monitor() yet — that would kill the thread before
            // it has a chance to stream.  Let it run until it signals itself done.
            while (running.load() && cam->is_monitoring()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }

            log("DVR: monitor stopped — reconnecting in 5 s");
            cam->stop_monitor();   // joins thread + cleanup (fast: already exited)
            cam->close();

            raw_queue.close();
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    }

    // ── Pipeline thread: feed ffmpeg, collect MJPEG ───────────────────────────
    void run_pipeline() {
        while (running.load()) {
            // Wait for first I-frame to detect codec
            std::string codec = "h264";
            RawFrame first_iframe;
            bool got_first = false;

            log("ffmpeg: waiting for first I-frame from DVR...");
            int wait_count = 0;
            while (running.load() && !got_first) {
                RawFrame f;
                if (!raw_queue.pop(f, std::chrono::milliseconds(20000))) {
                    ++wait_count;
                    log("ffmpeg: still waiting for I-frame (%d x 20 s)...", wait_count);
                    continue;
                }
                if (f.meta.frame == "I") {
                    std::string detected = detect_codec(f.data.data(), f.data.size());
                    if (!detected.empty())         codec = detected;
                    else if (!f.meta.type.empty()) codec = f.meta.type;

                    log("ffmpeg: codec=%s  %dx%d @ %d fps  first-bytes: %02x%02x%02x%02x",
                        codec.c_str(), f.meta.width, f.meta.height, f.meta.fps,
                        f.data.size() > 0 ? f.data[0] : 0,
                        f.data.size() > 1 ? f.data[1] : 0,
                        f.data.size() > 2 ? f.data[2] : 0,
                        f.data.size() > 3 ? f.data[3] : 0);

                    first_iframe = std::move(f);
                    got_first = true;
                }
            }
            if (!running.load()) break;

            // Start ffmpeg
            if (cfg.jpeg_scale_w > 0 || cfg.jpeg_scale_h > 0)
                log("ffmpeg: launching  codec=%s  q=%d  scale=%dx%d",
                    codec.c_str(), cfg.jpeg_quality,
                    cfg.jpeg_scale_w, cfg.jpeg_scale_h);
            else
                log("ffmpeg: launching  codec=%s  q=%d  scale=native",
                    codec.c_str(), cfg.jpeg_quality);
            FfmpegProc ffmpeg;
            if (!ffmpeg.start(codec, cfg.jpeg_quality,
                              cfg.jpeg_scale_w, cfg.jpeg_scale_h)) {
                log("ffmpeg: FAILED to start — is 'ffmpeg' in PATH? Retrying in 5 s");
                std::this_thread::sleep_for(std::chrono::seconds(5));
                continue;
            }
            log("ffmpeg: running — decoding to MJPEG");

            // Feed first frame
            ffmpeg.write(first_iframe.data.data(), first_iframe.data.size());

            // Reader thread: extract JPEG frames from ffmpeg stdout
            std::atomic<bool> pipe_ok{true};
            std::thread reader([&]() {
                std::vector<uint8_t> buf;
                buf.reserve(512 * 1024);

                uint8_t chunk[8192];
                while (running.load() && pipe_ok.load()) {
                    int n = ffmpeg.read(chunk, sizeof(chunk));
                    if (n < 0) { pipe_ok.store(false); break; }
                    if (n == 0) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                        continue;
                    }
                    buf.insert(buf.end(), chunk, chunk + n);

                    // Extract complete JPEG frames (\xFF\xD8 ... \xFF\xD9)
                    while (true) {
                        auto* d = buf.data();
                        size_t sz = buf.size();

                        // Find SOI
                        size_t soi = std::string::npos;
                        for (size_t i = 0; i + 1 < sz; ++i) {
                            if (d[i] == 0xFF && d[i+1] == 0xD8) { soi = i; break; }
                        }
                        if (soi == std::string::npos) {
                            if (sz > 2) buf.erase(buf.begin(), buf.end() - 2);
                            break;
                        }

                        // Find EOI after SOI
                        size_t eoi = std::string::npos;
                        for (size_t i = soi + 2; i + 1 < sz; ++i) {
                            if (d[i] == 0xFF && d[i+1] == 0xD9) { eoi = i + 2; break; }
                        }
                        if (eoi == std::string::npos) break;

                        // Complete JPEG
                        std::vector<uint8_t> jpeg(d + soi, d + eoi);
                        bool first_frame = latest_jpeg.get().empty();

                        // Fire JPEG-ready callback before moving (move empties the vector)
                        {
                            std::lock_guard<std::mutex> lk(jpeg_cb_mutex);
                            if (jpeg_cb) jpeg_cb(jpeg.data(), jpeg.size());
                        }

                        latest_jpeg.update(std::move(jpeg));
                        if (first_frame) log("ffmpeg: first JPEG frame ready — streaming");

                        buf.erase(buf.begin(), buf.begin() + eoi);
                    }
                }
            });

            // Feed frames from camera queue → ffmpeg stdin
            while (running.load() && pipe_ok.load()) {
                RawFrame f;
                if (!raw_queue.pop(f, std::chrono::milliseconds(10000))) {
                    if (!running.load()) break;
                    continue;  // timed out, no frames
                }
                // Only feed video frames (skip audio/info)
                if (f.meta.type == "g711a" || f.meta.type == "info") continue;

                if (!ffmpeg.write(f.data.data(), f.data.size())) {
                    pipe_ok.store(false);
                    break;
                }
            }

            pipe_ok.store(false);
            ffmpeg.terminate();
            reader.join();

            log("ffmpeg: pipeline stopped — restarting in 3 s");
            std::this_thread::sleep_for(std::chrono::seconds(3));
        }
    }

    // ── HTTP server thread ────────────────────────────────────────────────────
    void run_http() {
        SOCKET srv = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (srv == INVALID_SOCKET) return;
        http_server_sock = srv;

        // Allow quick restart
        int opt = 1;
        setsockopt(srv, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&opt), sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(static_cast<u_short>(cfg.http_port));
        addr.sin_addr.s_addr = INADDR_ANY;

        if (::bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr))
                == SOCKET_ERROR) {
            log("HTTP: bind failed on port %d (port in use?)", cfg.http_port);
            ::closesocket(srv);
            http_server_sock = INVALID_SOCKET;
            return;
        }

        ::listen(srv, SOMAXCONN);
        log("HTTP: listening on port %d  →  http://localhost:%d/stream",
            cfg.http_port, cfg.http_port);

        // Set accept timeout so we can check running flag
        DWORD ms = 500;
        setsockopt(srv, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&ms), sizeof(ms));

        while (running.load()) {
            SOCKET client = ::accept(srv, nullptr, nullptr);
            if (client == INVALID_SOCKET) continue;  // timeout or error

            std::thread([client, this]() {
                http_serve_client(client, &latest_jpeg, &running);
            }).detach();
        }

        ::closesocket(srv);
        http_server_sock = INVALID_SOCKET;
    }
};

// ════════════════════════════════════════════════════════════════════════════════
// StreamServer public API
// ════════════════════════════════════════════════════════════════════════════════

StreamServer::StreamServer(StreamServerConfig config)
    : d_(std::make_unique<Impl>())
{
    d_->cfg = std::move(config);
}

StreamServer::~StreamServer() {
    stop();
}

void StreamServer::set_stream_type(const std::string& stream_type) {
    if (!d_->running.load())
        d_->cfg.stream_type = stream_type;
}

bool StreamServer::start() {
    if (d_->running.load()) return false;
    d_->running.store(true);

    d_->camera_thread   = std::thread([this]{ d_->run_camera(); });
    d_->pipeline_thread = std::thread([this]{ d_->run_pipeline(); });
    d_->http_thread     = std::thread([this]{ d_->run_http(); });

    return true;
}

void StreamServer::stop() {
    if (!d_->running.load()) return;
    d_->running.store(false);

    d_->raw_queue.close();

    // Unblock HTTP server accept
    if (d_->http_server_sock != INVALID_SOCKET)
        ::closesocket(d_->http_server_sock);

    if (d_->camera_thread.joinable())   d_->camera_thread.join();
    if (d_->pipeline_thread.joinable()) d_->pipeline_thread.join();
    if (d_->http_thread.joinable())     d_->http_thread.join();
}

bool StreamServer::is_running() const {
    return d_->running.load();
}

std::vector<uint8_t> StreamServer::get_latest_frame() const {
    return d_->latest_jpeg.get();
}

const StreamServerConfig& StreamServer::config() const {
    return d_->cfg;
}

void StreamServer::set_log_callback(LogFn fn) {
    std::lock_guard<std::mutex> lk(d_->log_mutex);
    d_->log_fn = std::move(fn);
}

void StreamServer::set_jpeg_callback(JpegReadyFn fn) {
    std::lock_guard<std::mutex> lk(d_->jpeg_cb_mutex);
    d_->jpeg_cb = std::move(fn);
}

void StreamServer::set_raw_frame_callback(RawFrameFn fn) {
    std::lock_guard<std::mutex> lk(d_->raw_frame_cb_mutex);
    d_->raw_frame_cb = std::move(fn);
}

} // namespace cppdvr
