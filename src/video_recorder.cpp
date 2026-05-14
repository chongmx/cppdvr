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

#include "video_recorder.h"
#include "accel_probe.h"
#include "platform/platform_process.h"

#include <filesystem>

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cppdvr {

// ════════════════════════════════════════════════════════════════════════════════
// Encoder capability probe
//
// Checks presence (ffmpeg -encoders) AND runtime capability (tiny test encode).
// Result is cached per encoder name — the smoke-test runs at most once per
// encoder per process lifetime (< 0.5 s per encoder).
// ════════════════════════════════════════════════════════════════════════════════
static bool ffmpeg_encoder_in_list(const std::string& name) {
    // Quick check: does ffmpeg list this encoder at all?
    static std::mutex                  mu;
    static std::map<std::string, bool> cache;
    static bool                        probed = false;

    std::lock_guard<std::mutex> lk(mu);
    if (!probed) {
        probed = true;
#ifdef _WIN32
        FILE* p = _popen("ffmpeg -encoders 2>nul", "r");
#else
        FILE* p = popen("ffmpeg -encoders 2>/dev/null", "r");
#endif
        if (p) {
            char line[256];
            while (fgets(line, sizeof(line), p)) {
                if (line[0] != ' ' || line[1] == '-') continue;
                const char* q = line + 1;
                while (*q == ' ') ++q;           // skip leading spaces
                while (*q && *q != ' ') ++q;     // skip flags column
                while (*q == ' ') ++q;           // skip separator
                char enc[64] = {};
                int i = 0;
                while (*q && *q != ' ' && i < 63) enc[i++] = *q++;
                enc[i] = '\0';
                if (i > 0) cache[enc] = true;
            }
#ifdef _WIN32
            _pclose(p);
#else
            pclose(p);
#endif
        }
    }
    auto it = cache.find(name);
    return it != cache.end() && it->second;
}

bool ffmpeg_has_encoder(const std::string& name) {
    // Two-stage check: listed AND actually works on this hardware.
    // The smoke-test encodes one 128×128 frame — takes < 300 ms per call.
    static std::mutex                  mu;
    static std::map<std::string, bool> cap_cache;

    if (!ffmpeg_encoder_in_list(name)) return false;

    std::lock_guard<std::mutex> lk(mu);
    auto it = cap_cache.find(name);
    if (it != cap_cache.end()) return it->second;

#ifdef _WIN32
    std::string cmd = "ffmpeg -loglevel error -f lavfi -i testsrc=s=128x128:r=1 -t 0.04 -c:v "
                      + name + " -f null NUL 2>nul";
#else
    std::string cmd = "ffmpeg -loglevel error -f lavfi -i testsrc=s=128x128:r=1 -t 0.04 -c:v "
                      + name + " -f null /dev/null 2>/dev/null";
#endif
    bool ok = (system(cmd.c_str()) == 0);
    cap_cache[name] = ok;
    return ok;
}

// Select the best available encoder for the given codec and accel preference.
// Returns the ffmpeg encoder name and quality args to append.
struct EncResolve {
    std::string encoder;   // e.g. "h264_nvenc"
    std::vector<std::string> quality_args;  // quality/preset flags for this encoder
};

static EncResolve resolve_encoder(const std::string& codec, EncodeAccel accel,
                                   int crf, const std::string& sw_preset) {
    bool is_h265 = (codec == "h265");

    // Software encoders (always available if ffmpeg is in PATH)
    auto sw = [&]() -> EncResolve {
        std::string enc = is_h265 ? "libx265" : "libx264";
        return { enc, { "-crf", std::to_string(crf), "-preset", sw_preset } };
    };

    switch (accel) {
        case EncodeAccel::Software:
            return sw();

        case EncodeAccel::CUDA: {
            std::string enc = is_h265 ? "hevc_nvenc" : "h264_nvenc";
            if (ffmpeg_has_encoder(enc))
                return { enc, { "-rc", "constqp", "-qp", std::to_string(crf) } };
            return sw();
        }

        case EncodeAccel::OtherHW: {
            std::string enc = is_h265 ? "hevc_qsv" : "h264_qsv";
            if (ffmpeg_has_encoder(enc))
                return { enc, { "-global_quality", std::to_string(crf) } };
            return sw();
        }

        case EncodeAccel::Auto: {
            // Try in order: NVENC → QSV → AMF → software
            struct { const char* enc265; const char* enc264; std::vector<std::string> qa; } candidates[] = {
                { "hevc_nvenc",  "h264_nvenc",
                  { "-rc", "constqp", "-qp", std::to_string(crf) } },
                { "hevc_qsv",   "h264_qsv",
                  { "-global_quality", std::to_string(crf) } },
                { "hevc_amf",   "h264_amf",
                  { "-quality", "balanced", "-rc", "cqp",
                    "-qp_i", std::to_string(crf), "-qp_p", std::to_string(crf) } },
            };
            for (auto& c : candidates) {
                const char* enc = is_h265 ? c.enc265 : c.enc264;
                if (ffmpeg_has_encoder(enc))
                    return { enc, c.qa };
            }
            return sw();
        }
    }
    return sw();
}

// ════════════════════════════════════════════════════════════════════════════════
// NAL codec + I-frame detection
// ════════════════════════════════════════════════════════════════════════════════
//
// Scans the first 1 KB of a raw frame for I-frame NAL units (SPS/PPS/IDR for
// H264; VPS/SPS/PPS/IRAP for H265).  Returns "h264" or "h265" only when an
// actual parameter-set or IDR NAL is found — NOT for AUD or P-frame slices.
//
// Why scan instead of checking byte 0 only?
//   Many DVR cameras prepend an Access Unit Delimiter (H264 type 9 / H265 type
//   35) before the SPS/PPS/IDR NALs on every I-frame.  Checking only the first
//   NAL byte would see the AUD and return "" even for real I-frames, causing the
//   recorder to drop every I-frame and never start ffmpeg.
//
static std::string rec_detect_codec(const uint8_t* data, size_t len) {
    if (len < 4) return "";
    // Limit scan to the first 1 KB — SPS/PPS always appear at the very start.
    const size_t scan = (len < 1024) ? len : 1024;

    for (size_t i = 0; i + 3 < scan; ) {
        // Locate next Annex-B start code (3- or 4-byte)
        size_t sc = 0;
        if (i + 4 <= scan && data[i]==0 && data[i+1]==0 && data[i+2]==0 && data[i+3]==1)
            sc = 4;
        else if (data[i]==0 && data[i+1]==0 && data[i+2]==1)
            sc = 3;

        if (sc == 0) { ++i; continue; }

        size_t nal_pos = i + sc;
        if (nal_pos >= scan) break;
        uint8_t nb = data[nal_pos];

        // ── H.265 NAL unit type (bits [14:9] of the 2-byte header) ───────────
        // I-frame indicators: IRAP pictures (16-23) and VPS/SPS/PPS (32-34).
        // AUD = 35 — intentionally excluded so P-frames with AUD are NOT matched.
        int h5 = (nb >> 1) & 0x3F;
        if ((h5 >= 16 && h5 <= 23) || (h5 >= 32 && h5 <= 34))
            return "h265";

        // ── H.264 NAL unit type (bits [4:0]) ─────────────────────────────────
        // I-frame indicators: IDR (5), SEI (6), SPS (7), PPS (8).
        // AUD = 9 — intentionally excluded.
        int h4 = nb & 0x1F;
        if (h4 >= 5 && h4 <= 8)
            return "h264";

        i = nal_pos + 1;   // advance past this NAL header, keep scanning
    }
    return "";
}

// ════════════════════════════════════════════════════════════════════════════════
// ffmpeg child-process wrapper (cross-platform via platform_process)
// ════════════════════════════════════════════════════════════════════════════════
struct FfmpegRecProc {
    PlatformProcess proc        {INVALID_PROCESS};
    PlatformPipe    stdin_pipe  {INVALID_PIPE};
    PlatformPipe    stdout_pipe {INVALID_PIPE};   // not used — ffmpeg writes to file
    PlatformPipe    stderr_pipe {INVALID_PIPE};
    std::thread     stderr_thr;

    using LogFn = std::function<void(const char*)>;

    bool start(RecordingFormat fmt, const std::string& codec,
               const std::string& filename, int framerate,
               EncodeAccel enc_accel = EncodeAccel::Auto,
               LogFn log_fn = nullptr) {

        std::vector<std::string> args = { "-loglevel", "warning" };

        if (fmt == RecordingFormat::MP4) {
            const std::string input_fmt = (codec == "h265") ? "hevc" : "h264";
            args.insert(args.end(), { "-f", input_fmt, "-i", "pipe:0" });

#if RECORDER_USE_COPY
            args.insert(args.end(), { "-c:v", "copy" });
            if (log_fn) log_fn("recorder: encode=stream-copy (lossless)");
#else
            EncResolve er = resolve_encoder(codec, enc_accel, RECORDER_CRF, RECORDER_PRESET);
            args.push_back("-c:v");
            args.push_back(er.encoder);
            for (auto& a : er.quality_args) args.push_back(a);
            if (log_fn) {
                std::string msg = "recorder: encode=" + er.encoder;
                log_fn(msg.c_str());
            }
#endif
            args.insert(args.end(), { "-y", filename });

        } else {
            // JPEG-frame input mode.
            // Output encoding is chosen from the filename extension:
            //   .mp4  → H.264 encoder (GPU or software, selected by enc_accel)
            //   other → MJPEG in AVI  (lossless JPEG passthrough)
            const bool to_mp4 = filename.size() > 4 &&
                                  filename.substr(filename.size() - 4) == ".mp4";
            args.insert(args.end(), {
                "-f", "image2pipe", "-vcodec", "mjpeg",
                "-framerate", std::to_string(framerate),
                "-i", "pipe:0"
            });
            if (to_mp4) {
                // Always encode to H.264 for MP4 output; use enc_accel for selection
                EncResolve er = resolve_encoder("h264", enc_accel, 18, "veryfast");
                args.push_back("-c:v");
                args.push_back(er.encoder);
                args.insert(args.end(), { "-pix_fmt", "yuv420p" });
                for (auto& a : er.quality_args) args.push_back(a);
                if (log_fn) {
                    std::string msg = "recorder: encode=" + er.encoder + " (mjpeg→mp4)";
                    log_fn(msg.c_str());
                }
            } else {
                args.insert(args.end(), {
                    "-c:v", "mjpeg", "-pix_fmt", "yuvj420p", "-q:v", "3"
                });
                if (log_fn) log_fn("recorder: encode=mjpeg (passthrough quality)");
            }
            args.insert(args.end(), { "-y", filename });
        }

        if (log_fn) {
            try {
                std::string abs = std::filesystem::absolute(filename).string();
                log_fn(("recorder: output -> " + abs).c_str());
            } catch (...) {
                log_fn(("recorder: output -> " + filename).c_str());
            }
            std::string cmd = "ffmpeg";
            for (auto& a : args) { cmd += ' '; cmd += a; }
            log_fn(("recorder: cmd: " + cmd).c_str());
        }

        proc = platform_spawn_process("ffmpeg", args, stdin_pipe, stdout_pipe, stderr_pipe);
        if (proc == INVALID_PROCESS) return false;

        // ffmpeg writes the output file directly — stdout pipe is not needed
        if (stdout_pipe != INVALID_PIPE) {
            platform_close_pipe(stdout_pipe);
            stdout_pipe = INVALID_PIPE;
        }

        if (log_fn && stderr_pipe != INVALID_PIPE) {
            stderr_thr = std::thread([this, log_fn]() {
                char buf[512];
                std::string line;
                int n;
                while ((n = platform_read_pipe(stderr_pipe, buf, sizeof(buf) - 1)) > 0) {
                    buf[n] = '\0';
                    line += buf;
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
        if (stdin_pipe == INVALID_PIPE || len == 0) return false;
        return platform_write_pipe(stdin_pipe, data, static_cast<int>(len))
               == static_cast<int>(len);
    }

    // Close stdin — signals ffmpeg to finalise the output container.
    void close_stdin() {
        if (stdin_pipe != INVALID_PIPE) {
            platform_close_pipe(stdin_pipe);
            stdin_pipe = INVALID_PIPE;
        }
    }

    // Wait up to timeout_ms for ffmpeg to exit; kills it on timeout.
    // When the process exits its stderr write end closes, which unblocks
    // stderr_thr's platform_read_pipe and allows the thread to be joined.
    bool wait_for_exit(int timeout_ms) {
        if (proc == INVALID_PROCESS) return true;
        int exit_code = 0;
        bool ok = (platform_wait_process(proc, exit_code, timeout_ms)
                   == PlatformProcessError::Success);
        if (!ok) platform_kill_process(proc);
        proc = INVALID_PROCESS;
        // Process exited/killed → its stderr write end is closed →
        // platform_read_pipe returns 0 → stderr_thr exits naturally → safe to join.
        // Join before closing the read-end handle to avoid handle-use-after-free.
        if (stderr_thr.joinable()) stderr_thr.join();
        if (stderr_pipe != INVALID_PIPE) {
            platform_close_pipe(stderr_pipe);
            stderr_pipe = INVALID_PIPE;
        }
        return ok;
    }

    void terminate() {
        close_stdin();
        if (proc != INVALID_PROCESS) {
            platform_kill_process(proc);
            proc = INVALID_PROCESS;
        }
        if (stderr_pipe != INVALID_PIPE) {
            platform_close_pipe(stderr_pipe);
            stderr_pipe = INVALID_PIPE;
        }
        if (stderr_thr.joinable()) stderr_thr.join();
    }

    void close_all() { terminate(); }
};

// ════════════════════════════════════════════════════════════════════════════════
// VideoRecorder::Impl
// ════════════════════════════════════════════════════════════════════════════════
struct VideoRecorder::Impl {

    // ── State (guarded by mtx) ────────────────────────────────────────────────
    mutable std::mutex      mtx;
    std::condition_variable cv;

    RecorderState           rec_state  {RecorderState::Idle};
    RecordingFormat         rec_fmt    {RecordingFormat::MP4};
    EncodeAccel             enc_accel  {EncodeAccel::Auto};
    std::string             out_fname;
    int                     rec_fps    {25};

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
        EncodeAccel     ea;
        int fps;
        {
            std::lock_guard<std::mutex> lk(mtx);
            fname = out_fname;
            fmt   = rec_fmt;
            ea    = enc_accel;
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

            // ── MP4: wait for the first REAL I-frame before starting ffmpeg ──
            // We rely on NAL byte inspection (rec_detect_codec) rather than the
            // DVRIP is_iframe flag, which is buggy and always true.  An I-frame
            // from the camera is a chunk that begins with SPS/PPS/IDR NAL units;
            // rec_detect_codec returns non-empty only for those.  A P-frame
            // starts with a non-IDR slice NAL and rec_detect_codec returns "".
            if (fmt == RecordingFormat::MP4 && !ffp_started) {
                std::string codec = rec_detect_codec(frame.data.data(), frame.data.size());
                if (codec.empty()) {
                    // P-frame — drop it and keep waiting for an I-frame
                    ++count_dropped;
                    if (count_dropped == 1 || count_dropped % 50 == 0)
                        log("recorder: waiting for I-frame (%zu frames dropped)...",
                            count_dropped.load());
                    continue;
                }
                // codec determined from NAL bytes — reliable

                log("recorder: starting ffmpeg  format=mp4  codec=%s  crf=%d  copy=%d",
                    codec.c_str(), RECORDER_CRF, RECORDER_USE_COPY);
                log("recorder: -> %s", fname.c_str());

                if (!ffp.start(RecordingFormat::MP4, codec, fname, fps, ea, [this](const char* m){ log(m); })) {
                    log("recorder: ffmpeg failed to start (is ffmpeg in PATH?)");
                    aborted = true; break;
                }
                ffp_started = true;
            }

            // ── MJPEG: start ffmpeg on the very first JPEG frame ──────────────
            if (fmt == RecordingFormat::MJPEG && !ffp_started) {
                log("recorder: starting ffmpeg  format=mjpeg  fps=%d", fps);
                log("recorder: -> %s", fname.c_str());

                if (!ffp.start(RecordingFormat::MJPEG, "", fname, fps, ea, [this](const char* m){ log(m); })) {
                    log("recorder: ffmpeg failed to start (is ffmpeg in PATH?)");
                    aborted = true; break;
                }
                ffp_started = true;
            }

            // ── Write frame to ffmpeg stdin ────────────────────────────────────
            if (!ffp.write_data(frame.data.data(), frame.data.size())) {
                log("recorder: ffmpeg stdin write error — ffmpeg may have exited");
                aborted = true; break;
            }
            ++count_written;
            // Progress heartbeat — logged at 25, 50, 100, 200, … frames
            {
                size_t n = count_written.load();
                if (n == 25 || n == 100 || (n % 250 == 0))
                    log("recorder: %zu frames written so far", n);
            }
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

bool VideoRecorder::init_standalone() {
    d_->initialized = true;
    return true;
}

void VideoRecorder::feed_raw_frame(const uint8_t* data, size_t size,
                                    const std::string& codec, bool is_iframe) {
    d_->feed_raw(data, size, codec, is_iframe);
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

void VideoRecorder::set_encode_accel(EncodeAccel accel) {
    std::lock_guard<std::mutex> lk(d_->mtx);
    d_->enc_accel = accel;
}

EncodeAccel VideoRecorder::get_encode_accel() const {
    std::lock_guard<std::mutex> lk(d_->mtx);
    return d_->enc_accel;
}

} // namespace cppdvr
