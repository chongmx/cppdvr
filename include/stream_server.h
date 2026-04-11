#pragma once
/**
 * stream_server.h — DVR-to-MJPEG HTTP streaming server
 *
 * Mirrors the Python stream_server.py logic:
 *   1. Connects to DVR via DVRIP and receives raw H264/H265 frames.
 *   2. Auto-detects the actual codec from the bitstream (DVR metadata often lies).
 *   3. Decodes via a child ffmpeg process (pipe:0 → pipe:1 MJPEG).
 *   4. Serves MJPEG over HTTP on a configurable port.
 *
 * HTTP endpoints:
 *   GET /          – HTML page embedding the live stream
 *   GET /stream    – multipart/x-mixed-replace MJPEG stream
 *   GET /snapshot  – single JPEG
 */

#include "cppdvr_export.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace cppdvr {

struct StreamServerConfig {
    // DVR camera
    std::string dvr_host     = "172.20.80.12";
    int         dvr_port     = 34567;
    std::string dvr_user     = "admin";
    std::string dvr_password = "";
    std::string stream_type  = "Main";   // "Main" or "Extra"

    // HTTP server
    std::string http_host    = "0.0.0.0";
    int         http_port    = 8080;
    int         jpeg_quality = 5;        // ffmpeg -q:v 1=best 31=smallest
};

class CPPDVR_API StreamServer {
public:
    explicit StreamServer(StreamServerConfig config = {});
    ~StreamServer();

    StreamServer(const StreamServer&)            = delete;
    StreamServer& operator=(const StreamServer&) = delete;

    // Start background threads (camera, ffmpeg pipeline, HTTP server).
    // Returns true if all threads launched successfully.
    bool start();

    // Stop all background threads and close the HTTP server.
    void stop();

    bool is_running() const;

    // Get a copy of the latest decoded JPEG frame.
    // Returns empty vector if no frame is available yet.
    std::vector<uint8_t> get_latest_frame() const;

    const StreamServerConfig& config() const;

    // Optional log/status callback — called from background threads.
    // Set before start(); replaces any previously registered callback.
    using LogFn = std::function<void(const char* msg)>;
    void set_log_callback(LogFn fn);

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

} // namespace cppdvr
