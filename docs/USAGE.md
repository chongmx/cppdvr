# cppdvr — Application Usage Guide

This guide explains how to use the cppdvr C API (`cppdvr_api.h`) from an
application.  All examples are C, but the API is usable from C++ or C++/CLI
as-is.

---

## Architecture

```
Camera (DVR / DVRIP)
        │ raw H.264 / H.265 NAL frames
        ▼
  camera_thread ──────────────────────────────────────► raw_frame_cb (MP4 recording)
        │
        │ raw_queue
        ▼
  pipeline_thread (ffmpeg  pipe:0 → pipe:1  MJPEG decode)
        │ decoded JPEG frames
        ▼
  overlay_worker ──── text / RGB draw ──────────────────► overlay_jpeg_cb (overlaid recording)
        │
        ├──► jpeg_cb    (GUI display)
        └──► latest_jpeg  (stream_get_frame polling)
```

Two independent recording paths branch off from different points:

| Mode | Callback | Overlay | Quality |
|------|----------|---------|---------|
| **MP4** | `raw_frame_cb` (set by `recorder_init_with_stream`) | No — raw NAL bytes | Lossless (no re-encode) |
| **MJPEG** | `overlay_jpeg_cb` → `recorder_feed_jpeg()` | Yes — text/RGB drawn before encode | One extra JPEG encode cycle |

---

## Quick-start: stream display only

```c
#include "cppdvr_api.h"

StreamHandle sh = stream_create("192.168.1.10", 0, "admin", "", 0);
stream_start(sh, "Main");

// Poll from your display loop (e.g. every 40 ms):
size_t size;
uint8_t* jpeg = stream_get_frame(sh, &size);
if (jpeg) {
    // decode / render jpeg ...
    stream_free_frame(jpeg);
}

// On shutdown:
stream_stop(sh);
stream_destroy(sh);
```

`stream_get_frame()` returns a heap copy of the most-recently decoded JPEG.
Free it with `stream_free_frame()`.  Returns NULL if no frame has arrived yet
(camera buffers ~2–3 s of H.265 before the first JPEG appears).

---

## Recording with overlay (MJPEG)

This is the correct pattern for burning text into every recorded frame.

### How it works

1. Configure overlay text via `stream_overlay_*` before `stream_start()`.
2. Register `stream_set_overlay_jpeg_callback()` — fired with the
   **post-overlay** JPEG for every frame.
3. Inside that callback, call `recorder_feed_jpeg()` to pipe the frame to
   the recorder.
4. After recording is done, call `recorder_save()`.

### Minimal example

```c
#include "cppdvr_api.h"

static RecorderHandle g_rec = NULL;

static void on_overlay_jpeg(const uint8_t* jpeg, size_t size,
                             uint32_t frame_id, void* userdata)
{
    if (g_rec) recorder_feed_jpeg(g_rec, jpeg, size);
}

int main(void)
{
    StreamHandle   sh  = stream_create("192.168.1.10", 0, "admin", "", 0);
    RecorderHandle rec = recorder_create();
    g_rec = rec;

    // ── Overlay text ──────────────────────────────────────────────────────
    stream_overlay_set_scale(sh, 4);          // 4 × 8 = 32 px glyphs
    stream_overlay_set_cursor(sh, -1, -1);    // auto: bottom-left corner
    stream_overlay_print(sh, "Hello\nWorld"); // '\n' = new line

    // ── Wire up recording ─────────────────────────────────────────────────
    stream_set_overlay_jpeg_callback(sh, on_overlay_jpeg, NULL);
    recorder_init_with_stream(rec, sh);
    recorder_start(rec, "output.mp4", RECORDER_FORMAT_MJPEG, 25);

    // ── Start streaming (must be last) ────────────────────────────────────
    stream_start(sh, "Main");

    // … record for as long as needed; update overlay text from any thread:
    stream_overlay_print(sh, "Updated text\nLine 2");

    // ── Stop ─────────────────────────────────────────────────────────────
    stream_stop(sh);
    recorder_save(rec);   // finalise and flush the container

    recorder_deinit(rec);
    recorder_destroy(rec);
    stream_destroy(sh);
    return 0;
}
```

### Important: callback slot separation

The library has **two** JPEG callbacks:

| Function | When fired | Intended use |
|----------|-----------|--------------|
| `stream_set_jpeg_callback` | Pre-overlay JPEG | GUI display |
| `stream_set_overlay_jpeg_callback` | Post-overlay JPEG | Recording |

Using `stream_set_overlay_jpeg_callback` for recording leaves
`stream_set_jpeg_callback` free for the GUI to use simultaneously without
conflict.  Using `stream_set_jpeg_callback` for recording would record frames
**before** overlay text has been burned in.

---

## Recording raw MP4 (no overlay)

```c
StreamHandle   sh  = stream_create("192.168.1.10", 0, "admin", "", 0);
RecorderHandle rec = recorder_create();

// recorder_init_with_stream registers the raw-frame hook automatically.
recorder_init_with_stream(rec, sh);
recorder_start(rec, "output.mp4", RECORDER_FORMAT_MP4, 0);

stream_start(sh, "Main");

// … record …

stream_stop(sh);
recorder_save(rec);

recorder_deinit(rec);
recorder_destroy(rec);
stream_destroy(sh);
```

MP4 recording is done from the camera thread before any JPEG decode step.
It records the original H.264/H.265 bitstream without re-encoding, so there
is no quality loss.  Overlay text cannot be added to MP4 recordings.

---

## Overlay API

### Single-box (simple)

```c
stream_overlay_set_scale(sh, 4);       // glyph scale: 0=auto, 1=8px, 2=16px, 4=32px
stream_overlay_set_cursor(sh, -1, -1); // -1,-1 = auto bottom-left with proportional margin
stream_overlay_print(sh, "Line 1\nLine 2\nLine 3");
stream_overlay_clear(sh);              // hide until next print
```

`stream_overlay_print()` is thread-safe and can be called at any rate from
any thread — use it to push live sensor readings into the video.

### Multi-box (word-wrap, alignment, anchor)

Up to `STREAM_OVERLAY_MAX_BOXES` (default 4) independent text regions:

```c
// Box 0 — top-left, left-aligned, width 300 px, auto scale
stream_overlay_box_configure(sh, /*idx*/0,
    /*x*/10, /*y*/10, /*box_w*/300,
    /*scale*/0,
    STREAM_OVERLAY_ALIGN_LEFT,
    STREAM_OVERLAY_ANCHOR_TOP_LEFT);
stream_overlay_box_print(sh, 0, "Status: OK\nSpeed: 1.2 m/s");

// Box 1 — bottom-right, right-aligned
stream_overlay_box_configure(sh, /*idx*/1,
    /*x*/10, /*y*/10, /*box_w*/200,
    /*scale*/2,
    STREAM_OVERLAY_ALIGN_RIGHT,
    STREAM_OVERLAY_ANCHOR_BOTTOM_RIGHT);
stream_overlay_box_print(sh, 1, "AcmeCorp");

stream_overlay_box_clear(sh, 0);      // hide box 0
stream_overlay_box_clear_all(sh);     // hide all boxes
```

Layout rule: `x` and `y` are **inward** pixel offsets from the anchor corner.
For example with `ANCHOR_BOTTOM_RIGHT`, `x=10, y=10` places the box 10 px
left of the right edge and 10 px up from the bottom edge.

### Custom RGB drawing

For pixel-level drawing (shapes, images, etc.), register a frame callback:

```c
static void draw_frame(uint8_t* rgb, uint64_t ts_us,
                       int w, int h, int bpp, void* ud)
{
    // rgb is packed R,G,B — w*h*3 bytes total.
    // Draw directly; the library re-encodes to JPEG after this returns.
    for (int y = 0; y < 10; ++y)
        for (int x = 0; x < 10; ++x) {
            rgb[(y*w + x)*3 + 0] = 255; // R
            rgb[(y*w + x)*3 + 1] = 0;   // G
            rgb[(y*w + x)*3 + 2] = 0;   // B
        }
}

stream_set_overlay_frame_callback(sh, draw_frame, NULL);
```

Setting this callback triggers a JPEG decode → RGB → re-encode cycle on every
frame.  Text overlay and frame overlay can be used simultaneously.

---

## Display + record simultaneously

```c
static RecorderHandle g_rec  = NULL;
static int            g_recording = 0;

// Display callback (GUI thread-safe)
static void on_jpeg(const uint8_t* jpeg, size_t size, uint32_t id, void* ud)
{
    // Push to your image control / texture upload here
}

// Recording callback — fires with overlaid frame
static void on_overlay_jpeg(const uint8_t* jpeg, size_t size, uint32_t id, void* ud)
{
    if (g_recording && g_rec)
        recorder_feed_jpeg(g_rec, jpeg, size);
}

// Setup:
stream_set_jpeg_callback(sh, on_jpeg, NULL);
stream_set_overlay_jpeg_callback(sh, on_overlay_jpeg, NULL);

// To start recording mid-stream:
recorder_start(rec, "clip.mp4", RECORDER_FORMAT_MJPEG, 25);
g_recording = 1;

// To stop:
g_recording = 0;
recorder_save(rec);
```

---

## Hardware acceleration

### Decode acceleration (H.264 / H.265 → MJPEG pipeline)

The decode accelerator controls how ffmpeg decodes the raw camera bitstream.
Call `stream_set_decode_accel()` **before** `stream_start()`.

```c
// Constants (from cppdvr_api.h)
// CPPDVR_DECODE_ACCEL_SOFTWARE  0  — always software
// CPPDVR_DECODE_ACCEL_CUDA      1  — NVIDIA NVDEC
// CPPDVR_DECODE_ACCEL_OTHER_HW  2  — d3d11va (Windows) / vaapi (Linux)
// CPPDVR_DECODE_ACCEL_AUTO      3  — auto-probe, best available (default)

stream_set_decode_accel(sh, CPPDVR_DECODE_ACCEL_AUTO);     // default — no call needed
stream_set_decode_accel(sh, CPPDVR_DECODE_ACCEL_CUDA);     // force NVIDIA NVDEC
stream_set_decode_accel(sh, CPPDVR_DECODE_ACCEL_OTHER_HW); // force d3d11va / vaapi
stream_set_decode_accel(sh, CPPDVR_DECODE_ACCEL_SOFTWARE); // force software

// Query current setting:
int accel = stream_get_decode_accel(sh);
```

**Auto mode behavior:**

1. Probes `ffmpeg -hwaccels` once at first call (result cached for the process lifetime).
2. On Windows: tries `d3d11va` first, then `cuda`.  On Linux: tries `vaapi` first, then `cuda`.
3. If the selected hardware accelerator produces no decoded frames on the first
   pipeline attempt, automatically falls back to software for all subsequent
   retries — no application code needed.

The library also normalises the decoded pixel format to `yuv420p` before
the software MJPEG encoder regardless of which accelerator is chosen, so
hardware and software paths produce identical JPEG output.

**Raw override** — for edge cases where you need to pass a specific ffmpeg
hwaccel string not covered by the enum:

```c
stream_set_hwaccel(sh, "dxva2");   // raw ffmpeg -hwaccel value
stream_set_hwaccel(sh, "");        // clear override (use decode_accel enum again)
```

When the raw override is non-empty it takes precedence over `decode_accel`.

---

### Encode acceleration (recorder re-encode / MJPEG → MP4)

The encode accelerator controls which video encoder ffmpeg uses when the
recorder needs to re-encode video.

**When it applies:**
- MJPEG recording saved as `.mp4` — JPEG frames are re-encoded to H.264.
- MP4 re-encode mode (`RECORDER_USE_COPY=0`) — raw NAL stream is re-encoded.

**When it does NOT apply:**
- MP4 stream-copy mode (`RECORDER_USE_COPY=1`, the default) — raw NAL bytes
  are remuxed without re-encoding; no encoder is involved.
- MJPEG AVI/MKV output — JPEG frames are written directly; no encoder is used.

```c
// Constants (from cppdvr_api.h)
// CPPDVR_ENCODE_ACCEL_SOFTWARE  0  — libx264 / libx265
// CPPDVR_ENCODE_ACCEL_CUDA      1  — h264_nvenc / hevc_nvenc  (NVIDIA)
// CPPDVR_ENCODE_ACCEL_OTHER_HW  2  — h264_qsv / hevc_qsv  (Intel QuickSync)
// CPPDVR_ENCODE_ACCEL_AUTO      3  — probe best available (default)

recorder_set_encode_accel(rec, CPPDVR_ENCODE_ACCEL_AUTO);     // default — no call needed
recorder_set_encode_accel(rec, CPPDVR_ENCODE_ACCEL_CUDA);     // force NVIDIA NVENC
recorder_set_encode_accel(rec, CPPDVR_ENCODE_ACCEL_OTHER_HW); // force Intel QSV
recorder_set_encode_accel(rec, CPPDVR_ENCODE_ACCEL_SOFTWARE); // force libx264/libx265

// Query current setting:
int accel = recorder_get_encode_accel(rec);
```

**Auto mode behavior:**

1. Checks `ffmpeg -encoders` to see which encoders are compiled in.
2. Runs a small smoke-test encode (128×128, ~0.3 s) for each candidate to
   confirm the hardware actually works on this machine — catches the common
   case where an encoder is compiled into ffmpeg but the GPU is absent or has
   outdated drivers.
3. Tries in order: **NVENC → QSV → AMF → libx264/libx265**.
4. Result is cached for the process lifetime — the probe runs at most once
   per encoder candidate.

**Quality parameters used by each encoder:**

| Encoder | Mode | Parameter |
|---------|------|-----------|
| libx264 / libx265 | CRF | `-crf 18 -preset veryfast` |
| h264_nvenc / hevc_nvenc | Const QP | `-rc constqp -qp 18` |
| h264_qsv / hevc_qsv | Global quality | `-global_quality 18` |
| h264_amf / hevc_amf | Const QP | `-rc cqp -qp_i 18 -qp_p 18` |

Lower QP/CRF = better quality and larger file.  These defaults target a
visually near-lossless output for surveillance footage.

---

### Typical configuration for a UI settings panel

```c
// Map a combo-box selection (0–3) directly to the accel constant:
void apply_settings(StreamHandle sh, RecorderHandle rec,
                    int decode_combo, int encode_combo)
{
    // decode_combo: 0=Auto, 1=Software, 2=CUDA, 3=Platform HW
    static const int decode_map[] = {
        CPPDVR_DECODE_ACCEL_AUTO,
        CPPDVR_DECODE_ACCEL_SOFTWARE,
        CPPDVR_DECODE_ACCEL_CUDA,
        CPPDVR_DECODE_ACCEL_OTHER_HW,
    };
    // encode_combo: 0=Auto, 1=Software, 2=CUDA, 3=Platform HW
    static const int encode_map[] = {
        CPPDVR_ENCODE_ACCEL_AUTO,
        CPPDVR_ENCODE_ACCEL_SOFTWARE,
        CPPDVR_ENCODE_ACCEL_CUDA,
        CPPDVR_ENCODE_ACCEL_OTHER_HW,
    };
    stream_set_decode_accel(sh, decode_map[decode_combo]);
    recorder_set_encode_accel(rec, encode_map[encode_combo]);
}
```

Both calls must be made **before** `stream_start()` for decode, and before
`recorder_start()` for encode.  The encoder setting is safe to change between
recordings without stopping the stream.

---

### JPEG decode/encode backend (overlay pipeline)

The overlay pipeline (JPEG decode → RGB draw → JPEG encode) has its own
backend selector, independent of the ffmpeg accelerators above.

```c
// CPPDVR_JPEG_BACKEND_STB           0  — stb_image (default, zero deps)
// CPPDVR_JPEG_BACKEND_LIBJPEG_TURBO 1  — SIMD (SSE2/AVX2/NEON) — fast on CPU
// CPPDVR_JPEG_BACKEND_NVJPEG        2  — NVIDIA GPU JPEG — lowest latency

// Query availability:
if (cppdvr_jpeg_backend_available(CPPDVR_JPEG_BACKEND_LIBJPEG_TURBO))
    cppdvr_set_jpeg_backend(CPPDVR_JPEG_BACKEND_LIBJPEG_TURBO);

// Or let the library auto-select (default, happens inside stream_start()):
stream_set_auto_jpeg_backend(sh, 1);  // 1 = on (default)
```

Auto-selection order: nvJPEG → libjpeg-turbo → stb_image.  When
`auto_jpeg_backend` is true (the default), `stream_start()` promotes to
the fastest available backend if the process is still at the stb default.

---

## Full lifecycle

```
stream_create()
    │
    ├── stream_set_decode_accel()      ← HW decode: Auto/CUDA/OtherHW/Software
    ├── stream_overlay_set_scale()     ← configure before start
    ├── stream_overlay_print()
    ├── stream_set_jpeg_callback()     ← display (optional)
    ├── stream_set_overlay_jpeg_callback()  ← recording (optional)
    │
    ├── recorder_create()
    ├── recorder_set_encode_accel()    ← HW encode: Auto/CUDA/OtherHW/Software
    ├── recorder_init_with_stream()
    ├── recorder_start()
    │
stream_start()          ← starts camera + ffmpeg + HTTP threads
    │                      (probes hwaccels / JPEG backend here)
    │
    │   [running — callbacks fire from background threads]
    │   stream_overlay_print()          ← safe from any thread
    │   recorder_pause() / recorder_resume()
    │   recorder_set_encode_accel()     ← safe to change between recordings
    │
stream_stop()           ← blocks until all threads exit
recorder_save()         ← flush and finalise output file
    │
recorder_deinit()
recorder_destroy()
stream_destroy()
```

Rules:
- Configure `decode_accel` and callbacks **before** `stream_start()`.
- `stream_overlay_print()` is the only call safe to make **after** start from
  any thread.  All other configuration (`set_scale`, `set_cursor`,
  `box_configure`, `set_decode_accel`) must be done before start.
- `stream_stop()` is synchronous — when it returns all callbacks have
  finished and no more will fire.
- Always call `recorder_save()` after `stream_stop()`, never before.
- Always call `recorder_deinit()` before `recorder_destroy()`.

---

## Recorder state machine

```
IDLE ──recorder_start()──► RECORDING ──recorder_pause()──► PAUSED
                               │                               │
                           recorder_save()           recorder_resume()──► RECORDING
                           recorder_discard()
                               │
                              IDLE
```

Check state with `recorder_state()`.  `recorder_frames_recorded()` and
`recorder_frames_dropped()` reset to 0 on each `recorder_start()`.

---

## Common pitfalls

**No frames for 2–3 seconds after start** — normal for H.265 cameras.  ffmpeg
buffers several seconds of input before flushing the first JPEG.  Wait at
least 15 seconds before concluding the camera is unreachable.

**Blank display after restart** — `stream_stop()` + `stream_start()` restarts
all threads.  Re-register callbacks after restart if they were set on
per-run state.

**Hardware decode selected but image is blank or corrupted** — This should not
happen with the default `CPPDVR_DECODE_ACCEL_AUTO` setting, which automatically
falls back to software if the hardware path produces no frames.  If you forced
a specific accelerator (`CUDA` or `OTHER_HW`) on a machine that doesn't support
it, switch to `AUTO` or `SOFTWARE`.  The library adds pixel-format normalisation
filters (`hwdownload`, `format=yuv420p`) automatically, so a correctly supported
hardware decoder should always produce valid JPEG output.

**Hardware encoder probe is slow on first recording** — In `AUTO` mode the
encoder probe runs once on the first `recorder_start()` call (a short
smoke-test encode per candidate, ~0.3 s each).  The result is cached — all
subsequent recordings use the cached encoder instantly.  To avoid the delay
on first recording, call `recorder_set_encode_accel(rec, CPPDVR_ENCODE_ACCEL_SOFTWARE)`
if you know you are on a machine without a GPU.

**Recording produces no frames** — Verify you called
`stream_set_overlay_jpeg_callback()` (not `stream_set_jpeg_callback()`) and
that `recorder_start()` was called **before** `stream_start()`.

**Overlay text not showing up** — Text overlay requires a JPEG decode/encode
cycle.  It only appears when either text or an `overlay_frame_callback` is
set.  If you only set `stream_set_jpeg_callback()` (no overlay callback and no
text), the pre-overlay JPEG fires with no decode cycle.  Use
`stream_set_overlay_jpeg_callback()` for the recording path to trigger the
overlay cycle.

---

## Thread safety

| Function | Thread safety |
|----------|--------------|
| `stream_overlay_print()` | Safe from any thread at any rate |
| `stream_overlay_box_print()` | Safe from any thread at any rate |
| `stream_overlay_clear()` | Safe from any thread |
| `stream_get_frame()` | Safe from any thread |
| `recorder_feed_jpeg()` | Safe from any thread (called from callback) |
| `recorder_pause()` / `recorder_resume()` | Safe from any thread |
| `recorder_set_encode_accel()` | Safe between recordings (not while recording) |
| All other API calls | Call from one thread; not safe to race |

Callbacks fire from **background threads** owned by the library.  Do not call
blocking operations (file I/O, UI updates that take locks) inside callbacks —
copy the data and hand it off to your own worker thread.
