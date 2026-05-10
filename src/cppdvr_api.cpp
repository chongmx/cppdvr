/**
 * cppdvr_api.cpp — Pure-C export API (cppdvr_api.h)
 *
 * Wraps the C++ classes behind a handle-based C interface.
 * Compiled on all platforms (Windows and POSIX).
 */

#include "cppdvr_api.h"
#include "dvrip.h"
#include "stream_server.h"
#include "udp_stream_server.h"
#include "video_recorder.h"
#include "jpeg_overlay.h"   // jpeg_backend_*, jpeg_decode_rgb, jpeg_encode_rgb

#include <cstdlib>
#include <cstring>
#include <new>

// ── Internal C++ helpers (must live outside extern "C" to allow overloading) ───

static void copy_stream_fmt(DVRVideoStreamFormatC& dst,
                             const cppdvr::DVRIPCam::VideoStreamFormat& src) {
    auto cp = [](char* d, size_t n, const std::string& s) {
        std::strncpy(d, s.c_str(), n - 1); d[n-1] = '\0';
    };
    cp(dst.compression,  sizeof(dst.compression),  src.compression);
    cp(dst.resolution,   sizeof(dst.resolution),   src.resolution);
    cp(dst.bitrate_ctrl, sizeof(dst.bitrate_ctrl), src.bitrate_ctrl);
    dst.bitrate       = src.bitrate;
    dst.fps           = src.fps;
    dst.gop           = src.gop;
    dst.quality       = src.quality;
    dst.video_enable  = src.video_en ? 1 : 0;
    dst.audio_enable  = src.audio_en ? 1 : 0;
}

static void copy_stream_fmt(cppdvr::DVRIPCam::VideoStreamFormat& dst,
                             const DVRVideoStreamFormatC& src) {
    dst.compression  = src.compression;
    dst.resolution   = src.resolution;
    dst.bitrate_ctrl = src.bitrate_ctrl;
    dst.bitrate      = src.bitrate;
    dst.fps          = src.fps;
    dst.gop          = src.gop;
    dst.quality      = src.quality;
    dst.video_en     = (src.video_enable != 0);
    dst.audio_en     = (src.audio_enable != 0);
}

extern "C" {

// ════════════════════════════════════════════════════════════════════════════════
// DVR Camera C API
// ════════════════════════════════════════════════════════════════════════════════

CPPDVR_API DVRHandle dvr_create(const char* host, int port,
                                const char* user, const char* password) {
    if (!host) return nullptr;
    try {
        return static_cast<DVRHandle>(new cppdvr::DVRIPCam(
            host,
            user     ? user     : "admin",
            password ? password : "",
            "tcp",
            port
        ));
    } catch (...) { return nullptr; }
}

CPPDVR_API void dvr_destroy(DVRHandle h) {
    if (h) delete static_cast<cppdvr::DVRIPCam*>(h);
}

CPPDVR_API int dvr_login(DVRHandle h) {
    return (h && static_cast<cppdvr::DVRIPCam*>(h)->login()) ? 1 : 0;
}

CPPDVR_API void dvr_close(DVRHandle h) {
    if (h) static_cast<cppdvr::DVRIPCam*>(h)->close();
}

struct MonitorCtx { DVRFrameCallback cb; void* userdata; };

CPPDVR_API int dvr_start_monitor(DVRHandle h, DVRFrameCallback callback,
                                  void* userdata, const char* stream) {
    if (!h || !callback) return 0;
    auto* ctx = new MonitorCtx{ callback, userdata };
    std::string stream_str = stream ? stream : "Main";
    bool ok = static_cast<cppdvr::DVRIPCam*>(h)->start_monitor(
        [ctx](const uint8_t* data, size_t size, const cppdvr::FrameMeta& meta) {
            DVRFrameMeta cmeta{};
            cmeta.type   = meta.type.c_str();
            cmeta.frame  = meta.frame.c_str();
            cmeta.fps    = meta.fps;
            cmeta.width  = meta.width;
            cmeta.height = meta.height;
            ctx->cb(data, size, &cmeta, ctx->userdata);
        }, stream_str);
    if (!ok) { delete ctx; return 0; }
    return 1;
}

CPPDVR_API void dvr_stop_monitor(DVRHandle h) {
    if (h) static_cast<cppdvr::DVRIPCam*>(h)->stop_monitor();
}

CPPDVR_API uint8_t* dvr_snapshot(DVRHandle h, size_t* out_size, int channel) {
    if (!h || !out_size) { if (out_size) *out_size = 0; return nullptr; }
    auto jpeg = static_cast<cppdvr::DVRIPCam*>(h)->snapshot(channel);
    if (jpeg.empty()) { *out_size = 0; return nullptr; }
    uint8_t* buf = new (std::nothrow) uint8_t[jpeg.size()];
    if (!buf) { *out_size = 0; return nullptr; }
    std::memcpy(buf, jpeg.data(), jpeg.size());
    *out_size = jpeg.size();
    return buf;
}

CPPDVR_API void dvr_free_buffer(uint8_t* buf) { delete[] buf; }

CPPDVR_API int dvr_ptz(DVRHandle h, const char* cmd,
                        int step, int preset, int channel) {
    if (!h || !cmd) return 0;
    return static_cast<cppdvr::DVRIPCam*>(h)->ptz(cmd, step, preset, channel) ? 1 : 0;
}

CPPDVR_API void dvr_sofia_hash(const char* password, char* out_buf, int buf_len) {
    if (!out_buf || buf_len < 9) return;
    std::string hash = cppdvr::DVRIPCam::sofia_hash(password ? password : "");
    std::strncpy(out_buf, hash.c_str(), static_cast<size_t>(buf_len) - 1);
    out_buf[buf_len - 1] = '\0';
}

CPPDVR_API void dvr_last_error(DVRHandle h, char* out_buf, int buf_len) {
    if (!out_buf || buf_len < 1) return;
    if (!h) { out_buf[0] = '\0'; return; }
    std::string err = static_cast<cppdvr::DVRIPCam*>(h)->last_error();
    std::strncpy(out_buf, err.c_str(), static_cast<size_t>(buf_len) - 1);
    out_buf[buf_len - 1] = '\0';
}

// ── Time ───────────────────────────────────────────────────────────────────────

CPPDVR_API int dvr_get_time(DVRHandle h,
                              int* year, int* month, int* day,
                              int* hour, int* minute, int* second) {
    if (!h) return 0;
    cppdvr::DVRIPCam::DeviceTime dt;
    if (!static_cast<cppdvr::DVRIPCam*>(h)->get_time(dt)) return 0;
    if (year)   *year   = dt.year;
    if (month)  *month  = dt.month;
    if (day)    *day    = dt.day;
    if (hour)   *hour   = dt.hour;
    if (minute) *minute = dt.minute;
    if (second) *second = dt.second;
    return 1;
}

CPPDVR_API int dvr_set_time(DVRHandle h,
                              int year, int month, int day,
                              int hour, int minute, int second) {
    if (!h) return 0;
    cppdvr::DVRIPCam::DeviceTime dt{year, month, day, hour, minute, second};
    return static_cast<cppdvr::DVRIPCam*>(h)->set_time(dt) ? 1 : 0;
}

// ── Reboot ─────────────────────────────────────────────────────────────────────

CPPDVR_API int dvr_reboot(DVRHandle h) {
    return (h && static_cast<cppdvr::DVRIPCam*>(h)->reboot()) ? 1 : 0;
}

// ── OSD / Text overlay ─────────────────────────────────────────────────────────

CPPDVR_API int dvr_set_channel_titles(DVRHandle h,
                                       const char** titles, int count) {
    if (!h || !titles || count < 1) return 0;
    std::vector<std::string> vec;
    vec.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i)
        vec.emplace_back(titles[i] ? titles[i] : "");
    return static_cast<cppdvr::DVRIPCam*>(h)->set_channel_titles(vec) ? 1 : 0;
}

CPPDVR_API int dvr_set_channel_bitmap(DVRHandle h,
                                       int width, int height,
                                       const uint8_t* bitmap_data,
                                       size_t bitmap_size) {
    if (!h || !bitmap_data || bitmap_size == 0) return 0;
    std::vector<uint8_t> bmp(bitmap_data, bitmap_data + bitmap_size);
    return static_cast<cppdvr::DVRIPCam*>(h)->set_channel_bitmap(width, height, bmp) ? 1 : 0;
}

// ── Network settings ───────────────────────────────────────────────────────────

CPPDVR_API int dvr_get_network_info(DVRHandle h, DVRNetworkInfoC* out) {
    if (!h || !out) return 0;
    cppdvr::DVRIPCam::NetworkInfo info;
    if (!static_cast<cppdvr::DVRIPCam*>(h)->get_network_info(info)) return 0;
    auto copy_str = [](char* dst, size_t n, const std::string& src) {
        std::strncpy(dst, src.c_str(), n - 1);
        dst[n - 1] = '\0';
    };
    copy_str(out->ip,       sizeof(out->ip),       info.ip);
    copy_str(out->mask,     sizeof(out->mask),      info.mask);
    copy_str(out->gateway,  sizeof(out->gateway),   info.gateway);
    copy_str(out->dns,      sizeof(out->dns),       info.dns);
    copy_str(out->hostname, sizeof(out->hostname),  info.hostname);
    copy_str(out->mac,      sizeof(out->mac),       info.mac);
    out->tcp_port  = info.tcp_port;
    out->http_port = info.http_port;
    out->dhcp      = info.dhcp ? 1 : 0;
    return 1;
}

CPPDVR_API int dvr_set_network_info(DVRHandle h, const DVRNetworkInfoC* info) {
    if (!h || !info) return 0;
    cppdvr::DVRIPCam::NetworkInfo ni;
    ni.ip        = info->ip;
    ni.mask      = info->mask;
    ni.gateway   = info->gateway;
    ni.dns       = info->dns;
    ni.hostname  = info->hostname;
    ni.mac       = info->mac;
    ni.tcp_port  = info->tcp_port;
    ni.http_port = info->http_port;
    ni.dhcp      = (info->dhcp != 0);
    return static_cast<cppdvr::DVRIPCam*>(h)->set_network_info(ni) ? 1 : 0;
}

// ── Generic config get/set ─────────────────────────────────────────────────────

CPPDVR_API char* dvr_get_info(DVRHandle h, const char* name) {
    if (!h || !name) return nullptr;
    std::string result = static_cast<cppdvr::DVRIPCam*>(h)->get_info(name);
    if (result.empty()) return nullptr;
    char* s = new (std::nothrow) char[result.size() + 1];
    if (!s) return nullptr;
    std::memcpy(s, result.c_str(), result.size() + 1);
    return s;
}

CPPDVR_API int dvr_set_info(DVRHandle h, const char* name, const char* json) {
    if (!h || !name || !json) return 0;
    return static_cast<cppdvr::DVRIPCam*>(h)->set_info(name, json) ? 1 : 0;
}

CPPDVR_API void dvr_free_string(char* s) { delete[] s; }

// ── Device discovery ───────────────────────────────────────────────────────────

CPPDVR_API int dvr_discover(int timeout_ms,
                              DVRDiscoveredDeviceC** out_arr,
                              int*                   out_count) {
    if (!out_arr || !out_count) return 0;
    *out_arr   = nullptr;
    *out_count = 0;
    auto devices = cppdvr::DVRIPCam::discover(timeout_ms > 0 ? timeout_ms : 2000);
    if (devices.empty()) return 1;
    auto* arr = new (std::nothrow) DVRDiscoveredDeviceC[devices.size()];
    if (!arr) return 0;
    auto copy_str = [](char* dst, size_t n, const std::string& src) {
        std::strncpy(dst, src.c_str(), n - 1);
        dst[n - 1] = '\0';
    };
    for (size_t i = 0; i < devices.size(); ++i) {
        const auto& d = devices[i];
        std::memset(&arr[i], 0, sizeof(arr[i]));
        copy_str(arr[i].ip,       sizeof(arr[i].ip),       d.ip);
        copy_str(arr[i].mac,      sizeof(arr[i].mac),       d.mac);
        copy_str(arr[i].hostname, sizeof(arr[i].hostname),  d.hostname);
        copy_str(arr[i].sn,       sizeof(arr[i].sn),        d.sn);
        arr[i].tcp_port  = d.tcp_port;
        arr[i].http_port = d.http_port;
    }
    *out_arr   = arr;
    *out_count = static_cast<int>(devices.size());
    return 1;
}

CPPDVR_API void dvr_free_discovered(DVRDiscoveredDeviceC* arr) { delete[] arr; }

// ── Encoding settings ───────────────────────────────────────────────────────────

CPPDVR_API int dvr_get_encode_config(DVRHandle h, DVREncodeConfigC* out, int channel) {
    if (!h || !out) return 0;
    cppdvr::DVRIPCam::EncodeConfig cfg;
    if (!static_cast<cppdvr::DVRIPCam*>(h)->get_encode_config(cfg, channel)) return 0;
    copy_stream_fmt(out->main,  cfg.main);
    copy_stream_fmt(out->extra, cfg.extra);
    return 1;
}

CPPDVR_API int dvr_set_encode_config(DVRHandle h, const DVREncodeConfigC* c, int channel) {
    if (!h || !c) return 0;
    cppdvr::DVRIPCam::EncodeConfig cfg;
    copy_stream_fmt(cfg.main,  c->main);
    copy_stream_fmt(cfg.extra, c->extra);
    return static_cast<cppdvr::DVRIPCam*>(h)->set_encode_config(cfg, channel) ? 1 : 0;
}

// ── Video color / camera parameters ────────────────────────────────────────────

CPPDVR_API int dvr_get_video_color(DVRHandle h, DVRVideoColorC* out, int channel) {
    if (!h || !out) return 0;
    cppdvr::DVRIPCam::VideoColorParam p;
    if (!static_cast<cppdvr::DVRIPCam*>(h)->get_video_color(p, channel, 0)) return 0;
    out->brightness   = p.brightness;
    out->contrast     = p.contrast;
    out->saturation   = p.saturation;
    out->hue          = p.hue;
    out->sharpness    = p.sharpness;
    out->gain         = p.gain;
    out->whitebalance = p.whitebalance;
    return 1;
}

CPPDVR_API int dvr_set_video_color(DVRHandle h, const DVRVideoColorC* c, int channel) {
    if (!h || !c) return 0;
    cppdvr::DVRIPCam::VideoColorParam p;
    p.brightness   = c->brightness;
    p.contrast     = c->contrast;
    p.saturation   = c->saturation;
    p.hue          = c->hue;
    p.sharpness    = c->sharpness;
    p.gain         = c->gain;
    p.whitebalance = c->whitebalance;
    return static_cast<cppdvr::DVRIPCam*>(h)->set_video_color(p, channel, 0) ? 1 : 0;
}

// ════════════════════════════════════════════════════════════════════════════════
// Stream Server C API
// ════════════════════════════════════════════════════════════════════════════════

CPPDVR_API StreamHandle stream_create(const char* dvr_host, int dvr_port,
                                       const char* user, const char* password,
                                       int http_port) {
    if (!dvr_host) return nullptr;
    try {
        cppdvr::StreamServerConfig cfg;
        cfg.dvr_host     = dvr_host;
        cfg.dvr_port     = (dvr_port > 0) ? dvr_port : 34567;
        cfg.dvr_user     = user     ? user     : "admin";
        cfg.dvr_password = password ? password : "";
        cfg.http_port    = (http_port > 0) ? http_port : 8080;
        return static_cast<StreamHandle>(new cppdvr::StreamServer(std::move(cfg)));
    } catch (...) { return nullptr; }
}

CPPDVR_API void stream_destroy(StreamHandle h) {
    if (h) delete static_cast<cppdvr::StreamServer*>(h);
}

CPPDVR_API int stream_start(StreamHandle h, const char* stream_type) {
    if (!h) return 0;
    auto* srv = static_cast<cppdvr::StreamServer*>(h);
    if (stream_type && *stream_type) srv->set_stream_type(stream_type);
    return srv->start() ? 1 : 0;
}

CPPDVR_API void stream_set_hwaccel(StreamHandle h, const char* hwaccel) {
    if (!h) return;
    auto* srv = static_cast<cppdvr::StreamServer*>(h);
    // Access config via the const getter and cast — config is ours to modify
    // before start().  Use a non-const path: recreate via the stored config.
    // StreamServerConfig is not publicly mutable after construction, so we
    // expose it via a dedicated setter below on StreamServer.
    // For now, cast away const (cfg is a value member, not truly const).
    const_cast<cppdvr::StreamServerConfig&>(srv->config()).ffmpeg_hwaccel =
        hwaccel ? hwaccel : "";
}

CPPDVR_API void stream_get_hwaccel(StreamHandle h, char* out_buf, int buf_len) {
    if (!h || !out_buf || buf_len <= 0) return;
    const auto& hw = static_cast<cppdvr::StreamServer*>(h)->config().ffmpeg_hwaccel;
    std::strncpy(out_buf, hw.c_str(), static_cast<size_t>(buf_len) - 1);
    out_buf[buf_len - 1] = '\0';
}

CPPDVR_API void stream_set_auto_jpeg_backend(StreamHandle h, int enabled) {
    if (!h) return;
    const_cast<cppdvr::StreamServerConfig&>(
        static_cast<cppdvr::StreamServer*>(h)->config()).auto_jpeg_backend =
            (enabled != 0);
}

CPPDVR_API void stream_stop(StreamHandle h) {
    if (h) static_cast<cppdvr::StreamServer*>(h)->stop();
}

CPPDVR_API uint8_t* stream_get_frame(StreamHandle h, size_t* out_size) {
    if (!h || !out_size) { if (out_size) *out_size = 0; return nullptr; }
    auto frame = static_cast<cppdvr::StreamServer*>(h)->get_latest_frame();
    if (frame.empty()) { *out_size = 0; return nullptr; }
    uint8_t* buf = new (std::nothrow) uint8_t[frame.size()];
    if (!buf) { *out_size = 0; return nullptr; }
    std::memcpy(buf, frame.data(), frame.size());
    *out_size = frame.size();
    return buf;
}

CPPDVR_API void stream_free_frame(uint8_t* buf) { delete[] buf; }

CPPDVR_API int stream_is_running(StreamHandle h) {
    return (h && static_cast<cppdvr::StreamServer*>(h)->is_running()) ? 1 : 0;
}

CPPDVR_API void stream_set_log_callback(StreamHandle h,
                                         StreamLogCallback cb, void* userdata) {
    if (!h) return;
    auto* srv = static_cast<cppdvr::StreamServer*>(h);
    if (cb) srv->set_log_callback([cb, userdata](const char* msg){ cb(msg, userdata); });
    else    srv->set_log_callback(nullptr);
}

CPPDVR_API void stream_set_jpeg_callback(StreamHandle h,
                                          StreamJpegCallback cb, void* userdata) {
    if (!h) return;
    auto* srv = static_cast<cppdvr::StreamServer*>(h);
    if (cb) {
        auto* counter = new uint32_t{0};
        srv->set_jpeg_callback([cb, userdata, counter](const uint8_t* jpeg, size_t size) {
            cb(jpeg, size, ++(*counter), userdata);
        });
    } else {
        srv->set_jpeg_callback(nullptr);
    }
}

// ── JPEG backend ──────────────────────────────────────────────────────────────

CPPDVR_API int cppdvr_jpeg_backend_available(int backend) {
    return jpeg_backend_available(backend);
}
CPPDVR_API int cppdvr_set_jpeg_backend(int backend) {
    return jpeg_backend_set(backend);
}
CPPDVR_API int cppdvr_get_jpeg_backend(void) {
    return jpeg_backend_get();
}

CPPDVR_API int stream_set_jpeg_backend(StreamHandle h, int backend) {
    if (!h) return 0;
    auto* srv = static_cast<cppdvr::StreamServer*>(h);
    return srv->set_jpeg_backend(
        static_cast<cppdvr::StreamServer::JpegBackend>(backend)) ? 1 : 0;
}
CPPDVR_API int stream_get_jpeg_backend(StreamHandle h) {
    if (!h) return CPPDVR_JPEG_BACKEND_STB;
    return static_cast<int>(
        static_cast<cppdvr::StreamServer*>(h)->get_jpeg_backend());
}

CPPDVR_API uint8_t* cppdvr_jpeg_decode(const uint8_t* jpeg, size_t size,
                                        int* out_width, int* out_height) {
    return jpeg_decode_rgb(jpeg, size, out_width, out_height);
}
CPPDVR_API uint8_t* cppdvr_jpeg_encode(const uint8_t* rgb,
                                        int width, int height,
                                        int quality, size_t* out_size) {
    if (!out_size) return nullptr;
    uint8_t* buf = nullptr;
    jpeg_encode_rgb(rgb, width, height, quality, &buf, out_size);
    return buf;
}
CPPDVR_API void cppdvr_jpeg_free(uint8_t* buf) {
    free(buf);
}

CPPDVR_API void stream_overlay_set_cursor(StreamHandle h, int x, int y) {
    if (h) static_cast<cppdvr::StreamServer*>(h)->overlay_set_cursor(x, y);
}
CPPDVR_API void stream_overlay_set_scale(StreamHandle h, int scale) {
    if (h) static_cast<cppdvr::StreamServer*>(h)->overlay_set_scale(scale);
}
CPPDVR_API void stream_overlay_print(StreamHandle h, const char* text) {
    if (h) static_cast<cppdvr::StreamServer*>(h)->overlay_print(text);
}
CPPDVR_API void stream_overlay_clear(StreamHandle h) {
    if (h) static_cast<cppdvr::StreamServer*>(h)->overlay_clear();
}

CPPDVR_API void stream_overlay_box_configure(StreamHandle h,
                                              int idx,
                                              int x, int y,
                                              int box_w,
                                              int scale,
                                              int align,
                                              int anchor) {
    if (!h) return;
    static_cast<cppdvr::StreamServer*>(h)->overlay_box_configure(
        idx, x, y, box_w, scale,
        static_cast<cppdvr::StreamServer::OverlayAlign>(align),
        static_cast<cppdvr::StreamServer::OverlayAnchor>(anchor));
}
CPPDVR_API void stream_overlay_box_print(StreamHandle h, int idx, const char* text) {
    if (h) static_cast<cppdvr::StreamServer*>(h)->overlay_box_print(idx, text);
}
CPPDVR_API void stream_overlay_box_clear(StreamHandle h, int idx) {
    if (h) static_cast<cppdvr::StreamServer*>(h)->overlay_box_clear(idx);
}
CPPDVR_API void stream_overlay_box_clear_all(StreamHandle h) {
    if (h) static_cast<cppdvr::StreamServer*>(h)->overlay_box_clear_all();
}

CPPDVR_API void stream_set_overlay_frame_callback(StreamHandle h,
                                                   StreamOverlayFrameCallback cb,
                                                   void* userdata) {
    if (!h) return;
    auto* srv = static_cast<cppdvr::StreamServer*>(h);
    if (cb)
        srv->set_overlay_frame_callback(
            [cb, userdata](uint8_t* rgb, uint64_t ts, int w, int ht) {
                cb(rgb, ts, w, ht, 3, userdata);
            });
    else
        srv->set_overlay_frame_callback(nullptr);
}

CPPDVR_API void stream_set_overlay_jpeg_callback(StreamHandle h,
                                                  StreamOverlayJpegCallback cb,
                                                  void* userdata) {
    if (!h) return;
    auto* srv = static_cast<cppdvr::StreamServer*>(h);
    if (cb) {
        auto* counter = new uint32_t{0};
        srv->set_overlay_jpeg_callback(
            [cb, userdata, counter](const uint8_t* jpeg, size_t size) {
                cb(jpeg, size, ++(*counter), userdata);
            });
    } else {
        srv->set_overlay_jpeg_callback(nullptr);
    }
}

// ════════════════════════════════════════════════════════════════════════════════
// UDP Stream Server C API
// ════════════════════════════════════════════════════════════════════════════════

static inline cppdvr::UdpStreamServer* udp_cast(UdpHandle h) {
    return static_cast<cppdvr::UdpStreamServer*>(h);
}

CPPDVR_API UdpHandle udp_create(const char* local_ip, const char* target_ip,
                                 int rx_port, int tx_port) {
    try {
        auto* srv = new cppdvr::UdpStreamServer();
        if (local_ip  && *local_ip)  srv->setLocalIP(local_ip);
        if (target_ip && *target_ip) srv->setTargetIP(target_ip);
        if (rx_port > 0) srv->setRXPort(rx_port);
        if (tx_port > 0) srv->setTXPort(tx_port);
        return static_cast<UdpHandle>(srv);
    } catch (...) { return nullptr; }
}

CPPDVR_API void udp_destroy(UdpHandle h)                        { if (h) delete udp_cast(h); }
CPPDVR_API void udp_set_local_ip(UdpHandle h, const char* ip)  { if (h && ip) udp_cast(h)->setLocalIP(ip); }
CPPDVR_API void udp_set_target_ip(UdpHandle h, const char* ip) { if (h && ip) udp_cast(h)->setTargetIP(ip); }
CPPDVR_API void udp_set_rx_port(UdpHandle h, int port)         { if (h) udp_cast(h)->setRXPort(port); }
CPPDVR_API void udp_set_tx_port(UdpHandle h, int port)         { if (h) udp_cast(h)->setTXPort(port); }
CPPDVR_API void udp_set_localhost_debug(UdpHandle h, int en)   { if (h) udp_cast(h)->setLocalhostDebug(en != 0); }
CPPDVR_API int  udp_init(UdpHandle h)     { return (h && udp_cast(h)->init())      ? 1 : 0; }
CPPDVR_API int  udp_start(UdpHandle h)    { return (h && udp_cast(h)->start())     ? 1 : 0; }
CPPDVR_API void udp_stop(UdpHandle h)     { if (h) udp_cast(h)->stop(); }
CPPDVR_API void udp_deinit(UdpHandle h)   { if (h) udp_cast(h)->deinit(); }
CPPDVR_API int  udp_is_running(UdpHandle h)      { return (h && udp_cast(h)->isRunning())      ? 1 : 0; }
CPPDVR_API int  udp_is_initialized(UdpHandle h)  { return (h && udp_cast(h)->isInitialized())  ? 1 : 0; }

CPPDVR_API void udp_set_input_callback(UdpHandle h,
                                        UdpInputCallbackC cb, void* userdata) {
    if (!h) return;
    if (cb) {
        udp_cast(h)->setOnInputCallback([cb, userdata](const cppdvr::UdpInputEvent& e) {
            UdpInputEventC c{};
            c.seq          = e.seq;
            c.timestamp_us = e.timestamp_us;
            std::memcpy(c.gui, e.gui, sizeof(c.gui));
            auto copy_ctrl = [](UdpControllerStateC& d, const cppdvr::UdpControllerState& s) {
                d.primary_button   = s.primary_button;
                d.secondary_button = s.secondary_button;
                d.menu_button      = s.menu_button;
                d.thumbstick_click = s.thumbstick_click;
                d.trigger_value    = s.trigger_value;
                d.trigger_click    = s.trigger_click;
                d.grip_value       = s.grip_value;
                d.grip_click       = s.grip_click;
                d.thumbstick_x     = s.thumbstick_x;
                d.thumbstick_y     = s.thumbstick_y;
                d.primary_touch    = s.primary_touch;
                d.secondary_touch  = s.secondary_touch;
                d.thumbstick_touch = s.thumbstick_touch;
                d.trigger_touch    = s.trigger_touch;
                d.thumbrest_touch  = s.thumbrest_touch;
                d.active           = s.active;
                std::memcpy(d.aim_pos,  s.aim_pos,  sizeof(d.aim_pos));
                std::memcpy(d.aim_ori,  s.aim_ori,  sizeof(d.aim_ori));
                std::memcpy(d.grip_pos, s.grip_pos, sizeof(d.grip_pos));
                std::memcpy(d.grip_ori, s.grip_ori, sizeof(d.grip_ori));
            };
            copy_ctrl(c.left, e.left);
            copy_ctrl(c.right, e.right);
            cb(&c, userdata);
        });
    } else {
        udp_cast(h)->setOnInputCallback(nullptr);
    }
}

CPPDVR_API void udp_set_jpeg_callback(UdpHandle h,
                                       UdpJpegCallbackC cb, void* userdata) {
    if (!h) return;
    if (cb) udp_cast(h)->setOnJpegCallback([cb, userdata](uint32_t fid, const uint8_t* d, size_t n){ cb(fid, d, n, userdata); });
    else    udp_cast(h)->setOnJpegCallback(nullptr);
}

CPPDVR_API void udp_set_log_callback(UdpHandle h,
                                      StreamLogCallback cb, void* userdata) {
    if (!h) return;
    if (cb) udp_cast(h)->setLogCallback([cb, userdata](const char* msg){ cb(msg, userdata); });
    else    udp_cast(h)->setLogCallback(nullptr);
}

CPPDVR_API void udp_send_jpeg(UdpHandle h, const uint8_t* jpeg,
                               size_t size, uint32_t frame_id) {
    if (h && jpeg && size) udp_cast(h)->sendJpeg(jpeg, size, frame_id);
}
CPPDVR_API void udp_send_command(UdpHandle h, uint32_t command_id,
                                  const int32_t params[8], const float fparams[4]) {
    if (h) udp_cast(h)->sendCommand(command_id, params, fparams);
}
CPPDVR_API void udp_send_gui_update(UdpHandle h, const int32_t state[8]) {
    if (h) udp_cast(h)->sendGuiUpdate(state);
}
CPPDVR_API void udp_send_composite(UdpHandle h, const int32_t gui[8],
                                    uint32_t command_id, const int32_t params[8],
                                    const float fparams[4],
                                    const uint8_t* jpeg, size_t jpeg_size) {
    if (h) udp_cast(h)->sendComposite(gui, command_id, params, fparams, jpeg, jpeg_size);
}

CPPDVR_API void udp_get_target_ip(UdpHandle h, char* buf, int buf_len) {
    if (!h || !buf || buf_len < 1) return;
    std::string ip = udp_cast(h)->targetIP();
    std::strncpy(buf, ip.c_str(), static_cast<size_t>(buf_len) - 1);
    buf[buf_len - 1] = '\0';
}
CPPDVR_API void udp_get_local_ip(UdpHandle h, char* buf, int buf_len) {
    if (!h || !buf || buf_len < 1) return;
    std::string ip = udp_cast(h)->localIP();
    std::strncpy(buf, ip.c_str(), static_cast<size_t>(buf_len) - 1);
    buf[buf_len - 1] = '\0';
}

// ════════════════════════════════════════════════════════════════════════════════
// Video Recorder C API
// ════════════════════════════════════════════════════════════════════════════════

static inline cppdvr::VideoRecorder* rec_cast(RecorderHandle h) {
    return static_cast<cppdvr::VideoRecorder*>(h);
}
static inline cppdvr::StreamServer* stream_cast(StreamHandle h) {
    return static_cast<cppdvr::StreamServer*>(h);
}

CPPDVR_API RecorderHandle recorder_create(void) {
    try { return static_cast<RecorderHandle>(new cppdvr::VideoRecorder()); }
    catch (...) { return nullptr; }
}
CPPDVR_API void recorder_destroy(RecorderHandle h)  { if (h) delete rec_cast(h); }

CPPDVR_API int recorder_init_with_stream(RecorderHandle h, StreamHandle stream) {
    if (!h || !stream) return 0;
    return rec_cast(h)->init(stream_cast(stream)) ? 1 : 0;
}
CPPDVR_API int recorder_init_standalone(RecorderHandle h) {
    return (h && rec_cast(h)->init_standalone()) ? 1 : 0;
}
CPPDVR_API void recorder_deinit(RecorderHandle h) { if (h) rec_cast(h)->deinit(); }

CPPDVR_API int recorder_start(RecorderHandle h,
                               const char* filename, int format, int framerate) {
    if (!h || !filename) return 0;
    cppdvr::RecordingFormat fmt = (format == RECORDER_FORMAT_MJPEG)
                                ? cppdvr::RecordingFormat::MJPEG
                                : cppdvr::RecordingFormat::MP4;
    return rec_cast(h)->start_recording(filename, fmt, framerate > 0 ? framerate : 25) ? 1 : 0;
}

CPPDVR_API void recorder_feed_jpeg(RecorderHandle h, const uint8_t* jpeg, size_t size) {
    if (h && jpeg && size) rec_cast(h)->feed_jpeg(jpeg, size);
}
CPPDVR_API void recorder_feed_raw(RecorderHandle h, const uint8_t* data, size_t size,
                                   const char* codec, int is_iframe) {
    if (h && data && size) rec_cast(h)->feed_raw_frame(data, size, codec ? codec : "", is_iframe != 0);
}
CPPDVR_API void recorder_save(RecorderHandle h)    { if (h) rec_cast(h)->save_recording(); }
CPPDVR_API void recorder_discard(RecorderHandle h) { if (h) rec_cast(h)->discard_recording(); }
CPPDVR_API void recorder_pause(RecorderHandle h)   { if (h) rec_cast(h)->pause_recording(); }
CPPDVR_API void recorder_resume(RecorderHandle h)  { if (h) rec_cast(h)->resume_recording(); }

CPPDVR_API int recorder_state(RecorderHandle h) {
    if (!h) return RECORDER_STATE_IDLE;
    switch (rec_cast(h)->state()) {
        case cppdvr::RecorderState::Recording: return RECORDER_STATE_RECORDING;
        case cppdvr::RecorderState::Paused:    return RECORDER_STATE_PAUSED;
        default:                               return RECORDER_STATE_IDLE;
    }
}
CPPDVR_API size_t recorder_frames_recorded(RecorderHandle h) { return h ? rec_cast(h)->frames_recorded() : 0; }
CPPDVR_API size_t recorder_frames_dropped(RecorderHandle h)  { return h ? rec_cast(h)->frames_dropped()  : 0; }

CPPDVR_API void recorder_set_log_callback(RecorderHandle h,
                                           StreamLogCallback cb, void* userdata) {
    if (!h) return;
    if (cb) rec_cast(h)->set_log_callback([cb, userdata](const char* m){ cb(m, userdata); });
    else    rec_cast(h)->set_log_callback(nullptr);
}

} // extern "C"
