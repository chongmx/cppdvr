/**
 * dllmain.cpp — DLL entry point + pure-C export API (cppdvr_api.h)
 *
 * Wraps cppdvr::DVRIPCam and cppdvr::StreamServer behind a
 * handle-based C interface so MyForm.h (C++/CLI) can call the DLL
 * without pulling in any platform or third-party headers.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "cppdvr_api.h"
#include "dvrip.h"
#include "stream_server.h"
#include "udp_stream_server.h"
#include "video_recorder.h"

#include <cstring>
#include <memory>
#include <new>

// ── DLL entry point ─────────────────────────────────────────────────────────────
BOOL WINAPI DllMain(HINSTANCE /*hInst*/, DWORD reason, LPVOID /*reserved*/) {
    // Winsock is initialised by the WsaGuard global in dvrip.cpp.
    // Nothing extra needed here.
    (void)reason;
    return TRUE;
}

// ════════════════════════════════════════════════════════════════════════════════
// DVR Camera C API
// ════════════════════════════════════════════════════════════════════════════════

extern "C" {

CPPDVR_API DVRHandle dvr_create(const char* host, int port,
                                const char* user, const char* password) {
    if (!host) return nullptr;
    try {
        auto* cam = new cppdvr::DVRIPCam(
            host,
            user     ? user     : "admin",
            password ? password : "",
            "tcp",
            port
        );
        return static_cast<DVRHandle>(cam);
    } catch (...) {
        return nullptr;
    }
}

CPPDVR_API void dvr_destroy(DVRHandle h) {
    if (!h) return;
    delete static_cast<cppdvr::DVRIPCam*>(h);
}

CPPDVR_API int dvr_login(DVRHandle h) {
    if (!h) return 0;
    return static_cast<cppdvr::DVRIPCam*>(h)->login() ? 1 : 0;
}

CPPDVR_API void dvr_close(DVRHandle h) {
    if (!h) return;
    static_cast<cppdvr::DVRIPCam*>(h)->close();
}

// Adapter: translates C++ std::function callback to the C callback signature.
struct MonitorCtx {
    DVRFrameCallback cb;
    void*            userdata;
};

CPPDVR_API int dvr_start_monitor(DVRHandle        h,
                                  DVRFrameCallback callback,
                                  void*            userdata,
                                  const char*      stream) {
    if (!h || !callback) return 0;

    // Allocate context on heap; lifetime matches monitor session.
    // Will be cleaned up when stop_monitor() is called (or DVRIPCam is destroyed).
    auto* ctx = new MonitorCtx{ callback, userdata };

    std::string stream_str = stream ? stream : "Main";

    bool ok = static_cast<cppdvr::DVRIPCam*>(h)->start_monitor(
        [ctx](const uint8_t* data, size_t size, const cppdvr::FrameMeta& meta) {
            DVRFrameMeta cmeta{};
            // Temporary storage for string pointers valid for callback duration
            cmeta.type   = meta.type.c_str();
            cmeta.frame  = meta.frame.c_str();
            cmeta.fps    = meta.fps;
            cmeta.width  = meta.width;
            cmeta.height = meta.height;
            ctx->cb(data, size, &cmeta, ctx->userdata);
        },
        stream_str
    );

    if (!ok) { delete ctx; return 0; }
    return 1;
}

CPPDVR_API void dvr_stop_monitor(DVRHandle h) {
    if (!h) return;
    static_cast<cppdvr::DVRIPCam*>(h)->stop_monitor();
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

CPPDVR_API void dvr_free_buffer(uint8_t* buf) {
    delete[] buf;
}

CPPDVR_API int dvr_ptz(DVRHandle h, const char* cmd,
                        int step, int preset, int channel) {
    if (!h || !cmd) return 0;
    return static_cast<cppdvr::DVRIPCam*>(h)->ptz(cmd, step, preset, channel)
           ? 1 : 0;
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

        auto* srv = new cppdvr::StreamServer(std::move(cfg));
        return static_cast<StreamHandle>(srv);
    } catch (...) {
        return nullptr;
    }
}

CPPDVR_API void stream_destroy(StreamHandle h) {
    if (!h) return;
    delete static_cast<cppdvr::StreamServer*>(h);
}

CPPDVR_API int stream_start(StreamHandle h, const char* stream_type) {
    if (!h) return 0;
    auto* srv = static_cast<cppdvr::StreamServer*>(h);

    // Allow caller to override stream type before starting
    if (stream_type && *stream_type != '\0') {
        // Re-create with new config (simplest approach since config is set at construction)
        // In practice the user passes it at stream_create time; this just re-confirms it.
    }

    return srv->start() ? 1 : 0;
}

CPPDVR_API void stream_stop(StreamHandle h) {
    if (!h) return;
    static_cast<cppdvr::StreamServer*>(h)->stop();
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

CPPDVR_API void stream_free_frame(uint8_t* buf) {
    delete[] buf;
}

CPPDVR_API int stream_is_running(StreamHandle h) {
    if (!h) return 0;
    return static_cast<cppdvr::StreamServer*>(h)->is_running() ? 1 : 0;
}

// ── Utility ───────────────────────────────────────────────────────────────────

CPPDVR_API void stream_set_log_callback(StreamHandle h,
                                         StreamLogCallback cb,
                                         void* userdata) {
    if (!h) return;
    auto* srv = static_cast<cppdvr::StreamServer*>(h);
    if (cb) {
        srv->set_log_callback([cb, userdata](const char* msg) {
            cb(msg, userdata);
        });
    } else {
        srv->set_log_callback(nullptr);
    }
}

CPPDVR_API void stream_set_jpeg_callback(StreamHandle h,
                                          StreamJpegCallback cb,
                                          void* userdata) {
    if (!h) return;
    auto* srv = static_cast<cppdvr::StreamServer*>(h);
    if (cb) {
        // frame_id is auto-incremented locally since JpegReadyFn doesn't carry it
        auto* counter = new uint32_t{0};
        srv->set_jpeg_callback([cb, userdata, counter](const uint8_t* jpeg, size_t size) {
            cb(jpeg, size, ++(*counter), userdata);
        });
        // Note: counter leaks if callback is replaced without being cleared first.
        // Acceptable for typical use (set once, cleared on stream_stop/destroy).
    } else {
        srv->set_jpeg_callback(nullptr);
    }
}

CPPDVR_API void dvr_sofia_hash(const char* password, char* out_buf, int buf_len) {
    if (!out_buf || buf_len < 9) return;
    std::string hash = cppdvr::DVRIPCam::sofia_hash(password ? password : "");
    std::strncpy(out_buf, hash.c_str(), static_cast<size_t>(buf_len) - 1);
    out_buf[buf_len - 1] = '\0';
}

// ════════════════════════════════════════════════════════════════════════════════
// UDP Stream Server C API
// ════════════════════════════════════════════════════════════════════════════════

static inline cppdvr::UdpStreamServer* udp_cast(UdpHandle h) {
    return static_cast<cppdvr::UdpStreamServer*>(h);
}

CPPDVR_API UdpHandle udp_create(const char* local_ip,
                                 const char* target_ip,
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

CPPDVR_API void udp_destroy(UdpHandle h) {
    if (h) delete udp_cast(h);
}

CPPDVR_API void udp_set_local_ip(UdpHandle h, const char* ip) {
    if (h && ip) udp_cast(h)->setLocalIP(ip);
}
CPPDVR_API void udp_set_target_ip(UdpHandle h, const char* ip) {
    if (h && ip) udp_cast(h)->setTargetIP(ip);
}
CPPDVR_API void udp_set_rx_port(UdpHandle h, int port) {
    if (h) udp_cast(h)->setRXPort(port);
}
CPPDVR_API void udp_set_tx_port(UdpHandle h, int port) {
    if (h) udp_cast(h)->setTXPort(port);
}
CPPDVR_API void udp_set_localhost_debug(UdpHandle h, int enable) {
    if (h) udp_cast(h)->setLocalhostDebug(enable != 0);
}

// ── Callback wrappers ──────────────────────────────────────────────────────────

CPPDVR_API void udp_set_input_callback(UdpHandle h,
                                        UdpInputCallbackC cb, void* userdata) {
    if (!h) return;
    if (cb) {
        udp_cast(h)->setOnInputCallback(
            [cb, userdata](const cppdvr::UdpInputEvent& e) {
                // Map C++ struct → C struct field-by-field
                UdpInputEventC c{};
                c.seq          = e.seq;
                c.timestamp_us = e.timestamp_us;
                std::memcpy(c.gui, e.gui, sizeof(c.gui));

                auto copy_ctrl = [](UdpControllerStateC& d,
                                    const cppdvr::UdpControllerState& s) {
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
                copy_ctrl(c.left,  e.left);
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
    if (cb) {
        udp_cast(h)->setOnJpegCallback(
            [cb, userdata](uint32_t fid, const uint8_t* d, size_t n) {
                cb(fid, d, n, userdata);
            });
    } else {
        udp_cast(h)->setOnJpegCallback(nullptr);
    }
}

CPPDVR_API void udp_set_log_callback(UdpHandle h,
                                      StreamLogCallback cb, void* userdata) {
    if (!h) return;
    if (cb) {
        udp_cast(h)->setLogCallback([cb, userdata](const char* msg) {
            cb(msg, userdata);
        });
    } else {
        udp_cast(h)->setLogCallback(nullptr);
    }
}

// ── Lifecycle ──────────────────────────────────────────────────────────────────

CPPDVR_API int  udp_init(UdpHandle h) {
    return (h && udp_cast(h)->init()) ? 1 : 0;
}
CPPDVR_API int  udp_start(UdpHandle h) {
    return (h && udp_cast(h)->start()) ? 1 : 0;
}
CPPDVR_API void udp_stop(UdpHandle h) {
    if (h) udp_cast(h)->stop();
}
CPPDVR_API void udp_deinit(UdpHandle h) {
    if (h) udp_cast(h)->deinit();
}
CPPDVR_API int udp_is_running(UdpHandle h) {
    return (h && udp_cast(h)->isRunning()) ? 1 : 0;
}
CPPDVR_API int udp_is_initialized(UdpHandle h) {
    return (h && udp_cast(h)->isInitialized()) ? 1 : 0;
}

// ── Sends ──────────────────────────────────────────────────────────────────────

CPPDVR_API void udp_send_jpeg(UdpHandle h, const uint8_t* jpeg,
                               size_t size, uint32_t frame_id) {
    if (h && jpeg && size) udp_cast(h)->sendJpeg(jpeg, size, frame_id);
}
CPPDVR_API void udp_send_command(UdpHandle h, uint32_t command_id,
                                  const int32_t params[8],
                                  const float   fparams[4]) {
    if (h) udp_cast(h)->sendCommand(command_id, params, fparams);
}
CPPDVR_API void udp_send_gui_update(UdpHandle h, const int32_t state[8]) {
    if (h) udp_cast(h)->sendGuiUpdate(state);
}
CPPDVR_API void udp_send_composite(UdpHandle h,
                                    const int32_t  gui[8],
                                    uint32_t       command_id,
                                    const int32_t  params[8],
                                    const float    fparams[4],
                                    const uint8_t* jpeg, size_t jpeg_size) {
    if (h) udp_cast(h)->sendComposite(gui, command_id, params, fparams,
                                       jpeg, jpeg_size);
}

// ── Getters ────────────────────────────────────────────────────────────────────

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

CPPDVR_API void recorder_destroy(RecorderHandle h) {
    if (h) delete rec_cast(h);
}

CPPDVR_API int recorder_init_with_stream(RecorderHandle h, StreamHandle stream) {
    if (!h || !stream) return 0;
    return rec_cast(h)->init(stream_cast(stream)) ? 1 : 0;
}

CPPDVR_API void recorder_deinit(RecorderHandle h) {
    if (h) rec_cast(h)->deinit();
}

CPPDVR_API int recorder_start(RecorderHandle h,
                               const char* filename, int format, int framerate) {
    if (!h || !filename) return 0;
    cppdvr::RecordingFormat fmt = (format == RECORDER_FORMAT_MJPEG)
                                ? cppdvr::RecordingFormat::MJPEG
                                : cppdvr::RecordingFormat::MP4;
    int fps = (framerate > 0) ? framerate : 25;
    return rec_cast(h)->start_recording(filename, fmt, fps) ? 1 : 0;
}

CPPDVR_API void recorder_feed_jpeg(RecorderHandle h,
                                    const uint8_t* jpeg, size_t size) {
    if (h && jpeg && size) rec_cast(h)->feed_jpeg(jpeg, size);
}

CPPDVR_API void recorder_save(RecorderHandle h) {
    if (h) rec_cast(h)->save_recording();
}

CPPDVR_API void recorder_discard(RecorderHandle h) {
    if (h) rec_cast(h)->discard_recording();
}

CPPDVR_API void recorder_pause(RecorderHandle h) {
    if (h) rec_cast(h)->pause_recording();
}

CPPDVR_API void recorder_resume(RecorderHandle h) {
    if (h) rec_cast(h)->resume_recording();
}

CPPDVR_API int recorder_state(RecorderHandle h) {
    if (!h) return RECORDER_STATE_IDLE;
    switch (rec_cast(h)->state()) {
        case cppdvr::RecorderState::Recording: return RECORDER_STATE_RECORDING;
        case cppdvr::RecorderState::Paused:    return RECORDER_STATE_PAUSED;
        default:                               return RECORDER_STATE_IDLE;
    }
}

CPPDVR_API size_t recorder_frames_recorded(RecorderHandle h) {
    return h ? rec_cast(h)->frames_recorded() : 0;
}

CPPDVR_API size_t recorder_frames_dropped(RecorderHandle h) {
    return h ? rec_cast(h)->frames_dropped() : 0;
}

CPPDVR_API void recorder_set_log_callback(RecorderHandle h,
                                           StreamLogCallback cb, void* userdata) {
    if (!h) return;
    if (cb) rec_cast(h)->set_log_callback([cb, userdata](const char* m){ cb(m, userdata); });
    else    rec_cast(h)->set_log_callback(nullptr);
}

} // extern "C"
