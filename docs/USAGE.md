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

## Full lifecycle

```
stream_create()
    │
    ├── stream_overlay_set_scale()     ← configure before start
    ├── stream_overlay_print()
    ├── stream_set_jpeg_callback()     ← display (optional)
    ├── stream_set_overlay_jpeg_callback()  ← recording (optional)
    │
    ├── recorder_create()
    ├── recorder_init_with_stream()
    ├── recorder_start()
    │
stream_start()          ← starts camera + ffmpeg + HTTP threads
    │
    │   [running — callbacks fire from background threads]
    │   stream_overlay_print()  ← can be called at any time
    │   recorder_pause() / recorder_resume()
    │
stream_stop()           ← blocks until all threads exit
recorder_save()         ← flush and finalise output file
    │
recorder_deinit()
recorder_destroy()
stream_destroy()
```

Rules:
- Configure callbacks and overlay **before** `stream_start()`.
- `stream_overlay_print()` is the only call safe to make **after** start from
  any thread.  All other configuration (`set_scale`, `set_cursor`,
  `box_configure`) should be done before start.
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

**Hardware decode breaks the image** — The default hwaccel is `""` (software
decode).  Do not pass `"auto"` unless you have verified your ffmpeg build can
decode H.265 into a pixel format compatible with the MJPEG encoder.  To opt
in: `stream_set_hwaccel(sh, "d3d11va")` before `stream_start()`.

**Overlay text not showing up** — Text overlay requires a JPEG decode/encode
cycle.  It only appears when either text or an `overlay_frame_callback` is
set.  If you only set `stream_set_jpeg_callback()` (no overlay callback and no
text), the pre-overlay JPEG fires with no decode cycle and overlay_worker is a
no-op.  Use `stream_set_overlay_jpeg_callback()` for the recording path so
the overlay cycle is triggered.

**Recording produces no frames** — Verify you called
`stream_set_overlay_jpeg_callback()` (not `stream_set_jpeg_callback()`) and
that `recorder_start()` was called **before** `stream_start()`.

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
| All other API calls | Call from one thread; not safe to race |

Callbacks fire from **background threads** owned by the library.  Do not call
blocking operations (file I/O, UI updates that take locks) inside callbacks —
copy the data and hand it off to your own worker thread.
