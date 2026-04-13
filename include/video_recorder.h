#pragma once
/**
 * video_recorder.h — records the DVR stream to MP4 or MJPEG via ffmpeg.
 *
 * Two recording formats are supported:
 *
 *   RecordingFormat::MP4
 *     Input  : raw H264/H265 NAL stream (tapped via StreamServer raw-frame callback)
 *     Output : MP4 — no quality loss when RECORDER_USE_COPY=1, or re-encoded with CRF
 *     Wire-up: automatic through init(srv).  No extra user code needed.
 *
 *   RecordingFormat::MJPEG
 *     Input  : decoded JPEG frames (tapped from StreamServer JPEG callback)
 *     Output : AVI/MKV/MOV with MJPEG codec — one JPEG frame per video frame
 *     Wire-up: call feed_jpeg() from your StreamServer::set_jpeg_callback() lambda.
 *
 * Quality knobs — set via #define before including, or as compiler flags:
 *
 *   RECORDER_USE_COPY      1 = remux raw H265/H264 without re-encoding (MP4 only)
 *                          0 = re-encode with libx265/libx264 + CRF/PRESET below
 *   RECORDER_CRF           CRF value for re-encode  (0=lossless … 18=great … 51=bad)
 *   RECORDER_PRESET        ffmpeg preset string  (ultrafast/fast/medium/slow/…)
 *   RECORDER_BUFFER_FRAMES internal queue depth before oldest frame is dropped
 *                          (300 ≈ 12 s at 25 fps — absorbs slow disk I/O bursts)
 *
 * Typical usage — MP4 (stream-copy, no re-encode):
 *   #define RECORDER_USE_COPY 1
 *   cppdvr::VideoRecorder rec;
 *   rec.set_log_callback([](const char* m){ puts(m); });
 *   rec.init(&srv);                                          // before srv.start()
 *   // … srv.start() …
 *   rec.start_recording("clip.mp4");
 *   rec.pause_recording();
 *   rec.resume_recording();
 *   rec.save_recording();    // blocks until ffmpeg finalises
 *
 * Typical usage — MJPEG AVI:
 *   rec.init(&srv);
 *   srv.set_jpeg_callback([&](const uint8_t* d, size_t n){
 *       rec.feed_jpeg(d, n);          // forward decoded JPEG frames to recorder
 *       udp.sendJpeg(d, n);           // also forward to UDP as before
 *   });
 *   rec.start_recording("clip.avi", cppdvr::RecordingFormat::MJPEG, 25);
 *   rec.save_recording();
 */

#include "cppdvr_export.h"
#include "stream_server.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

// ── Quality / tuning constants ─────────────────────────────────────────────────
#ifndef RECORDER_USE_COPY
#  define RECORDER_USE_COPY       1        // 0 = re-encode; 1 = stream-copy (MP4 only)
#endif
#ifndef RECORDER_CRF
#  define RECORDER_CRF            18       // CRF for re-encode (lower = better quality)
#endif
#ifndef RECORDER_PRESET
#  define RECORDER_PRESET         "medium" // ffmpeg encoding preset
#endif
#ifndef RECORDER_BUFFER_FRAMES
#  define RECORDER_BUFFER_FRAMES  300      // max queued frames (overflow drops oldest)
#endif

namespace cppdvr {

// ── Recording output format ────────────────────────────────────────────────────
enum class RecordingFormat {
    MP4,    // H264/H265 → MP4  (stream-copy or re-encode; input: raw NAL from DVR)
    MJPEG,  // JPEG frames → MJPEG container  (input: call feed_jpeg() per frame)
};

// ── Recorder state ─────────────────────────────────────────────────────────────
enum class RecorderState {
    Idle,       // not recording
    Recording,  // actively capturing + writing frames
    Paused,     // session open; incoming frames are silently dropped
};

class CPPDVR_API VideoRecorder {
public:
    VideoRecorder();
    ~VideoRecorder();

    VideoRecorder(const VideoRecorder&)            = delete;
    VideoRecorder& operator=(const VideoRecorder&) = delete;

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    // Attach to srv and register the raw-frame callback used for MP4 recording.
    // Call before or after StreamServer::start() — safe either way.
    // For MJPEG recording, also wire feed_jpeg() into srv.set_jpeg_callback().
    // Returns false if srv is nullptr.
    bool init(StreamServer* srv);

    // Detach from srv and release all resources.
    // Implicitly calls discard_recording() if a recording is in progress.
    void deinit();

    // ── Recording control ─────────────────────────────────────────────────────

    // Begin recording to 'filename'.
    //   format    : MP4 or MJPEG (choose container by file extension too, e.g. .avi)
    //   framerate : frames-per-second hint written into the container header.
    //               Used only for MJPEG (MP4 derives timing from the H265 stream).
    // Returns false if already recording or not initialised.
    // For MP4  : ffmpeg starts on the first I-frame — there may be a brief delay.
    // For MJPEG: ffmpeg starts on the first call to feed_jpeg() after this.
    bool start_recording(const std::string& filename,
                         RecordingFormat format   = RecordingFormat::MP4,
                         int             framerate = 25);

    // Feed a decoded JPEG frame into the recorder.
    // Only has effect when recording in MJPEG format; no-op otherwise.
    // Call this from StreamServer::set_jpeg_callback() (or any thread).
    void feed_jpeg(const uint8_t* data, size_t size);

    // Finalise and write the output file to disk.
    // Closes ffmpeg stdin and waits up to 10 s for the muxer to flush.
    // No-op if idle.
    void save_recording();

    // Abort and delete the partial output file.
    // Kills ffmpeg immediately — the output file is incomplete and removed.
    // No-op if idle.
    void discard_recording();

    // Temporarily drop incoming frames.
    // The ffmpeg process and output file remain open.
    // Resume with resume_recording(); the video will contain a time-gap.
    // No-op if not recording.
    void pause_recording();

    // Resume forwarding frames after a pause.
    // No-op if not paused.
    void resume_recording();

    // ── Status ────────────────────────────────────────────────────────────────
    RecorderState   state()            const;
    bool            is_recording()     const;   // true when Recording or Paused
    bool            is_paused()        const;
    RecordingFormat format()           const;   // format of the current/last recording
    std::string     current_filename() const;
    size_t          frames_recorded()  const;   // frames successfully written to ffmpeg
    size_t          frames_dropped()   const;   // dropped (buffer overflow or paused)

    // ── Log callback ──────────────────────────────────────────────────────────
    // Called from the writer thread — keep it brief and thread-safe.
    using LogFn = std::function<void(const char* msg)>;
    void set_log_callback(LogFn fn);

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

} // namespace cppdvr
