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

// Hardware accelerator selection for H.264/H.265 decode (streaming pipeline).
// The library probes available hwaccels once at first use and caches results.
enum class DecodeAccel {
    Software = 0,  // always software decode
    CUDA     = 1,  // NVIDIA NVDEC  (-hwaccel cuda)
    OtherHW  = 2,  // platform HW: d3d11va (Windows) / vaapi (Linux)
    Auto     = 3,  // try OtherHW → CUDA → software  (default)
};

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

    // H.264/H.265 decode acceleration.  Auto probes available hwaccels (d3d11va
    // on Windows, vaapi on Linux) and falls back to software if none work.
    // ffmpeg_hwaccel is a raw override: when non-empty it takes precedence over
    // decode_accel and is passed verbatim as -hwaccel <value>.
    DecodeAccel decode_accel    = DecodeAccel::Auto;
    std::string ffmpeg_hwaccel  = "";   // raw override; prefer decode_accel

    // When true, StreamServer::start() automatically selects the fastest available
    // JPEG backend (libjpeg-turbo > stb_image) if the process-global backend is
    // still at the default (STB). Set false if you want to manage the backend
    // yourself via cppdvr_set_jpeg_backend() before calling start().
    bool        auto_jpeg_backend = true;
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
    // NOTE: This slot is typically owned by the GUI application for display.
    //       For overlay recording use set_overlay_jpeg_callback() instead.
    using JpegReadyFn = std::function<void(const uint8_t* jpeg, size_t size)>;
    void set_jpeg_callback(JpegReadyFn fn);

    // Fired with the POST-overlay JPEG for every frame (after any overlay
    // text/frame callbacks have been applied and re-encoded).
    // When no overlay callbacks are set the data is identical to jpeg_cb.
    // Use this slot for recording the overlaid stream so that the GUI can
    // independently use set_jpeg_callback() for display without conflict.
    using OverlayJpegReadyFn = std::function<void(const uint8_t* jpeg, size_t size)>;
    void set_overlay_jpeg_callback(OverlayJpegReadyFn fn);

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

    // ── Text overlay constants ─────────────────────────────────────────────────

    // Maximum text per overlay region (bytes incl. null terminator).
    // Excess is silently truncated.
    static constexpr size_t kOverlayMaxText = 512;

    // Maximum simultaneous text boxes.
    // Recompile with -DSTREAM_OVERLAY_MAX_BOXES=N to raise the limit.
#ifndef STREAM_OVERLAY_MAX_BOXES
#  define STREAM_OVERLAY_MAX_BOXES 4
#endif
    static constexpr int kOverlayMaxBoxes = STREAM_OVERLAY_MAX_BOXES;

    enum class OverlayAlign  { Left = 0, Right = 1 };
    enum class OverlayAnchor {
        TopLeft     = 0,
        TopRight    = 1,
        BottomLeft  = 2,
        BottomRight = 3
    };

    // ── Push-based single-region overlay ─────────────────────────────────────
    // Quick single-box overlay (no word-wrap, auto bottom-left position).
    // For word-wrap, alignment, and multiple regions use overlay_box_* below.
    // '\n' in text starts a new line at the same x origin.

    // Set the pixel origin for the top-left of the first character.
    // Pass x=-1, y=-1 to use the auto default: bottom-left with a margin that
    // scales proportionally with the frame height.
    void overlay_set_cursor(int x, int y);

    // Set the glyph scale factor (each font pixel becomes scale×scale screen pixels).
    // 1 = native 8×8 px glyphs.  Pass 0 for auto-scale (frame_height / 400).
    void overlay_set_scale(int scale);

    // Replace the current overlay text.  Supports '\n' for multiple lines.
    // Thread-safe; safe to call at any rate from any thread.
    void overlay_print(const char* text);

    // Clear the overlay (no text drawn until next overlay_print).
    void overlay_clear();

    // ── Text box overlay (multiple regions, word-wrap, alignment) ────────────
    // Up to kOverlayMaxBoxes independent boxes, each with its own position,
    // size, scale, alignment, and anchor corner.
    //
    // Box layout:
    //   x, y   — inward pixel offset from the anchor corner.
    //   box_w  — text-box width in pixels:
    //              • controls word-wrap (text wider than box_w wraps to next line)
    //              • required for OverlayAlign::Right (right edge = x + box_w)
    //              • 0 = unconstrained (only '\n' causes line breaks)
    //   scale  — glyph scale factor (0 = auto: frame_height / 400, min 1).
    //   align  — Left or Right within box_w.
    //   anchor — which image corner x,y are measured from.
    //
    // All calls are thread-safe.  Text is double-buffered: the renderer always
    // sees a fully-committed snapshot, never a partially-written string.
    // '\n' in text always forces an explicit line break.

    // Configure a box (call before overlay_box_print; idx must be 0…kOverlayMaxBoxes-1).
    void overlay_box_configure(int idx, int x, int y, int box_w,
                               int scale, OverlayAlign align,
                               OverlayAnchor anchor = OverlayAnchor::TopLeft);

    // Set display text for box idx.  Thread-safe.
    void overlay_box_print(int idx, const char* text);

    // Hide box idx (stops drawing until next overlay_box_print).
    void overlay_box_clear(int idx);

    // Hide all boxes.
    void overlay_box_clear_all();

    // ── JPEG decode/encode backend ────────────────────────────────────────────────
    // All stream instances in this process share the active backend.
    // Switch at any time; takes effect on the next overlay decode/encode call.
    enum class JpegBackend {
        STB           = 0,  // stb_image (default, always available, no extra deps)
        LibJpegTurbo  = 1,  // libjpeg-turbo: SIMD (SSE2/AVX2/NEON) — cross-platform
        NvJPEG        = 2,  // nvJPEG: NVIDIA GPU — lowest latency at high resolution
    };

    // Returns true if this backend was compiled in and hardware is available.
    // Calling jpeg_backend_available(NvJPEG) performs a lazy GPU init on the first call.
    static bool jpeg_backend_available(JpegBackend backend);

    // Switch the active backend. Returns false if unavailable (current backend unchanged).
    bool set_jpeg_backend(JpegBackend backend);

    // Return the currently active backend.
    JpegBackend get_jpeg_backend() const;

    // ── Frame overlay callback ─────────────────────────────────────────────────
    // For custom drawing: called with the raw RGB buffer every frame.
    // Triggers a full JPEG decode → RGB → re-encode cycle when set.
    // rgb points to width * height * 3 bytes (packed R, G, B; no padding).
    using OverlayFrameFn = std::function<void(uint8_t* rgb, uint64_t ts_us,
                                               int w, int h)>;
    void set_overlay_frame_callback(OverlayFrameFn fn);

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

} // namespace cppdvr
