#pragma once
/**
 * cppdvr_api.h — Pure C export API for cppdvr.dll
 *
 * Use these functions from C++/CLI (MyForm.h) or any other language
 * that can call a Windows DLL via P/Invoke / LoadLibrary.
 *
 * All strings are UTF-8 null-terminated.
 * All handles are opaque pointers; must be destroyed with the matching
 * *_destroy() call to avoid resource leaks.
 */

#include "cppdvr_export.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── Opaque handles ─────────────────────────────────────────────────────────────
typedef void* DVRHandle;
typedef void* StreamHandle;

// ── Frame metadata passed to DVRFrameCallback ─────────────────────────────────
typedef struct {
    const char* type;   // "h264", "h265", "mpeg4", "jpeg", "g711a", "info"
    const char* frame;  // "I" or "P"
    int         fps;
    int         width;
    int         height;
} DVRFrameMeta;

// Frame callback: called on the monitor background thread for every frame.
// data / size — raw NAL bytes (Annex-B for H264/H265)
// meta        — frame metadata (valid only for the duration of the call)
// userdata    — value passed to dvr_start_monitor()
typedef void (*DVRFrameCallback)(const uint8_t*    data,
                                 size_t            size,
                                 const DVRFrameMeta* meta,
                                 void*             userdata);

// ════════════════════════════════════════════════════════════════════════════════
// DVR Camera API
// ════════════════════════════════════════════════════════════════════════════════

// Create a DVR camera handle.  port=0 uses the default DVRIP port (34567).
CPPDVR_API DVRHandle dvr_create(const char* host,
                                int         port,
                                const char* user,
                                const char* password);

// Free all resources; safe to call with NULL.
CPPDVR_API void dvr_destroy(DVRHandle h);

// Connect + authenticate + start keep-alive timer.
// Returns non-zero on success.
CPPDVR_API int dvr_login(DVRHandle h);

// Close the socket connection (keep-alive is stopped).
CPPDVR_API void dvr_close(DVRHandle h);

// Start live monitor on a background thread.
// stream = "Main" or "Extra"
// Returns non-zero on success.
CPPDVR_API int dvr_start_monitor(DVRHandle        h,
                                 DVRFrameCallback callback,
                                 void*            userdata,
                                 const char*      stream);

// Stop the monitor loop; waits for the background thread to exit.
CPPDVR_API void dvr_stop_monitor(DVRHandle h);

// Capture a JPEG snapshot.
// Returns heap-allocated buffer; caller must free with dvr_free_buffer().
// *out_size is set to the byte count, or 0 on failure.
CPPDVR_API uint8_t* dvr_snapshot(DVRHandle h, size_t* out_size, int channel);

// Free a buffer returned by dvr_snapshot().
CPPDVR_API void dvr_free_buffer(uint8_t* buf);

// Send a PTZ command.
// cmd: "DirectionUp","DirectionDown","DirectionLeft","DirectionRight",
//      "ZoomTile","ZoomWide","SetPreset","GotoPreset","StartTour","StopTour"
// Returns non-zero on success.
CPPDVR_API int dvr_ptz(DVRHandle   h,
                       const char* cmd,
                       int         step,
                       int         preset,
                       int         channel);

// ════════════════════════════════════════════════════════════════════════════════
// Stream Server API  (DVR → ffmpeg → HTTP MJPEG)
// ════════════════════════════════════════════════════════════════════════════════

// Create a stream server.  http_port=0 defaults to 8080.
CPPDVR_API StreamHandle stream_create(const char* dvr_host,
                                      int         dvr_port,
                                      const char* user,
                                      const char* password,
                                      int         http_port);

// Free all resources; stops server if running.
CPPDVR_API void stream_destroy(StreamHandle h);

// Start camera, ffmpeg, and HTTP server threads.
// stream_type = "Main" or "Extra"
// Returns non-zero on success.
CPPDVR_API int stream_start(StreamHandle h, const char* stream_type);

// Stop all threads gracefully.
CPPDVR_API void stream_stop(StreamHandle h);

// Get a copy of the latest decoded JPEG frame.
// Returns heap-allocated buffer; caller must free with stream_free_frame().
// *out_size = 0 and returns NULL if no frame is available.
CPPDVR_API uint8_t* stream_get_frame(StreamHandle h, size_t* out_size);

// Free a buffer returned by stream_get_frame().
CPPDVR_API void stream_free_frame(uint8_t* buf);

// Return non-zero if the server is running.
CPPDVR_API int stream_is_running(StreamHandle h);

// ── Log / status callback ──────────────────────────────────────────────────────
// Optional callback invoked from background threads with status/error messages.
// msg is valid only for the duration of the call.
// Register before calling stream_start(); safe to call from any thread.
typedef void (*StreamLogCallback)(const char* msg, void* userdata);
CPPDVR_API void stream_set_log_callback(StreamHandle h,
                                         StreamLogCallback cb,
                                         void* userdata);

// ── Utility ────────────────────────────────────────────────────────────────────

// Compute the Sofia password hash (XiongMai login).
// out_buf must be at least 9 bytes (8 chars + NUL).
CPPDVR_API void dvr_sofia_hash(const char* password, char* out_buf, int buf_len);

#ifdef __cplusplus
} // extern "C"
#endif
