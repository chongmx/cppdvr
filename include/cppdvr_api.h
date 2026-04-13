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

// ── JPEG frame callback ────────────────────────────────────────────────────────
// Fired from the ffmpeg pipeline thread for every decoded JPEG frame.
// jpeg / size are valid only for the duration of the call — copy if needed.
// frame_id increments from 1.  Use this to forward frames to UDP / recorder
// without the 40 ms polling overhead of stream_get_frame().
// Register before stream_start(); safe to replace at any time.
typedef void (*StreamJpegCallback)(const uint8_t* jpeg, size_t size,
                                    uint32_t frame_id, void* userdata);
CPPDVR_API void stream_set_jpeg_callback(StreamHandle h,
                                          StreamJpegCallback cb,
                                          void* userdata);

// ════════════════════════════════════════════════════════════════════════════════
// UDP Stream Server API  (XrRobotOperator XRRO v1 protocol)
// ════════════════════════════════════════════════════════════════════════════════

typedef void* UdpHandle;

// Controller state for one Quest controller (received from headset).
typedef struct {
    uint8_t  primary_button, secondary_button, menu_button, thumbstick_click;
    float    trigger_value;
    uint8_t  trigger_click;
    float    grip_value;
    uint8_t  grip_click;
    float    thumbstick_x, thumbstick_y;
    uint8_t  primary_touch, secondary_touch, thumbstick_touch,
             trigger_touch, thumbrest_touch;
    uint8_t  active;
    float    aim_pos[3];    // metres (x,y,z)
    float    aim_ori[4];    // quaternion (x,y,z,w)
    float    grip_pos[3];
    float    grip_ori[4];
} UdpControllerStateC;

// Input event delivered to UdpInputCallbackC.
typedef struct {
    uint32_t             seq;
    uint64_t             timestamp_us;
    UdpControllerStateC  left;
    UdpControllerStateC  right;
    int32_t              gui[8];
} UdpInputEventC;

// Callbacks (called from the receive thread).
typedef void (*UdpInputCallbackC)(const UdpInputEventC* evt, void* userdata);
typedef void (*UdpJpegCallbackC)(uint32_t frame_id,
                                  const uint8_t* data, size_t size,
                                  void* userdata);

// Create a UDP stream server handle.
// local_ip  — NIC to bind (NULL or "" → "0.0.0.0")
// target_ip — headset IP  (NULL or "" → auto-discover from first packet)
// rx_port   — port we listen on  (0 → default 9000)
// tx_port   — port headset listens on  (0 → default 9001)
CPPDVR_API UdpHandle udp_create(const char* local_ip,
                                 const char* target_ip,
                                 int         rx_port,
                                 int         tx_port);

// Free all resources; calls udp_stop() / udp_deinit() automatically.
CPPDVR_API void udp_destroy(UdpHandle h);

// ── Configuration (must be called before udp_init) ────────────────────────────
CPPDVR_API void udp_set_local_ip(UdpHandle h, const char* ip);
CPPDVR_API void udp_set_target_ip(UdpHandle h, const char* ip);
CPPDVR_API void udp_set_rx_port(UdpHandle h, int port);
CPPDVR_API void udp_set_tx_port(UdpHandle h, int port);
// Mirror every outgoing datagram to 127.0.0.1:rx_port for local Python debug.
CPPDVR_API void udp_set_localhost_debug(UdpHandle h, int enable);

// ── Callbacks ─────────────────────────────────────────────────────────────────
CPPDVR_API void udp_set_input_callback(UdpHandle h,
                                        UdpInputCallbackC cb, void* userdata);
CPPDVR_API void udp_set_jpeg_callback(UdpHandle h,
                                       UdpJpegCallbackC cb, void* userdata);
CPPDVR_API void udp_set_log_callback(UdpHandle h,
                                      StreamLogCallback cb, void* userdata);

// ── Lifecycle ─────────────────────────────────────────────────────────────────
// Creates and binds the UDP socket.  Returns non-zero on success.
CPPDVR_API int  udp_init(UdpHandle h);

// Starts the background receive thread.  Returns non-zero on success.
CPPDVR_API int  udp_start(UdpHandle h);

// Stops the receive thread (socket stays open; sends still work).
CPPDVR_API void udp_stop(UdpHandle h);

// Closes the socket.  Calls udp_stop() first if needed.
CPPDVR_API void udp_deinit(UdpHandle h);

CPPDVR_API int  udp_is_running(UdpHandle h);
CPPDVR_API int  udp_is_initialized(UdpHandle h);

// ── Sends (PC → headset) ─────────────────────────────────────────────────────
// Send a JPEG frame as one or more JpegChunk (0x02) datagrams.
// frame_id = 0 → auto-increment.
CPPDVR_API void udp_send_jpeg(UdpHandle h,
                               const uint8_t* jpeg, size_t size,
                               uint32_t frame_id);

// Send a Command (0x03) datagram.
// params[8] and fparams[4] may be NULL (treated as all-zero).
CPPDVR_API void udp_send_command(UdpHandle h,
                                  uint32_t      command_id,
                                  const int32_t params[8],
                                  const float   fparams[4]);

// Send a GuiUpdate (0x04) datagram.  state[8] may be NULL (all-zero).
CPPDVR_API void udp_send_gui_update(UdpHandle h, const int32_t state[8]);

// Send a Composite (0x05) datagram.  Any pointer may be NULL (treated as zero).
CPPDVR_API void udp_send_composite(UdpHandle h,
                                    const int32_t  gui[8],
                                    uint32_t       command_id,
                                    const int32_t  params[8],
                                    const float    fparams[4],
                                    const uint8_t* jpeg,
                                    size_t         jpeg_size);

// ── Getters ───────────────────────────────────────────────────────────────────
// Returns the resolved target IP (configured or auto-discovered).
// Buffer must be at least INET_ADDRSTRLEN (16) bytes.
CPPDVR_API void udp_get_target_ip(UdpHandle h, char* buf, int buf_len);
CPPDVR_API void udp_get_local_ip(UdpHandle h,  char* buf, int buf_len);

// ── Utility ────────────────────────────────────────────────────────────────────

// Compute the Sofia password hash (XiongMai login).
// out_buf must be at least 9 bytes (8 chars + NUL).
CPPDVR_API void dvr_sofia_hash(const char* password, char* out_buf, int buf_len);

// ════════════════════════════════════════════════════════════════════════════════
// Video Recorder API  (records stream to MP4 or MJPEG AVI via ffmpeg)
// ════════════════════════════════════════════════════════════════════════════════

typedef void* RecorderHandle;

// Recording output format.
#define RECORDER_FORMAT_MP4   0   // H264/H265 → MP4  (raw stream, no quality loss)
#define RECORDER_FORMAT_MJPEG 1   // JPEG frames → MJPEG AVI

// Recorder state values returned by recorder_state().
#define RECORDER_STATE_IDLE      0
#define RECORDER_STATE_RECORDING 1
#define RECORDER_STATE_PAUSED    2

// Create a recorder handle.
CPPDVR_API RecorderHandle recorder_create(void);

// Free all resources; calls recorder_discard() if a recording is in progress.
CPPDVR_API void recorder_destroy(RecorderHandle h);

// Attach to a StreamServer — registers the raw-frame hook for MP4 recording.
// Must be called before recorder_start().
// For MJPEG recording, also call recorder_feed_jpeg() from stream_set_jpeg_callback().
// Returns non-zero on success.
CPPDVR_API int recorder_init_with_stream(RecorderHandle h, StreamHandle stream);

// Initialise without a StreamServer (standalone / test mode).
// Raw NAL frames must be injected manually via recorder_feed_raw().
// Returns non-zero on success (always succeeds).
CPPDVR_API int recorder_init_standalone(RecorderHandle h);

// Detach from the stream server and release internal resources.
// Implicitly calls recorder_discard() if recording.
CPPDVR_API void recorder_deinit(RecorderHandle h);

// Begin recording to 'filename'.
//   format    : RECORDER_FORMAT_MP4 or RECORDER_FORMAT_MJPEG
//   framerate : FPS written into the container (used only for MJPEG; 0 → 25)
// Returns non-zero on success.
CPPDVR_API int  recorder_start(RecorderHandle h,
                                const char* filename,
                                int format,
                                int framerate);

// Feed a decoded JPEG frame (MJPEG format only).
// Call this from stream_set_jpeg_callback() when recording in MJPEG format.
// No-op otherwise.
CPPDVR_API void recorder_feed_jpeg(RecorderHandle h,
                                    const uint8_t* jpeg, size_t size);

// Feed a raw NAL frame (MP4 format, standalone mode).
// Use after recorder_init_standalone() to inject H264/H265 Annex-B bytes.
//   codec     : "h264" or "h265" (NULL or "" → auto-detect from NAL bytes)
//   is_iframe : non-zero if this is an I-frame (hint; NAL scan is authoritative)
CPPDVR_API void recorder_feed_raw(RecorderHandle h,
                                   const uint8_t* data, size_t size,
                                   const char* codec, int is_iframe);

// Finalise and save the output file.
// Closes ffmpeg stdin and waits up to 10 s for the muxer to flush.
// No-op if idle.
CPPDVR_API void recorder_save(RecorderHandle h);

// Abort and delete the partial output file.  No-op if idle.
CPPDVR_API void recorder_discard(RecorderHandle h);

// Temporarily drop frames (ffmpeg process stays alive).  No-op if not recording.
CPPDVR_API void recorder_pause(RecorderHandle h);

// Resume after a pause.  No-op if not paused.
CPPDVR_API void recorder_resume(RecorderHandle h);

// Return the current recorder state: RECORDER_STATE_*.
CPPDVR_API int recorder_state(RecorderHandle h);

// Return the number of frames successfully written to ffmpeg since last start.
CPPDVR_API size_t recorder_frames_recorded(RecorderHandle h);

// Return the number of frames dropped (buffer overflow or paused) since last start.
CPPDVR_API size_t recorder_frames_dropped(RecorderHandle h);

// Optional log/status callback — called from the writer thread.
CPPDVR_API void recorder_set_log_callback(RecorderHandle h,
                                           StreamLogCallback cb,
                                           void* userdata);

#ifdef __cplusplus
} // extern "C"
#endif
