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

#include "stream_server.h"
#include "dvrip.h"
#include "platform/platform_net.h"
#include "platform/platform_process.h"
#include "jpeg_overlay.h"

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdarg>
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
// Network helper — find the local IP that routes to a destination
// Handles multi-NIC hosts (e.g., WiFi + wired): ensures the TCP connection
// to the DVR goes out on the interface that is on the same subnet.
// ════════════════════════════════════════════════════════════════════════════════
static std::string find_outbound_local_ip(const std::string& dest_ip, int dest_port) {
    std::string local_ip;
    if (platform_get_local_ip_for_remote(dest_ip, local_ip) != PlatformSocketError::Success) {
        return "";
    }
    // 0.0.0.0 means "any interface" — not useful for explicit binding
    if (local_ip == "0.0.0.0") return "";
    return local_ip;
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
// ffmpeg process wrapper (cross-platform via platform_process)
// ════════════════════════════════════════════════════════════════════════════════
struct FfmpegProc {
    PlatformProcess proc        {INVALID_PROCESS};
    PlatformPipe    stdin_pipe  {INVALID_PIPE};
    PlatformPipe    stdout_pipe {INVALID_PIPE};
    PlatformPipe    stderr_pipe {INVALID_PIPE};

    bool start(const std::string& codec, int jpeg_quality,
               int jpeg_scale_w = 0, int jpeg_scale_h = 0) {
        std::string fmt = "h264";
        if (codec == "h265")  fmt = "hevc";
        else if (codec == "mpeg4") fmt = "mpeg4";

        std::vector<std::string> args = {
            "-loglevel", "warning",
            "-f", fmt, "-i", "pipe:0"
        };

        // Optional scale filter
        if (jpeg_scale_w > 0 || jpeg_scale_h > 0) {
            int w = jpeg_scale_w > 0 ? jpeg_scale_w : -1;
            int h = jpeg_scale_h > 0 ? jpeg_scale_h : -1;
            args.push_back("-vf");
            args.push_back("scale=" + std::to_string(w) + ":" + std::to_string(h));
        }

        args.push_back("-f");
        args.push_back("mjpeg");
        args.push_back("-q:v");
        args.push_back(std::to_string(jpeg_quality));
        args.push_back("pipe:1");

        proc = platform_spawn_process("ffmpeg", args, stdin_pipe, stdout_pipe, stderr_pipe);
        return proc != INVALID_PROCESS;
    }

    bool write(const uint8_t* data, size_t len) {
        if (stdin_pipe == INVALID_PIPE || len == 0) return false;
        return platform_write_pipe(stdin_pipe, data, static_cast<int>(len))
               == static_cast<int>(len);
    }

    // Blocking read from stdout — returns 0 on EOF, <0 on error.
    int read(uint8_t* buf, size_t buf_size) {
        if (stdout_pipe == INVALID_PIPE) return -1;
        return platform_read_pipe(stdout_pipe, buf, static_cast<int>(buf_size));
    }

    void terminate() {
        if (stdin_pipe != INVALID_PIPE) {
            platform_close_pipe(stdin_pipe);
            stdin_pipe = INVALID_PIPE;
        }
        if (proc != INVALID_PROCESS) {
            platform_kill_process(proc);
            proc = INVALID_PROCESS;
        }
        if (stdout_pipe != INVALID_PIPE) {
            platform_close_pipe(stdout_pipe);
            stdout_pipe = INVALID_PIPE;
        }
        if (stderr_pipe != INVALID_PIPE) {
            platform_close_pipe(stderr_pipe);
            stderr_pipe = INVALID_PIPE;
        }
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

static void send_all(PlatformSocket s, const char* data, size_t len) {
    while (len > 0) {
        int r = platform_send(s, data, static_cast<int>(len), 30000);
        if (r <= 0) break;
        data += r;
        len  -= r;
    }
}

static void http_serve_client(PlatformSocket client, LatestJpeg* latest,
                               std::atomic<bool>* running) {
    char req_buf[1024] = {};
    int  nread = platform_recv(client, req_buf, sizeof(req_buf) - 1, 30000);
    if (nread <= 0) { platform_close(client); return; }

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

    platform_close(client);
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
    PlatformSocket         http_server_sock{INVALID_PLATFORM_SOCKET};

    // ── Log callback ──────────────────────────────────────────────────────────
    std::mutex             log_mutex;
    StreamServer::LogFn    log_fn;

    // ── JPEG-ready callback ───────────────────────────────────────────────────
    std::mutex               jpeg_cb_mutex;
    StreamServer::JpegReadyFn jpeg_cb;

    // ── Raw-frame callback (for recording) ────────────────────────────────────
    std::mutex                raw_frame_cb_mutex;
    StreamServer::RawFrameFn  raw_frame_cb;

    // ── Frame overlay callback (custom RGB drawing) ───────────────────────────
    std::mutex                    overlay_mutex;
    StreamServer::OverlayFrameFn  overlay_frame_cb;
    std::atomic<uint64_t>         overlay_frame_count{0};

    // ── Push-based text overlay (thread-safe double buffer) ───────────────────
    // back  — user writes here (protected by text_mu)
    // front — renderer reads from here (swapped under text_mu when dirty)
    static constexpr size_t kMaxText = StreamServer::kOverlayMaxText;
    std::mutex text_mu;
    char       text_back [kMaxText]{};
    char       text_front[kMaxText]{};
    bool       text_dirty   {false};
    int        text_cursor_x{-1};
    int        text_cursor_y{-1};
    int        text_scale   { 0};   // 0 = auto (frame_height / 400)

    struct TextSnapshot {
        char text[kMaxText];
        int  cx, cy, scale;
    };

    // Called by user thread — O(kMaxText) copy under lock, then returns.
    void text_print(const char* src) {
        std::lock_guard<std::mutex> lk(text_mu);
        std::strncpy(text_back, src ? src : "", kMaxText - 1);
        text_back[kMaxText - 1] = '\0';
        text_dirty = true;
    }
    void text_clear() {
        std::lock_guard<std::mutex> lk(text_mu);
        text_back[0] = '\0';
        text_dirty   = true;
    }
    void text_set_cursor(int x, int y) {
        std::lock_guard<std::mutex> lk(text_mu);
        text_cursor_x = x;
        text_cursor_y = y;
    }
    void text_set_scale(int s) {
        std::lock_guard<std::mutex> lk(text_mu);
        text_scale = s;
    }

    // Called by renderer thread — swaps buffers if dirty, returns snapshot.
    TextSnapshot text_snap() {
        TextSnapshot s{};
        std::lock_guard<std::mutex> lk(text_mu);
        if (text_dirty) {
            std::memcpy(text_front, text_back, kMaxText);
            text_dirty = false;
        }
        std::memcpy(s.text, text_front, kMaxText);
        s.cx    = text_cursor_x;
        s.cy    = text_cursor_y;
        s.scale = text_scale;
        return s;
    }

    // ── Text box overlay (word-wrap, alignment, anchor) ───────────────────────
    struct BoxState {
        static constexpr size_t kBuf = StreamServer::kOverlayMaxText;

        std::mutex mu;
        // Config — written under mu
        int  x      {0};
        int  y      {0};
        int  box_w  {0};
        int  scale  {0};    // 0 = auto
        int  align  {0};    // 0 = left, 1 = right
        int  anchor {0};    // OVERLAY_ANCHOR_*
        bool active {false};
        // Double-buffered text
        char back [kBuf]{};
        char front[kBuf]{};
        bool dirty {false};

        void configure(int _x, int _y, int _bw, int _sc, int _al, int _an) {
            std::lock_guard<std::mutex> lk(mu);
            x = _x; y = _y; box_w = _bw; scale = _sc;
            align = _al; anchor = _an; active = true;
        }
        void print(const char* src) {
            std::lock_guard<std::mutex> lk(mu);
            std::strncpy(back, src ? src : "", kBuf - 1);
            back[kBuf - 1] = '\0';
            dirty = true;
        }
        void clear() {
            std::lock_guard<std::mutex> lk(mu);
            back[0] = '\0'; dirty = true;
        }

        struct Snap {
            char text[kBuf];
            int  x, y, box_w, scale, align, anchor;
            bool active, empty;
        };
        Snap snap() {
            Snap s{};
            std::lock_guard<std::mutex> lk(mu);
            if (dirty) { std::memcpy(front, back, kBuf); dirty = false; }
            std::memcpy(s.text, front, kBuf);
            s.x = x; s.y = y; s.box_w = box_w;
            s.scale = scale; s.align = align; s.anchor = anchor;
            s.active = active; s.empty = (front[0] == '\0');
            return s;
        }
    } overlay_boxes[STREAM_OVERLAY_MAX_BOXES];

    // ── Overlay JPEG-ready callback (post-overlay, for recording) ─────────────
    std::mutex                       overlay_jpeg_cb_mutex;
    StreamServer::OverlayJpegReadyFn overlay_jpeg_cb;

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

            // Reader thread: extract JPEG frames from ffmpeg stdout (blocking read)
            std::atomic<bool> pipe_ok{true};
            std::thread reader([&]() {
                std::vector<uint8_t> buf;
                buf.reserve(512 * 1024);

                uint8_t chunk[8192];
                while (true) {
                    int n = ffmpeg.read(chunk, sizeof(chunk));
                    if (n <= 0) { pipe_ok.store(false); break; }  // EOF or error
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

                        // ── Apply overlay (decode → draw → re-encode) ────────
                        {
                            // Snapshot all overlay state before touching pixels
                            StreamServer::OverlayFrameFn frame_fn;
                            {
                                std::lock_guard<std::mutex> lk(overlay_mutex);
                                frame_fn = overlay_frame_cb;
                            }
                            auto tsnap = text_snap();

                            BoxState::Snap bsnaps[STREAM_OVERLAY_MAX_BOXES];
                            bool has_boxes = false;
                            for (int bi = 0; bi < STREAM_OVERLAY_MAX_BOXES; ++bi) {
                                bsnaps[bi] = overlay_boxes[bi].snap();
                                if (bsnaps[bi].active && !bsnaps[bi].empty)
                                    has_boxes = true;
                            }

                            bool has_text  = tsnap.text[0] != '\0';
                            bool needs_rgb = (frame_fn != nullptr)
                                             || has_text || has_boxes;

                            if (needs_rgb) {
                                int w = 0, h = 0;
                                uint8_t* rgb = jpeg_decode_rgb(
                                    jpeg.data(), jpeg.size(), &w, &h);
                                if (rgb) {
                                    uint64_t ts = ++overlay_frame_count * 40000u;

                                    // Custom frame callback
                                    if (frame_fn) frame_fn(rgb, ts, w, h);

                                    // Single-region legacy overlay
                                    if (has_text) {
                                        int sc = tsnap.scale > 0
                                                 ? tsnap.scale
                                                 : (h > 400 ? h / 400 : 1);
                                        int ox = tsnap.cx >= 0 ? tsnap.cx : 8 * sc;
                                        int oy;
                                        if (tsnap.cy >= 0) {
                                            oy = tsnap.cy;
                                        } else {
                                            int nln = 1;
                                            for (const char* p = tsnap.text; *p; ++p)
                                                if (*p == '\n') ++nln;
                                            int bh = nln * 8 * sc;
                                            oy = (h > bh) ? h - bh : 0;
                                        }
                                        jpeg_overlay_draw_text(
                                            rgb, w, h, tsnap.text,
                                            ox, oy, 255, 255, 255, sc);
                                    }

                                    // Text boxes
                                    for (int bi = 0; bi < STREAM_OVERLAY_MAX_BOXES; ++bi) {
                                        const auto& b = bsnaps[bi];
                                        if (!b.active || b.empty) continue;
                                        int sc = b.scale > 0
                                                 ? b.scale
                                                 : (h > 400 ? h / 400 : 1);
                                        jpeg_overlay_draw_textbox(
                                            rgb, w, h, b.text,
                                            b.x, b.y, b.box_w,
                                            255, 255, 255,
                                            sc, b.align, b.anchor);
                                    }

                                    uint8_t* enc = nullptr;
                                    size_t   esz = 0;
                                    if (jpeg_encode_rgb(rgb, w, h, 90, &enc, &esz) && enc) {
                                        jpeg.assign(enc, enc + esz);
                                        free(enc);
                                    }
                                    free(rgb);
                                }
                            }
                        }
                        // ── End overlay ──────────────────────────────────────

                        // Fire JPEG-ready callback before moving (move empties the vector)
                        {
                            std::lock_guard<std::mutex> lk(jpeg_cb_mutex);
                            if (jpeg_cb) jpeg_cb(jpeg.data(), jpeg.size());
                        }

                        // Fire overlay JPEG callback (post-overlay; used by recorder
                        // so the GUI can independently use jpeg_cb for display)
                        {
                            std::lock_guard<std::mutex> lk(overlay_jpeg_cb_mutex);
                            if (overlay_jpeg_cb) overlay_jpeg_cb(jpeg.data(), jpeg.size());
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
        platform_net_init();

        PlatformSocket srv = platform_socket();
        if (srv == INVALID_PLATFORM_SOCKET) return;
        http_server_sock = srv;

        platform_set_reuseaddr(srv);

        if (platform_bind(srv, "0.0.0.0", static_cast<uint16_t>(cfg.http_port))
                != PlatformSocketError::Success) {
            log("HTTP: bind failed on port %d (port in use?)", cfg.http_port);
            platform_close(srv);
            http_server_sock = INVALID_PLATFORM_SOCKET;
            return;
        }

        platform_listen(srv, 16);
        log("HTTP: listening on port %d  →  http://localhost:%d/stream",
            cfg.http_port, cfg.http_port);

        // 500 ms accept timeout — lets us check running flag periodically
        platform_set_recv_timeout(srv, 500);

        while (running.load()) {
            PlatformSocket client = platform_accept(srv);
            if (client == INVALID_PLATFORM_SOCKET) continue;  // timeout or error

            std::thread([client, this]() {
                http_serve_client(client, &latest_jpeg, &running);
            }).detach();
        }

        platform_close(srv);
        http_server_sock = INVALID_PLATFORM_SOCKET;
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
    if (d_->http_server_sock != INVALID_PLATFORM_SOCKET)
        platform_close(d_->http_server_sock);

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

void StreamServer::overlay_set_cursor(int x, int y) { d_->text_set_cursor(x, y); }
void StreamServer::overlay_set_scale(int scale)     { d_->text_set_scale(scale); }
void StreamServer::overlay_print(const char* text)  { d_->text_print(text); }
void StreamServer::overlay_clear()                  { d_->text_clear(); }

void StreamServer::overlay_box_configure(int idx, int x, int y, int box_w,
                                          int scale, OverlayAlign align,
                                          OverlayAnchor anchor) {
    if (idx < 0 || idx >= kOverlayMaxBoxes) return;
    d_->overlay_boxes[idx].configure(x, y, box_w, scale,
                                      static_cast<int>(align),
                                      static_cast<int>(anchor));
}
void StreamServer::overlay_box_print(int idx, const char* text) {
    if (idx < 0 || idx >= kOverlayMaxBoxes) return;
    d_->overlay_boxes[idx].print(text);
}
void StreamServer::overlay_box_clear(int idx) {
    if (idx < 0 || idx >= kOverlayMaxBoxes) return;
    d_->overlay_boxes[idx].clear();
}
void StreamServer::overlay_box_clear_all() {
    for (int i = 0; i < kOverlayMaxBoxes; ++i)
        d_->overlay_boxes[i].clear();
}

void StreamServer::set_overlay_frame_callback(OverlayFrameFn fn) {
    std::lock_guard<std::mutex> lk(d_->overlay_mutex);
    d_->overlay_frame_cb = std::move(fn);
}

void StreamServer::set_overlay_jpeg_callback(OverlayJpegReadyFn fn) {
    std::lock_guard<std::mutex> lk(d_->overlay_jpeg_cb_mutex);
    d_->overlay_jpeg_cb = std::move(fn);
}

bool StreamServer::jpeg_backend_available(JpegBackend backend) {
    return ::jpeg_backend_available(static_cast<int>(backend)) != 0;
}

bool StreamServer::set_jpeg_backend(JpegBackend backend) {
    return ::jpeg_backend_set(static_cast<int>(backend)) != 0;
}

StreamServer::JpegBackend StreamServer::get_jpeg_backend() const {
    return static_cast<JpegBackend>(::jpeg_backend_get());
}

} // namespace cppdvr
