#pragma once
/**
 * src/jpeg_overlay.h — JPEG decode / draw-text / encode helpers (internal)
 *
 * Not exported. Used by stream_server.cpp to apply overlays to JPEG frames.
 *
 * Three runtime-selectable JPEG backends (selected via jpeg_backend_set):
 *   JPEG_BACKEND_STB           — stb_image (default, zero external deps)
 *   JPEG_BACKEND_LIBJPEG_TURBO — SIMD (SSE2/AVX2/NEON); enabled when
 *                                 libjpeg-turbo is found at cmake configure time
 *   JPEG_BACKEND_NVJPEG        — NVIDIA GPU; enabled when CUDA toolkit found
 *                                 and -DCPPDVR_NVJPEG=ON
 *
 * The active backend is process-global. Switch before calling decode/encode.
 * The overlay pipeline in stream_server.cpp is single-threaded per stream,
 * so no external locking is needed around decode/encode calls.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Backend constants ────────────────────────────────────────────────────────
 * Keep in sync with CPPDVR_JPEG_BACKEND_* in cppdvr_api.h.               */
#define JPEG_BACKEND_STB           0   /* stb_image   — portable, no deps      */
#define JPEG_BACKEND_LIBJPEG_TURBO 1   /* libjpeg-turbo — SIMD (SSE/AVX/NEON)  */
#define JPEG_BACKEND_NVJPEG        2   /* nvJPEG      — NVIDIA GPU              */

/* Returns 1 if this backend was compiled in and hardware / libs are available.
 * Calling jpeg_backend_available(JPEG_BACKEND_NVJPEG) will attempt a lazy GPU
 * init on the first call; subsequent calls are instant. */
int  jpeg_backend_available(int backend);

/* Switch the active backend. Returns 1 on success, 0 if unavailable.
 * On failure the previously active backend is unchanged. */
int  jpeg_backend_set(int backend);

/* Return the currently active backend constant. */
int  jpeg_backend_get(void);

/* ── Alignment and anchor constants ─────────────────────────────────────────── */
#define OVERLAY_ALIGN_LEFT  0
#define OVERLAY_ALIGN_RIGHT 1

/* Anchor corner for jpeg_overlay_draw_textbox.
 * x, y are inward pixel offsets from that corner. */
#define OVERLAY_ANCHOR_TOP_LEFT     0
#define OVERLAY_ANCHOR_TOP_RIGHT    1
#define OVERLAY_ANCHOR_BOTTOM_LEFT  2
#define OVERLAY_ANCHOR_BOTTOM_RIGHT 3

/* ── Full-JPEG overlay (decode → draw → encode in one call) ──────────────────
 * x, y     : top-left origin of the first character.
 * r, g, b  : text colour.
 * quality  : 1–100 (100 = best quality / largest file).
 * out_data : heap-allocated result — caller must free().
 * Returns 1 on success, 0 on decode/encode failure. */
int jpeg_overlay_text(
    const uint8_t* jpeg_in, size_t in_size,
    const char*    text,
    int            x, int y,
    uint8_t        r, uint8_t g, uint8_t b,
    int            quality,
    uint8_t**      out_data, size_t* out_size);

/* ── Single-region text draw (operates on an already-decoded RGB buffer) ──────
 * Draw 'text' directly on a packed RGB buffer.  '\n' starts a new line at x.
 * Shadow offset equals scale for legibility on any background.
 * scale: each glyph pixel becomes a scale×scale block (1 = native 8×8 px). */
void jpeg_overlay_draw_text(
    uint8_t*    rgb,  int width, int height,
    const char* text, int x,    int y,
    uint8_t     r,    uint8_t g, uint8_t b,
    int         scale);

/* ── Text-box draw (word-wrap + alignment + anchor) ──────────────────────────
 * Draws word-wrapped, aligned text anchored to a corner of the image.
 *
 * x, y   : inward pixel offset from the anchor corner.
 * box_w  : text-box width in pixels used for word-wrap and right-alignment.
 *           0 = unconstrained (only '\n' causes line breaks; no wrap).
 * scale  : glyph scale factor (1 = native 8×8 px).
 * align  : OVERLAY_ALIGN_LEFT or OVERLAY_ALIGN_RIGHT.
 * anchor : OVERLAY_ANCHOR_* — which image corner x,y are measured from.
 *
 * '\n' in text forces an explicit line break regardless of box_w.
 * Words longer than box_w are hard-broken at the box boundary. */
void jpeg_overlay_draw_textbox(
    uint8_t*    rgb,   int width, int height,
    const char* text,
    int x, int y, int box_w,
    uint8_t r, uint8_t g, uint8_t b,
    int scale, int align, int anchor);

/* ── Decode / encode (dispatch through the active backend) ───────────────────
 *
 * jpeg_decode_rgb : Returns malloc()'d RGB buffer (3 bytes/pixel, packed R-G-B).
 *                   Caller must free() the returned pointer.
 *                   Returns NULL on failure.
 *
 * jpeg_encode_rgb : Encodes RGB to JPEG. Sets *out_data (malloc'd) and *out_size.
 *                   Caller must free() *out_data.
 *                   Returns 1 on success, 0 on failure.                       */
uint8_t* jpeg_decode_rgb(const uint8_t* jpeg_in, size_t in_size,
                          int* out_width, int* out_height);

int jpeg_encode_rgb(
    const uint8_t* rgb,     int width, int height,
    int            quality,
    uint8_t**      out_data, size_t* out_size);

#ifdef __cplusplus
}
#endif
