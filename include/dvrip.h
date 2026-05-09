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

    // ── Time ───────────────────────────────────────────────────────────────────
    struct DeviceTime {
        int year = 2000, month = 1, day = 1;
        int hour = 0, minute = 0, second = 0;
    };
    bool get_time(DeviceTime& out);
    bool set_time(const DeviceTime& dt);

    // ── OSD / Text overlay ─────────────────────────────────────────────────────
    bool set_channel_titles(const std::vector<std::string>& titles);
    bool set_channel_bitmap(int width, int height,
                            const std::vector<uint8_t>& bitmap_data);

    // ── Reboot ─────────────────────────────────────────────────────────────────
    bool reboot();

    // ── Encoding settings  (Simplify.Encode) ──────────────────────────────────
    struct VideoStreamFormat {
        std::string compression;  // "H.264", "H.265", "MPEG4"
        std::string resolution;   // "5M","4M","3M","1080P","720P","D1","HD1","CIF"
        std::string bitrate_ctrl; // "VBR" or "CBR"
        int bitrate    = 0;       // kbps
        int fps        = 25;
        int gop        = 2;       // keyframe interval (seconds)
        int quality    = 4;       // 1–6
        bool video_en  = true;
        bool audio_en  = false;
    };
    struct EncodeConfig {
        VideoStreamFormat main;
        VideoStreamFormat extra;
    };
    // channel=0 for single-channel cameras
    bool get_encode_config(EncodeConfig& out, int channel = 0);
    bool set_encode_config(const EncodeConfig& cfg, int channel = 0);

    // ── Camera / video-color parameters  (AVEnc.VideoColor) ───────────────────
    struct VideoColorParam {
        int brightness   = 50;   // 0–100
        int contrast     = 50;   // 0–100
        int saturation   = 50;   // 0–100
        int hue          = 50;   // 0–100
        int sharpness    = 0;    // Acutance — camera-specific range
        int gain         = 0;    // 0–100
        int whitebalance = 128;  // 0–255
    };
    // channel=0, time_section=0 (always-on section)
    bool get_video_color(VideoColorParam& out, int channel = 0, int time_section = 0);
    bool set_video_color(const VideoColorParam& p, int channel = 0, int time_section = 0);

    // ── Network settings ───────────────────────────────────────────────────────
    struct NetworkInfo {
        std::string ip;
        std::string mask;
        std::string gateway;
        std::string dns;
        std::string hostname;
        std::string mac;
        int         tcp_port  = 34567;
        int         http_port = 80;
        bool        dhcp      = false;
    };
    bool get_network_info(NetworkInfo& out);
    bool set_network_info(const NetworkInfo& info);

    // ── Generic config get/set ─────────────────────────────────────────────────
    std::string get_info(const std::string& name);
    bool        set_info(const std::string& name, const std::string& data_json);

    // ── Device discovery ───────────────────────────────────────────────────────
    struct DiscoveredDevice {
        std::string ip;
        std::string mac;
        std::string hostname;
        std::string sn;
        int         tcp_port  = 34567;
        int         http_port = 80;
    };
    static std::vector<DiscoveredDevice> discover(int timeout_ms = 2000,
                                                   const std::string& bind_ip = "");

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
