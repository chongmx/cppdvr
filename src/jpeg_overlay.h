#pragma once
/**
 * src/jpeg_overlay.h — JPEG decode / draw-text / encode helpers
 *
 * Internal (not exported) — used by stream_server.cpp to apply overlays.
 * Based on stb_image + stb_image_write (single-header, MIT-licensed).
 */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Alignment and anchor constants ─────────────────────────────────────────── */
#define OVERLAY_ALIGN_LEFT  0
#define OVERLAY_ALIGN_RIGHT 1

/* Anchor corner for jpeg_overlay_draw_textbox.
 * x, y are inward pixel offsets from that corner. */
#define OVERLAY_ANCHOR_TOP_LEFT     0
#define OVERLAY_ANCHOR_TOP_RIGHT    1
#define OVERLAY_ANCHOR_BOTTOM_LEFT  2
#define OVERLAY_ANCHOR_BOTTOM_RIGHT 3

/* ── Simple full-JPEG overlay (decode → draw → encode) ───────────────────────
 * x, y     : top-left origin of the first character.
 * r, g, b  : text colour.
 * quality  : 1–100 (stb_image_write scale; 90 = high quality).
 * out_data : heap-allocated result — caller must free().
 * Returns 1 on success, 0 on decode/encode failure. */
int jpeg_overlay_text(
    const uint8_t* jpeg_in, size_t in_size,
    const char*    text,
    int            x, int y,
    uint8_t        r, uint8_t g, uint8_t b,
    int            quality,
    uint8_t**      out_data, size_t* out_size);

/* ── Single-region text draw ─────────────────────────────────────────────────
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

/* ── Decode / encode helpers ─────────────────────────────────────────────────
 * jpeg_decode_rgb: Returns malloc'd RGB buffer (3 B/px). Caller must free().
 * jpeg_encode_rgb: Returns malloc'd JPEG in *out_data.  Caller must free(). */
uint8_t* jpeg_decode_rgb(const uint8_t* jpeg_in, size_t in_size,
                          int* out_width, int* out_height);

int jpeg_encode_rgb(
    const uint8_t* rgb,     int width, int height,
    int            quality,
    uint8_t**      out_data, size_t* out_size);

#ifdef __cplusplus
}
#endif
