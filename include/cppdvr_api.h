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

// ── Time ───────────────────────────────────────────────────────────────────────
// Returns non-zero on success.
CPPDVR_API int dvr_get_time(DVRHandle h,
                             int* year, int* month, int* day,
                             int* hour, int* minute, int* second);
CPPDVR_API int dvr_set_time(DVRHandle h,
                             int year, int month, int day,
                             int hour, int minute, int second);

// ── Reboot ─────────────────────────────────────────────────────────────────────
CPPDVR_API int dvr_reboot(DVRHandle h);

// ── OSD / Text overlay ─────────────────────────────────────────────────────────
// titles[0..count-1] are UTF-8 channel label strings.
CPPDVR_API int dvr_set_channel_titles(DVRHandle h,
                                       const char** titles, int count);
// bitmap_data: packed 1-bpp pixels, row-major, ceil(width/8)*height bytes.
CPPDVR_API int dvr_set_channel_bitmap(DVRHandle h,
                                       int width, int height,
                                       const uint8_t* bitmap_data,
                                       size_t bitmap_size);

// ── Network settings ───────────────────────────────────────────────────────────
typedef struct {
    char ip[64];
    char mask[64];
    char gateway[64];
    char dns[64];
    char hostname[128];
    char mac[32];
    int  tcp_port;
    int  http_port;
    int  dhcp;   // 0 = static, 1 = DHCP
} DVRNetworkInfoC;
CPPDVR_API int dvr_get_network_info(DVRHandle h, DVRNetworkInfoC* out);
CPPDVR_API int dvr_set_network_info(DVRHandle h, const DVRNetworkInfoC* info);

// ── Generic config get/set ─────────────────────────────────────────────────────
// dvr_get_info returns a heap-allocated JSON string; caller must dvr_free_string().
// Returns NULL on failure.
CPPDVR_API char* dvr_get_info(DVRHandle h, const char* name);
CPPDVR_API int   dvr_set_info(DVRHandle h, const char* name, const char* json);
CPPDVR_API void  dvr_free_string(char* s);

// ── Device discovery ───────────────────────────────────────────────────────────
// UDP broadcast on port 34569.  Does not require a connected DVRHandle.
// On success returns non-zero and sets *out_arr / *out_count.
// Caller must free with dvr_free_discovered().
typedef struct {
    char ip[64];
    char mac[32];
    char hostname[128];
    char sn[64];
    int  tcp_port;
    int  http_port;
} DVRDiscoveredDeviceC;
CPPDVR_API int  dvr_discover(int timeout_ms,
                               DVRDiscoveredDeviceC** out_arr,
                               int*                   out_count);
CPPDVR_API void dvr_free_discovered(DVRDiscoveredDeviceC* arr);

// ── Encoding settings  (Simplify.Encode) ───────────────────────────────────────
typedef struct {
    char compression[32];   // "H.264", "H.265", "MPEG4"
    char resolution[32];    // "5M","4M","3M","1080P","720P","D1","HD1","CIF"
    char bitrate_ctrl[8];   // "VBR" or "CBR"
    int  bitrate;           // kbps
    int  fps;
    int  gop;               // keyframe interval in seconds
    int  quality;           // 1–6
    int  video_enable;
    int  audio_enable;
} DVRVideoStreamFormatC;

typedef struct {
    DVRVideoStreamFormatC main;
    DVRVideoStreamFormatC extra;
} DVREncodeConfigC;

// channel=0 for single-channel cameras.
CPPDVR_API int dvr_get_encode_config(DVRHandle h, DVREncodeConfigC* out, int channel);
CPPDVR_API int dvr_set_encode_config(DVRHandle h, const DVREncodeConfigC* cfg, int channel);

// ── Camera / video-color parameters  (AVEnc.VideoColor) ────────────────────────
typedef struct {
    int brightness;    // 0–100
    int contrast;      // 0–100
    int saturation;    // 0–100
    int hue;           // 0–100
    int sharpness;     // Acutance — camera-specific range
    int gain;          // 0–100
    int whitebalance;  // 0–255
} DVRVideoColorC;

// channel=0, time_section=0 (always-on section).
CPPDVR_API int dvr_get_video_color(DVRHandle h, DVRVideoColorC* out, int channel);
CPPDVR_API int dvr_set_video_color(DVRHandle h, const DVRVideoColorC* params, int channel);

// Copy the last error string into out_buf (null-terminated, at most buf_len bytes).
CPPDVR_API void dvr_last_error(DVRHandle h, char* out_buf, int buf_len);

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

// ── Hardware acceleration — decode ────────────────────────────────────────────
// Decode accelerator for the H.264/H.265 → MJPEG streaming pipeline.
// The library probes available hwaccels once (ffmpeg -hwaccels) and caches results.
#define CPPDVR_DECODE_ACCEL_SOFTWARE 0   // always software
#define CPPDVR_DECODE_ACCEL_CUDA     1   // NVIDIA NVDEC
#define CPPDVR_DECODE_ACCEL_OTHER_HW 2   // d3d11va (Windows) / vaapi (Linux)
#define CPPDVR_DECODE_ACCEL_AUTO     3   // OtherHW → CUDA → software  (default)

// Set the decode accelerator.  Must be called before stream_start().
CPPDVR_API void stream_set_decode_accel(StreamHandle h, int accel);

// Get the currently configured decode accelerator (one of CPPDVR_DECODE_ACCEL_*).
CPPDVR_API int  stream_get_decode_accel(StreamHandle h);

// Raw hwaccel string override — passed verbatim as ffmpeg -hwaccel <value>.
// When non-empty, takes precedence over stream_set_decode_accel().
// Must be called before stream_start().  Pass "" to clear the override.
CPPDVR_API void stream_set_hwaccel(StreamHandle h, const char* hwaccel);
CPPDVR_API void stream_get_hwaccel(StreamHandle h, char* out_buf, int buf_len);

// Control whether stream_start() auto-selects the fastest available JPEG backend.
// Default: 1 (enabled).  Set to 0 if you call cppdvr_set_jpeg_backend() yourself.
CPPDVR_API void stream_set_auto_jpeg_backend(StreamHandle h, int enabled);

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

// ── JPEG decode/encode backend ────────────────────────────────────────────────
// All stream instances in this process share one active backend.
// Keep in sync with JPEG_BACKEND_* in src/jpeg_overlay.h.

#define CPPDVR_JPEG_BACKEND_STB           0   // stb_image (default, always available)
#define CPPDVR_JPEG_BACKEND_LIBJPEG_TURBO 1   // libjpeg-turbo (SIMD: SSE/AVX/NEON)
#define CPPDVR_JPEG_BACKEND_NVJPEG        2   // nvJPEG (NVIDIA GPU)

// Returns 1 if this backend was compiled in and hardware is present.
// Calling this for NVJPEG performs a lazy GPU init on the first call.
CPPDVR_API int cppdvr_jpeg_backend_available(int backend);

// Switch the active backend. Returns 1 on success, 0 if unavailable.
// On failure the previously active backend is unchanged.
CPPDVR_API int cppdvr_set_jpeg_backend(int backend);

// Return the currently active backend (one of CPPDVR_JPEG_BACKEND_*).
CPPDVR_API int cppdvr_get_jpeg_backend(void);

// Convenience wrappers on a StreamHandle (delegates to the process-level above).
CPPDVR_API int stream_set_jpeg_backend(StreamHandle h, int backend);
CPPDVR_API int stream_get_jpeg_backend(StreamHandle h);

// Decode a JPEG to packed RGB (3 bytes/pixel).
// Returns heap-allocated buffer; caller must free with cppdvr_jpeg_free().
// *out_width and *out_height are set on success. Returns NULL on failure.
// Uses the backend set by cppdvr_set_jpeg_backend().
CPPDVR_API uint8_t* cppdvr_jpeg_decode(const uint8_t* jpeg, size_t size,
                                        int* out_width, int* out_height);

// Encode packed RGB (3 bytes/pixel) to JPEG.
// Returns heap-allocated JPEG buffer; caller must free with cppdvr_jpeg_free().
// *out_size is set on success. Returns NULL on failure.
// quality: 1 (worst) to 100 (best).
CPPDVR_API uint8_t* cppdvr_jpeg_encode(const uint8_t* rgb,
                                        int width, int height,
                                        int quality, size_t* out_size);

// Free a buffer returned by cppdvr_jpeg_decode() or cppdvr_jpeg_encode().
CPPDVR_API void cppdvr_jpeg_free(uint8_t* buf);

// ── Overlay constants ─────────────────────────────────────────────────────────
#define STREAM_OVERLAY_MAX_TEXT 512

// Maximum number of simultaneous text boxes.
// Recompile with -DSTREAM_OVERLAY_MAX_BOXES=N to raise the limit.
#ifndef STREAM_OVERLAY_MAX_BOXES
#  define STREAM_OVERLAY_MAX_BOXES 4
#endif

// Alignment within a text box
#define STREAM_OVERLAY_ALIGN_LEFT  0
#define STREAM_OVERLAY_ALIGN_RIGHT 1

// Anchor corner — x,y in stream_overlay_box_configure are inward offsets from this corner
#define STREAM_OVERLAY_ANCHOR_TOP_LEFT     0
#define STREAM_OVERLAY_ANCHOR_TOP_RIGHT    1
#define STREAM_OVERLAY_ANCHOR_BOTTOM_LEFT  2
#define STREAM_OVERLAY_ANCHOR_BOTTOM_RIGHT 3

// ── Push-based single-region overlay ─────────────────────────────────────────
// Quick single-box overlay (no word-wrap, auto bottom-left position).
// For word-wrap, alignment, and multiple regions use stream_overlay_box_* below.
// '\n' starts a new line at the same x origin.

// Set pixel origin for the first character.
// Pass x=-1, y=-1 for the auto default: bottom-left, margin scales with height.
CPPDVR_API void stream_overlay_set_cursor(StreamHandle h, int x, int y);

// Set glyph scale factor (1 = native 8×8 px, 2 = 16×16, 4 = 32×32, etc.).
// Pass 0 for auto-scale (frame_height / 400, minimum 1).
CPPDVR_API void stream_overlay_set_scale(StreamHandle h, int scale);

// Replace overlay text.  Supports '\n' for multiple lines.
// Thread-safe — safe to call from any thread at any rate.
CPPDVR_API void stream_overlay_print(StreamHandle h, const char* text);

// Clear overlay (nothing drawn until next stream_overlay_print).
CPPDVR_API void stream_overlay_clear(StreamHandle h);

// ── Text box overlay (word-wrap, alignment, anchor) ───────────────────────────
// Up to STREAM_OVERLAY_MAX_BOXES independent regions, each with its own layout.
// All functions are thread-safe.
//
// Layout parameters for stream_overlay_box_configure():
//   idx    — box index 0 … STREAM_OVERLAY_MAX_BOXES-1
//   x, y   — inward pixel offset from the anchor corner
//   box_w  — text-box width in pixels:
//               • text wider than box_w word-wraps to the next line
//               • required for ALIGN_RIGHT (right edge = anchor_x + box_w)
//               • 0 = unconstrained (only '\n' causes line breaks)
//   scale  — glyph scale factor (1 = native 8×8 px; 0 = auto frame_height/400)
//   align  — STREAM_OVERLAY_ALIGN_LEFT or STREAM_OVERLAY_ALIGN_RIGHT
//   anchor — STREAM_OVERLAY_ANCHOR_* corner x,y are measured from

CPPDVR_API void stream_overlay_box_configure(StreamHandle h,
                                              int idx,
                                              int x, int y,
                                              int box_w,
                                              int scale,
                                              int align,
                                              int anchor);

// Set display text for box idx ('\n' and word-wrap both create new lines).
// Thread-safe — safe to call at any rate from any thread.
CPPDVR_API void stream_overlay_box_print(StreamHandle h, int idx, const char* text);

// Hide box idx (nothing drawn until next stream_overlay_box_print).
CPPDVR_API void stream_overlay_box_clear(StreamHandle h, int idx);

// Hide all boxes.
CPPDVR_API void stream_overlay_box_clear_all(StreamHandle h);

// ── Frame overlay callback ────────────────────────────────────────────────────
// For custom drawing directly into the raw RGB buffer every frame.
// Triggers a JPEG decode → RGB → re-encode cycle when set.
// rgb            — packed R,G,B pixels; width * height * 3 bytes total.
// bytes_per_pixel — always 3.
typedef void (*StreamOverlayFrameCallback)(uint8_t* rgb,
                                           uint64_t ts_us,
                                           int      width,
                                           int      height,
                                           int      bytes_per_pixel,
                                           void*    userdata);
CPPDVR_API void stream_set_overlay_frame_callback(StreamHandle h,
                                                   StreamOverlayFrameCallback cb,
                                                   void* userdata);

// ── Overlay JPEG callback ─────────────────────────────────────────────────────
// Fired with the POST-overlay JPEG for every frame.
// When no overlay callbacks are set the data is identical to StreamJpegCallback.
// Use this slot for recording the overlaid stream; the JPEG callback slot is
// then free for the GUI to use for display without conflict.
typedef void (*StreamOverlayJpegCallback)(const uint8_t* jpeg, size_t size,
                                           uint32_t frame_id, void* userdata);
CPPDVR_API void stream_set_overlay_jpeg_callback(StreamHandle h,
                                                  StreamOverlayJpegCallback cb,
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

// ── Encode acceleration ───────────────────────────────────────────────────────
// Controls which encoder is used when the recorder re-encodes video.
// Has no effect when RECORDER_USE_COPY=1 (stream-copy, the default for MP4).
// The library probes available encoders once (ffmpeg -encoders) and caches results.
#define CPPDVR_ENCODE_ACCEL_SOFTWARE 0   // libx264 / libx265
#define CPPDVR_ENCODE_ACCEL_CUDA     1   // h264_nvenc / hevc_nvenc  (NVIDIA)
#define CPPDVR_ENCODE_ACCEL_OTHER_HW 2   // h264_qsv / hevc_qsv  (Intel QuickSync)
#define CPPDVR_ENCODE_ACCEL_AUTO     3   // NVENC → QSV → AMF → software  (default)

// Set/get the encode accelerator.  Safe to change between recordings.
CPPDVR_API void recorder_set_encode_accel(RecorderHandle h, int accel);
CPPDVR_API int  recorder_get_encode_accel(RecorderHandle h);

// ── Hardware availability queries ─────────────────────────────────────────────
// Use these to populate UI combo boxes — grey out options that return 0.
// Results are cached after the first call (probe runs at most once per process).
//
// SOFTWARE and AUTO always return 1 (software fallback is always present).
// CUDA/OTHER_HW return 0 when the hardware or driver is absent.

// Returns 1 if the given CPPDVR_DECODE_ACCEL_* value is usable on this machine.
CPPDVR_API int cppdvr_decode_accel_available(int accel);

// Returns 1 if the given CPPDVR_ENCODE_ACCEL_* value is usable on this machine.
// For CUDA this checks h264_nvenc/hevc_nvenc via a smoke-test encode (~300 ms,
// cached).  For OTHER_HW it checks QSV then AMF.
CPPDVR_API int cppdvr_encode_accel_available(int accel);

// Fill out_buf with the backend name actually selected for this accel constant,
// e.g. "d3d11va", "cuda", "h264_nvenc", "h264_qsv", "software", "auto".
// Returns 1 on success, 0 if the accel is not available (buf set to "").
// out_buf of 32 bytes is always sufficient.
CPPDVR_API int cppdvr_decode_accel_name(int accel, char* out_buf, int buf_len);
CPPDVR_API int cppdvr_encode_accel_name(int accel, char* out_buf, int buf_len);

// Optional log/status callback — called from the writer thread.
CPPDVR_API void recorder_set_log_callback(RecorderHandle h,
                                           StreamLogCallback cb,
                                           void* userdata);

#ifdef __cplusplus
} // extern "C"
#endif
