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

    // JPEG encoding
    int         jpeg_quality = 5;   // ffmpeg -q:v: 1=best quality/largest … 31=smallest
    // Output resolution before JPEG encode.  0 = keep native camera resolution.
    // Set one dimension and leave the other 0 to preserve aspect ratio
    //   e.g. jpeg_scale_w=416, jpeg_scale_h=0  →  ffmpeg -vf scale=416:-1
    int         jpeg_scale_w = 0;
    int         jpeg_scale_h = 0;
};

class CPPDVR_API StreamServer {
public:
    explicit StreamServer(StreamServerConfig config = {});
    ~StreamServer();

    StreamServer(const StreamServer&)            = delete;
    StreamServer& operator=(const StreamServer&) = delete;

    // Override the stream type ("Main", "Sub", "Extra") before calling start().
    // Has no effect if the server is already running.
    void set_stream_type(const std::string& stream_type);

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

    // Optional callback fired for every decoded JPEG frame (from the ffmpeg
    // pipeline thread).  Use this to forward frames over UDP or any other
    // transport without polling get_latest_frame().
    // Set before start(); safe to replace at any time.
    using JpegReadyFn = std::function<void(const uint8_t* jpeg, size_t size)>;
    void set_jpeg_callback(JpegReadyFn fn);

    // Optional callback fired for every raw H264/H265 NAL video frame received
    // from the DVR (camera thread), before it enters the JPEG decode pipeline.
    // codec_hint is from the DVRIP frame header ("h264"/"h265"/empty).
    // is_iframe is true for keyframes (I-frames).
    // Use this for lossless MP4 recording via VideoRecorder.
    // Set before start(); safe to replace at any time.
    using RawFrameFn = std::function<void(const uint8_t* data, size_t size,
                                           const std::string& codec_hint,
                                           bool is_iframe)>;
    void set_raw_frame_callback(RawFrameFn fn);

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

} // namespace cppdvr
