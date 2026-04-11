#pragma once
/**
 * dvrip.h — C++ wrapper for the XiongMai DVRIP protocol
 *
 * Mirrors the Python dvrip.py DVRIPCam class.
 * Uses a PIMPL to hide platform headers (Winsock2, CryptoAPI) from consumers.
 */

#include "cppdvr_export.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace cppdvr {

// ── Frame metadata ─────────────────────────────────────────────────────────────
struct FrameMeta {
    std::string type;   // "h264", "h265", "mpeg4", "jpeg", "g711a", "info"
    std::string frame;  // "I" (keyframe) or "P" (delta frame)
    int fps    = 0;
    int width  = 0;     // already multiplied by 8 (raw value * 8)
    int height = 0;

    struct Dt {
        int year = 0, month = 0, day = 0;
        int hour = 0, minute = 0, second = 0;
    };
    using dt_type = Dt;
    Dt dt;
};

// Callback invoked for each received video/audio frame.
// data  – raw NAL/payload bytes (Annex B for H264/H265)
// size  – byte count
// meta  – frame metadata
using FrameCallback = std::function<void(const uint8_t* data, size_t size,
                                         const FrameMeta& meta)>;

// ── DVRIPCam ───────────────────────────────────────────────────────────────────
class CPPDVR_API DVRIPCam {
public:
    static constexpr int kTcpPort = 34567;
    static constexpr int kUdpPort = 34568;

    // port=0  → use default for the chosen proto ("tcp"/"udp")
    explicit DVRIPCam(std::string ip,
                      std::string user     = "admin",
                      std::string password = "",
                      std::string proto    = "tcp",
                      int         port     = 0);
    ~DVRIPCam();

    DVRIPCam(const DVRIPCam&)            = delete;
    DVRIPCam& operator=(const DVRIPCam&) = delete;

    // ── Connection ─────────────────────────────────────────────────────────────
    bool connect(int timeout_sec = 10);
    void close();
    bool login();               // connect() + authenticate + start keep-alive
    bool is_connected() const;

    // Bind outbound socket to a specific local IP before connecting.
    // Call before login() when the host has multiple NICs (e.g., WiFi + wired).
    // If not set, the OS picks the interface based on its routing table.
    void set_bind_ip(const std::string& local_ip);

    // ── Live streaming ─────────────────────────────────────────────────────────
    // Runs the receive loop on a background thread; returns immediately.
    // stream = "Main" (full-res) or "Extra" (sub-stream)
    bool start_monitor(FrameCallback callback,
                       const std::string& stream = "Main");
    void stop_monitor();
    // Returns true while the background monitor thread is running.
    // Becomes false when stop_monitor() is called OR when a socket error exits the loop.
    bool is_monitoring() const;

    // ── Snapshot ───────────────────────────────────────────────────────────────
    // Returns JPEG bytes; empty on failure.
    std::vector<uint8_t> snapshot(int channel = 0);

    // ── PTZ control ────────────────────────────────────────────────────────────
    // cmd: "DirectionUp","DirectionDown","DirectionLeft","DirectionRight",
    //      "ZoomTile","ZoomWide","SetPreset","GotoPreset","StartTour","StopTour",...
    bool ptz(const std::string& cmd, int step = 5,
             int preset = -1, int channel = 0);

    // ── Generic protocol interface ─────────────────────────────────────────────
    // Returns parsed JSON response (or empty object on error).
    // These match the Python get_command/set_command semantics.
    std::string get_command(const std::string& command, int code = 0);
    std::string set_command(const std::string& command,
                            const std::string& data_json, int code = 0);

    // ── Utilities ──────────────────────────────────────────────────────────────
    // Sofia password hash used by XiongMai DVR login.
    static std::string sofia_hash(const std::string& password);

    uint32_t    session_id()    const;
    std::string last_error()    const;

    // ── Log callback ───────────────────────────────────────────────────────────
    // Optional; called from background threads with status/error messages.
    // Set before start_monitor(); safe to call from any thread.
    using LogFn = std::function<void(const char* msg)>;
    void set_log_callback(LogFn fn);

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

} // namespace cppdvr
