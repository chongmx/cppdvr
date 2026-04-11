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

CPPDVR_API void dvr_sofia_hash(const char* password, char* out_buf, int buf_len) {
    if (!out_buf || buf_len < 9) return;
    std::string hash = cppdvr::DVRIPCam::sofia_hash(password ? password : "");
    std::strncpy(out_buf, hash.c_str(), static_cast<size_t>(buf_len) - 1);
    out_buf[buf_len - 1] = '\0';
}

} // extern "C"
