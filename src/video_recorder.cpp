/**
 * video_recorder.cpp — records the DVR stream to MP4 or MJPEG via a child ffmpeg process.
 *
 * Pipeline overview:
 *   MP4   : StreamServer raw-frame callback → frame queue → writer thread → ffmpeg stdin → .mp4
 *   MJPEG : User calls feed_jpeg()          → frame queue → writer thread → ffmpeg stdin → .avi
 *
 * Thread model:
 *   Producer   — StreamServer camera thread (MP4) or user JPEG callback thread (MJPEG)
 *   Consumer   — writer_thread (single consumer, drives ffmpeg)
 *   Signalling — condition_variable + q_stop / q_abort flags
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>   // must precede windows.h on MSVC
#include <windows.h>

#include "video_recorder.h"

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cppdvr {

// ════════════════════════════════════════════════════════════════════════════════
// NAL codec detection (mirrors stream_server.cpp — kept local to avoid coupling)
// ════════════════════════════════════════════════════════════════════════════════
static std::string rec_detect_codec(const uint8_t* data, size_t len) {
    if (len < 5) return "";
    uint8_t nal_byte = 0;
    if      (data[0]==0 && data[1]==0 && data[2]==0 && data[3]==1) nal_byte = data[4];
    else if (data[0]==0 && data[1]==0 && data[2]==1)                nal_byte = data[3];
    else return "";

    int h265_type = (nal_byte >> 1) & 0x3F;
    if (h265_type >= 16 && h265_type <= 40) return "h265";

    int h264_type = nal_byte & 0x1F;
    if (h264_type == 5 || h264_type == 6 || h264_type == 7 || h264_type == 8)
        return "h264";

    return "";
}

// ════════════════════════════════════════════════════════════════════════════════
// ffmpeg child-process wrapper
// ════════════════════════════════════════════════════════════════════════════════
struct FfmpegRecProc {
    HANDLE      hStdinWrite  {INVALID_HANDLE_VALUE};
    HANDLE      hStderrRead  {INVALID_HANDLE_VALUE};
    HANDLE      hProcess     {INVALID_HANDLE_VALUE};
    HANDLE      hProcThread  {INVALID_HANDLE_VALUE};
    std::thread stderr_thr;

    using LogFn = std::function<void(const char*)>;

    // Build and launch the ffmpeg command for the chosen format.
    // log_fn receives stderr lines from ffmpeg (errors, warnings).
    bool start(RecordingFormat fmt, const std::string& codec,
               const std::string& filename, int framerate,
               LogFn log_fn = nullptr) {

        std::string cmd;

        if (fmt == RecordingFormat::MP4) {
            const std::string input_fmt = (codec == "h265") ? "hevc" : "h264";

            std::string vcodec_args;
#if RECORDER_USE_COPY
            vcodec_args = "-c:v copy";
#else
            const std::string enc = (codec == "h265") ? "libx265" : "libx264";
            vcodec_args = std::string("-c:v ") + enc
                        + " -crf "    + std::to_string(RECORDER_CRF)
                        + " -preset " + RECORDER_PRESET;
#endif
            cmd = "ffmpeg -loglevel warning"
                  " -f "  + input_fmt + " -i pipe:0 "
                  + vcodec_args
                  + " -y \"" + filename + "\"";

        } else {
            cmd = "ffmpeg -loglevel warning"
                  " -f image2pipe -vcodec mjpeg"
                  " -framerate " + std::to_string(framerate) +
                  " -i pipe:0 -c:v copy"
                  " -y \"" + filename + "\"";
        }

        // Resolve the absolute output path and log it so the user knows where
        // the file will appear (CWD-relative names can be confusing).
        if (log_fn) {
            wchar_t abs[MAX_PATH] = {};
            std::wstring wfn(filename.begin(), filename.end());
            if (GetFullPathNameW(wfn.c_str(), MAX_PATH, abs, nullptr)) {
                std::wstring wa(abs);
                std::string sa(wa.begin(), wa.end());
                log_fn(("recorder: output -> " + sa).c_str());
            }
            log_fn(("recorder: cmd: " + cmd).c_str());
        }

        SECURITY_ATTRIBUTES sa{};
        sa.nLength        = sizeof(sa);
        sa.bInheritHandle = TRUE;

        // Stdin pipe — we write raw data to hStdinWrite
        HANDLE hStdinR{INVALID_HANDLE_VALUE};
        if (!CreatePipe(&hStdinR, &hStdinWrite, &sa, 0)) return false;
        SetHandleInformation(hStdinWrite, HANDLE_FLAG_INHERIT, 0);

        // Stderr pipe — we read ffmpeg error/warning messages
        HANDLE hStderrWrite{INVALID_HANDLE_VALUE};
        if (!CreatePipe(&hStderrRead, &hStderrWrite, &sa, 4096)) {
            close_all(); CloseHandle(hStdinR); return false;
        }
        SetHandleInformation(hStderrRead, HANDLE_FLAG_INHERIT, 0);

        // Stdout → NUL (ffmpeg writes the file; stdout not needed)
        HANDLE hNul = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE,
                                   &sa, OPEN_EXISTING, 0, nullptr);

        STARTUPINFOW si{};
        si.cb         = sizeof(si);
        si.hStdInput  = hStdinR;
        si.hStdOutput = (hNul != INVALID_HANDLE_VALUE) ? hNul : INVALID_HANDLE_VALUE;
        si.hStdError  = hStderrWrite;
        si.dwFlags    = STARTF_USESTDHANDLES;

        PROCESS_INFORMATION pi{};
        std::wstring wcmd(cmd.begin(), cmd.end());

        const BOOL ok = CreateProcessW(
            nullptr, wcmd.data(), nullptr, nullptr,
            TRUE, CREATE_NO_WINDOW,
            nullptr, nullptr, &si, &pi
        );

        CloseHandle(hStdinR);
        CloseHandle(hStderrWrite);   // child has its own copy; close ours
        if (hNul != INVALID_HANDLE_VALUE) CloseHandle(hNul);

        if (!ok) { close_all(); return false; }

        hProcess    = pi.hProcess;
        hProcThread = pi.hThread;

        // Drain ffmpeg stderr on a background thread so we see errors in the log
        if (log_fn) {
            stderr_thr = std::thread([this, log_fn]() {
                char buf[512];
                DWORD n = 0;
                std::string line;
                while (ReadFile(hStderrRead, buf, sizeof(buf) - 1, &n, nullptr) && n > 0) {
                    buf[n] = '\0';
                    line += buf;
                    // Forward complete lines
                    size_t pos;
                    while ((pos = line.find('\n')) != std::string::npos) {
                        std::string l = line.substr(0, pos);
                        while (!l.empty() && (l.back() == '\r' || l.back() == '\n'))
                            l.pop_back();
                        if (!l.empty()) log_fn(("ffmpeg: " + l).c_str());
                        line.erase(0, pos + 1);
                    }
                }
                if (!line.empty()) log_fn(("ffmpeg: " + line).c_str());
            });
        }

        return true;
    }

    bool write_data(const uint8_t* data, size_t len) {
        if (hStdinWrite == INVALID_HANDLE_VALUE || len == 0) return false;
        DWORD written = 0;
        return WriteFile(hStdinWrite, data, static_cast<DWORD>(len),
                         &written, nullptr) != FALSE
               && written == static_cast<DWORD>(len);
    }

    // Close stdin gracefully — signals ffmpeg to finalise the container.
    void close_stdin() {
        if (hStdinWrite != INVALID_HANDLE_VALUE) {
            CloseHandle(hStdinWrite);
            hStdinWrite = INVALID_HANDLE_VALUE;
        }
    }

    // Wait for ffmpeg to exit. Returns true if it exited within timeout_ms.
    // Also waits for the stderr reader thread to finish.
    bool wait_for_exit(DWORD timeout_ms) {
        if (hProcess == INVALID_HANDLE_VALUE) return true;
        bool ok = (WaitForSingleObject(hProcess, timeout_ms) == WAIT_OBJECT_0);
        // Stderr pipe closes when process exits; join reader thread
        if (hStderrRead != INVALID_HANDLE_VALUE) {
            CloseHandle(hStderrRead);
            hStderrRead = INVALID_HANDLE_VALUE;
        }
        if (stderr_thr.joinable()) stderr_thr.join();
        return ok;
    }

    // Hard-kill the ffmpeg process and close all handles.
    void terminate() {
        close_stdin();
        if (hProcess != INVALID_HANDLE_VALUE) {
            TerminateProcess(hProcess, 0);
            WaitForSingleObject(hProcess, 2000);
        }
        if (hStderrRead != INVALID_HANDLE_VALUE) {
            CloseHandle(hStderrRead);
            hStderrRead = INVALID_HANDLE_VALUE;
        }
        if (stderr_thr.joinable()) stderr_thr.join();
        close_all();
    }

    void close_all() {
        auto cl = [](HANDLE& h) {
            if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); h = INVALID_HANDLE_VALUE; }
        };
        cl(hStdinWrite);
        cl(hStderrRead);
        cl(hProcess);
        cl(hProcThread);
    }
};

// ════════════════════════════════════════════════════════════════════════════════
// VideoRecorder::Impl
// ════════════════════════════════════════════════════════════════════════════════
struct VideoRecorder::Impl {

    // ── State (guarded by mtx) ────────────────────────────────────────────────
    mutable std::mutex      mtx;
    std::condition_variable cv;

    RecorderState           rec_state {RecorderState::Idle};
    RecordingFormat         rec_fmt   {RecordingFormat::MP4};
    std::string             out_fname;
    int                     rec_fps   {25};

    // Stop / abort signals (set under mtx, checked by writer thread)
    bool q_stop  {false};   // save path: drain queue then finalise
    bool q_abort {false};   // discard path: kill ffmpeg immediately

    // ── Frame queue ───────────────────────────────────────────────────────────
    struct QFrame {
        std::vector<uint8_t> data;
        std::string          codec_hint; // "h264"/"h265" (MP4 mode, from DVRIP header)
        bool                 is_iframe {false};
    };
    std::deque<QFrame> queue;

    // ── Counters (atomic — readable without holding mtx) ─────────────────────
    std::atomic<size_t> count_written {0};
    std::atomic<size_t> count_dropped {0};

    // ── Writer thread ─────────────────────────────────────────────────────────
    std::thread writer_thr;

    // ── Log callback ──────────────────────────────────────────────────────────
    mutable std::mutex   log_mtx;
    VideoRecorder::LogFn log_fn;

    bool initialized {false};

    // ─────────────────────────────────────────────────────────────────────────

    void log(const char* fmt, ...) {
        std::function<void(const char*)> fn;
        { std::lock_guard<std::mutex> lk(log_mtx); fn = log_fn; }
        if (!fn) return;
        char buf[512];
        va_list ap; va_start(ap, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        fn(buf);
    }

    // ── Internal: push one frame (MP4 raw NAL) ────────────────────────────────
    void feed_raw(const uint8_t* data, size_t size,
                  const std::string& codec_hint, bool is_iframe) {
        std::lock_guard<std::mutex> lk(mtx);
        if (rec_state == RecorderState::Idle) return;
        if (rec_fmt   != RecordingFormat::MP4) return;
        push_frame(data, size, codec_hint, is_iframe);
    }

    // ── Internal: push one JPEG frame (MJPEG) ────────────────────────────────
    void feed_jpeg_internal(const uint8_t* data, size_t size) {
        std::lock_guard<std::mutex> lk(mtx);
        if (rec_state == RecorderState::Idle) return;
        if (rec_fmt   != RecordingFormat::MJPEG) return;
        push_frame(data, size, "", true /*every JPEG is a "keyframe"*/);
    }

    // ── Internal: enqueue a frame (caller holds mtx) ─────────────────────────
    void push_frame(const uint8_t* data, size_t size,
                    const std::string& codec_hint, bool is_iframe) {
        if (rec_state == RecorderState::Paused) { ++count_dropped; return; }
        if (q_stop || q_abort)                  return;

        if (queue.size() >= RECORDER_BUFFER_FRAMES) {
            queue.pop_front();   // drop oldest on overflow
            ++count_dropped;
        }
        QFrame f;
        f.data.assign(data, data + size);
        f.codec_hint = codec_hint;
        f.is_iframe  = is_iframe;
        queue.push_back(std::move(f));
        cv.notify_one();
    }

    // ════════════════════════════════════════════════════════════════════════
    // Writer thread — runs until save/discard
    // ════════════════════════════════════════════════════════════════════════
    void run_writer() {
        FfmpegRecProc ffp;
        bool        ffp_started = false;
        bool        aborted     = false;
        std::string fname;
        RecordingFormat fmt;
        int fps;
        {
            std::lock_guard<std::mutex> lk(mtx);
            fname = out_fname;
            fmt   = rec_fmt;
            fps   = rec_fps;
        }

        while (true) {
            QFrame frame;

            {
                std::unique_lock<std::mutex> lk(mtx);
                cv.wait(lk, [this]{ return !queue.empty() || q_stop || q_abort; });

                if (q_abort) { aborted = true; break; }
                if (queue.empty()) { /* q_stop + drained */ break; }

                frame = std::move(queue.front());
                queue.pop_front();
            }

            // ── MP4: wait for the first I-frame before starting ffmpeg ────────
            if (fmt == RecordingFormat::MP4 && !ffp_started) {
                if (!frame.is_iframe) { ++count_dropped; continue; }

                std::string codec = rec_detect_codec(frame.data.data(), frame.data.size());
                if (codec.empty()) codec = frame.codec_hint;
                if (codec.empty()) codec = "h265";   // safe default

                log("recorder: starting ffmpeg  format=mp4  codec=%s  crf=%d  copy=%d",
                    codec.c_str(), RECORDER_CRF, RECORDER_USE_COPY);
                log("recorder: -> %s", fname.c_str());

                if (!ffp.start(RecordingFormat::MP4, codec, fname, fps, [this](const char* m){ log(m); })) {
                    log("recorder: ffmpeg failed to start (is ffmpeg in PATH?)");
                    aborted = true; break;
                }
                ffp_started = true;
            }

            // ── MJPEG: start ffmpeg on the very first JPEG frame ──────────────
            if (fmt == RecordingFormat::MJPEG && !ffp_started) {
                log("recorder: starting ffmpeg  format=mjpeg  fps=%d", fps);
                log("recorder: -> %s", fname.c_str());

                if (!ffp.start(RecordingFormat::MJPEG, "", fname, fps, [this](const char* m){ log(m); })) {
                    log("recorder: ffmpeg failed to start (is ffmpeg in PATH?)");
                    aborted = true; break;
                }
                ffp_started = true;
            }

            // ── Write frame to ffmpeg stdin ────────────────────────────────────
            if (!ffp.write_data(frame.data.data(), frame.data.size())) {
                log("recorder: ffmpeg stdin write error — stopping");
                aborted = true; break;
            }
            ++count_written;
        }

        // ── Finalise or abort ─────────────────────────────────────────────────
        if (ffp_started) {
            if (aborted) {
                log("recorder: aborting — killing ffmpeg");
                ffp.terminate();
            } else {
                ffp.close_stdin();
                log("recorder: finalizing  (%zu frames written)...", count_written.load());
                if (!ffp.wait_for_exit(10000)) {
                    log("recorder: ffmpeg finalize timed out — killing");
                    ffp.terminate();
                } else {
                    log("recorder: saved -> %s", fname.c_str());
                }
                ffp.close_all();
            }
        } else {
            log("recorder: no frames written (stream not yet active when stopped)");
        }

        {
            std::lock_guard<std::mutex> lk(mtx);
            rec_state = RecorderState::Idle;
        }
    }

    // ── Internal stop helper — sets stop or abort flag, joins writer thread ───
    // Must be called WITHOUT holding mtx.
    void do_stop(bool abort_flag) {
        {
            std::lock_guard<std::mutex> lk(mtx);
            if (rec_state == RecorderState::Idle) return;
            if (abort_flag) q_abort = true;
            else            q_stop  = true;
            cv.notify_all();
        }
        if (writer_thr.joinable()) writer_thr.join();
        {
            std::lock_guard<std::mutex> lk(mtx);
            queue.clear();
            q_stop  = false;
            q_abort = false;
            // rec_state already set Idle by run_writer
        }
        count_written = 0;
        count_dropped = 0;
    }
};

// ════════════════════════════════════════════════════════════════════════════════
// VideoRecorder public API
// ════════════════════════════════════════════════════════════════════════════════

VideoRecorder::VideoRecorder() : d_(std::make_unique<Impl>()) {}
VideoRecorder::~VideoRecorder() { deinit(); }

// ── Lifecycle ──────────────────────────────────────────────────────────────────
bool VideoRecorder::init(StreamServer* srv) {
    if (!srv) return false;

    // Register raw-frame hook for MP4 recording.
    // For MJPEG, the user wires feed_jpeg() into their jpeg callback instead.
    srv->set_raw_frame_callback(
        [this](const uint8_t* data, size_t size,
               const std::string& codec, bool is_iframe) {
            d_->feed_raw(data, size, codec, is_iframe);
        }
    );

    d_->initialized = true;
    return true;
}

void VideoRecorder::deinit() {
    if (!d_->initialized) return;
    discard_recording();
    d_->initialized = false;
}

// ── Recording control ──────────────────────────────────────────────────────────
bool VideoRecorder::start_recording(const std::string& filename,
                                    RecordingFormat format, int framerate) {
    std::lock_guard<std::mutex> lk(d_->mtx);
    if (!d_->initialized)                       return false;
    if (d_->rec_state != RecorderState::Idle)   return false;

    d_->out_fname     = filename;
    d_->rec_fmt       = format;
    d_->rec_fps       = framerate;
    d_->count_written = 0;
    d_->count_dropped = 0;
    d_->q_stop        = false;
    d_->q_abort       = false;
    d_->queue.clear();
    d_->rec_state     = RecorderState::Recording;

    d_->writer_thr = std::thread([this]{ d_->run_writer(); });

    d_->log("recorder: start_recording  format=%s  fps=%d",
            format == RecordingFormat::MP4 ? "mp4" : "mjpeg", framerate);
    d_->log("recorder: -> %s", filename.c_str());
    return true;
}

void VideoRecorder::feed_jpeg(const uint8_t* data, size_t size) {
    d_->feed_jpeg_internal(data, size);
}

void VideoRecorder::save_recording() {
    d_->do_stop(false);
}

void VideoRecorder::discard_recording() {
    std::string fname;
    {
        std::lock_guard<std::mutex> lk(d_->mtx);
        if (d_->rec_state == RecorderState::Idle) return;
        fname = d_->out_fname;
    }
    d_->do_stop(true);
    if (!fname.empty()) {
        std::remove(fname.c_str());
        d_->log("recorder: discarded %s", fname.c_str());
    }
}

void VideoRecorder::pause_recording() {
    std::lock_guard<std::mutex> lk(d_->mtx);
    if (d_->rec_state == RecorderState::Recording) {
        d_->rec_state = RecorderState::Paused;
        d_->log("recorder: paused");
    }
}

void VideoRecorder::resume_recording() {
    std::lock_guard<std::mutex> lk(d_->mtx);
    if (d_->rec_state == RecorderState::Paused) {
        d_->rec_state = RecorderState::Recording;
        d_->log("recorder: resumed");
    }
}

// ── Status ─────────────────────────────────────────────────────────────────────
RecorderState VideoRecorder::state() const {
    std::lock_guard<std::mutex> lk(d_->mtx);
    return d_->rec_state;
}
bool VideoRecorder::is_recording() const {
    std::lock_guard<std::mutex> lk(d_->mtx);
    return d_->rec_state != RecorderState::Idle;
}
bool VideoRecorder::is_paused() const {
    std::lock_guard<std::mutex> lk(d_->mtx);
    return d_->rec_state == RecorderState::Paused;
}
RecordingFormat VideoRecorder::format() const {
    std::lock_guard<std::mutex> lk(d_->mtx);
    return d_->rec_fmt;
}
std::string VideoRecorder::current_filename() const {
    std::lock_guard<std::mutex> lk(d_->mtx);
    return d_->out_fname;
}
size_t VideoRecorder::frames_recorded() const { return d_->count_written.load(); }
size_t VideoRecorder::frames_dropped()  const { return d_->count_dropped.load(); }

void VideoRecorder::set_log_callback(LogFn fn) {
    std::lock_guard<std::mutex> lk(d_->log_mtx);
    d_->log_fn = std::move(fn);
}

} // namespace cppdvr
