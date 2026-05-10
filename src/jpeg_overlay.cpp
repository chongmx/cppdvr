/**
 * src/jpeg_overlay.cpp — JPEG decode / draw-text / encode with multi-backend support
 *
 * Compile-time backend gates (set by CMakeLists.txt):
 *   CPPDVR_HAS_LIBJPEG_TURBO — libjpeg-turbo found; enables SIMD decode/encode
 *   CPPDVR_HAS_NVJPEG         — CUDA + nvJPEG found; enables GPU decode/encode
 *
 * STB is always compiled in as the zero-dependency fallback.
 */

/* ── STB (always compiled; implementation defined exactly once here) ──────── */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#include <stb_image.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

/* ── libjpeg-turbo ───────────────────────────────────────────────────────── */
#ifdef CPPDVR_HAS_LIBJPEG_TURBO
#  include <turbojpeg.h>
/* 3.0+ provides tj3* API with size_t buffer sizes and no deprecated-decl.
 * 2.x uses tjDecompress2/tjCompress2 with unsigned long sizes (still works
 * in 3.x as deprecated compat, but cleaner to branch explicitly).         */
#  if defined(LIBJPEG_TURBO_VERSION_NUMBER) && LIBJPEG_TURBO_VERSION_NUMBER >= 3000000
#    define CPPDVR_TJ_V3
#  endif
#endif

/* ── nvJPEG (CUDA) ───────────────────────────────────────────────────────── */
#ifdef CPPDVR_HAS_NVJPEG
#  include <nvjpeg.h>
#  include <cuda_runtime.h>
#endif

#include "jpeg_overlay.h"

#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <mutex>
#include <vector>

/* ══════════════════════════════════════════════════════════════════════════════
 * Backend state
 * ══════════════════════════════════════════════════════════════════════════════ */

static std::atomic<int> g_backend{ JPEG_BACKEND_STB };

/* ── nvJPEG lazy-init state ─────────────────────────────────────────────── */
#ifdef CPPDVR_HAS_NVJPEG

static std::mutex             g_nvjpeg_mu;
static bool                   g_nvjpeg_init_done   = false;
static bool                   g_nvjpeg_init_ok     = false;
static nvjpegHandle_t         g_nvjpeg_handle      = nullptr;
static nvjpegJpegState_t      g_nvjpeg_dec_state   = nullptr;
static nvjpegEncoderState_t   g_nvjpeg_enc_state   = nullptr;
static nvjpegEncoderParams_t  g_nvjpeg_enc_params  = nullptr;
static cudaStream_t           g_cuda_stream        = nullptr;

/* Called with g_nvjpeg_mu already held. */
static bool nvjpeg_ensure_init_locked() {
    if (g_nvjpeg_init_done) return g_nvjpeg_init_ok;
    g_nvjpeg_init_done = true;

    if (nvjpegCreate(NVJPEG_BACKEND_DEFAULT, nullptr, &g_nvjpeg_handle)
            != NVJPEG_STATUS_SUCCESS) goto fail;
    if (nvjpegJpegStateCreate(g_nvjpeg_handle, &g_nvjpeg_dec_state)
            != NVJPEG_STATUS_SUCCESS) goto fail;
    if (nvjpegEncoderStateCreate(g_nvjpeg_handle, &g_nvjpeg_enc_state, nullptr)
            != NVJPEG_STATUS_SUCCESS) goto fail;
    if (nvjpegEncoderParamsCreate(g_nvjpeg_handle, &g_nvjpeg_enc_params, nullptr)
            != NVJPEG_STATUS_SUCCESS) goto fail;
    if (cudaStreamCreate(&g_cuda_stream) != cudaSuccess) goto fail;

    g_nvjpeg_init_ok = true;
    return true;

fail:
    if (g_nvjpeg_enc_params) { nvjpegEncoderParamsDestroy(g_nvjpeg_enc_params); g_nvjpeg_enc_params = nullptr; }
    if (g_nvjpeg_enc_state)  { nvjpegEncoderStateDestroy(g_nvjpeg_enc_state);   g_nvjpeg_enc_state  = nullptr; }
    if (g_nvjpeg_dec_state)  { nvjpegJpegStateDestroy(g_nvjpeg_dec_state);      g_nvjpeg_dec_state  = nullptr; }
    if (g_nvjpeg_handle)     { nvjpegDestroy(g_nvjpeg_handle);                  g_nvjpeg_handle     = nullptr; }
    if (g_cuda_stream)       { cudaStreamDestroy(g_cuda_stream);                g_cuda_stream       = nullptr; }
    return false;
}

#endif /* CPPDVR_HAS_NVJPEG */

/* ══════════════════════════════════════════════════════════════════════════════
 * Backend management (public API)
 * ══════════════════════════════════════════════════════════════════════════════ */

int jpeg_backend_available(int backend) {
    switch (backend) {
        case JPEG_BACKEND_STB:
            return 1;
        case JPEG_BACKEND_LIBJPEG_TURBO:
#ifdef CPPDVR_HAS_LIBJPEG_TURBO
            return 1;
#else
            return 0;
#endif
        case JPEG_BACKEND_NVJPEG:
#ifdef CPPDVR_HAS_NVJPEG
            {
                std::lock_guard<std::mutex> lk(g_nvjpeg_mu);
                return nvjpeg_ensure_init_locked() ? 1 : 0;
            }
#else
            return 0;
#endif
        default:
            return 0;
    }
}

int jpeg_backend_set(int backend) {
    if (!jpeg_backend_available(backend)) return 0;
    g_backend.store(backend);
    return 1;
}

int jpeg_backend_get(void) {
    return g_backend.load();
}

/* ══════════════════════════════════════════════════════════════════════════════
 * Backend decode implementations
 * ══════════════════════════════════════════════════════════════════════════════ */

/* ── STB decode ──────────────────────────────────────────────────────────── */
static uint8_t* decode_stb(const uint8_t* jpeg_in, size_t in_size,
                             int* out_w, int* out_h) {
    int ch = 0;
    return stbi_load_from_memory(jpeg_in, static_cast<int>(in_size),
                                  out_w, out_h, &ch, 3);
}

/* ── libjpeg-turbo decode ────────────────────────────────────────────────── */
#ifdef CPPDVR_HAS_LIBJPEG_TURBO
static uint8_t* decode_libjpeg_turbo(const uint8_t* jpeg_in, size_t in_size,
                                      int* out_w, int* out_h) {
#ifdef CPPDVR_TJ_V3
    tjhandle tj = tj3Init(TJINIT_DECOMPRESS);
    if (!tj) return nullptr;

    if (tj3DecompressHeader(tj, jpeg_in, in_size) < 0) {
        tj3Destroy(tj); return nullptr;
    }
    int w = tj3Get(tj, TJPARAM_JPEGWIDTH);
    int h = tj3Get(tj, TJPARAM_JPEGHEIGHT);
    if (w <= 0 || h <= 0) { tj3Destroy(tj); return nullptr; }

    uint8_t* rgb = static_cast<uint8_t*>(malloc(static_cast<size_t>(w) * h * 3));
    if (!rgb) { tj3Destroy(tj); return nullptr; }

    if (tj3Decompress8(tj, jpeg_in, in_size, rgb, /*pitch=*/0, TJPF_RGB) < 0) {
        free(rgb); tj3Destroy(tj); return nullptr;
    }
    tj3Destroy(tj);
    *out_w = w; *out_h = h;
    return rgb;
#else /* libjpeg-turbo 2.x API */
    tjhandle tj = tjInitDecompress();
    if (!tj) return nullptr;

    int w = 0, h = 0, subsamp = 0, colorspace = 0;
    if (tjDecompressHeader3(tj,
                             jpeg_in, static_cast<unsigned long>(in_size),
                             &w, &h, &subsamp, &colorspace) < 0) {
        tjDestroy(tj); return nullptr;
    }

    uint8_t* rgb = static_cast<uint8_t*>(malloc(static_cast<size_t>(w) * h * 3));
    if (!rgb) { tjDestroy(tj); return nullptr; }

    if (tjDecompress2(tj,
                      jpeg_in, static_cast<unsigned long>(in_size),
                      rgb, w, /*pitch=*/0, h,
                      TJPF_RGB, TJFLAG_FASTDCT) < 0) {
        free(rgb); tjDestroy(tj); return nullptr;
    }
    tjDestroy(tj);
    *out_w = w; *out_h = h;
    return rgb;
#endif
}
#endif /* CPPDVR_HAS_LIBJPEG_TURBO */

/* ── nvJPEG decode ───────────────────────────────────────────────────────── */
#ifdef CPPDVR_HAS_NVJPEG
static uint8_t* decode_nvjpeg(const uint8_t* jpeg_in, size_t in_size,
                                int* out_w, int* out_h) {
    std::lock_guard<std::mutex> lk(g_nvjpeg_mu);
    if (!nvjpeg_ensure_init_locked()) return nullptr;

    int nComponents = 0;
    nvjpegChromaSubsampling_t subsampling{};
    int widths[NVJPEG_MAX_COMPONENT]  = {};
    int heights[NVJPEG_MAX_COMPONENT] = {};

    if (nvjpegGetImageInfo(g_nvjpeg_handle,
                            jpeg_in, in_size,
                            &nComponents, &subsampling,
                            widths, heights) != NVJPEG_STATUS_SUCCESS)
        return nullptr;

    int w = widths[0], h = heights[0];
    if (w <= 0 || h <= 0) return nullptr;

    uint8_t* d_rgb = nullptr;
    if (cudaMalloc(&d_rgb, static_cast<size_t>(w) * h * 3) != cudaSuccess)
        return nullptr;

    nvjpegImage_t out_img{};
    out_img.channel[0] = d_rgb;
    out_img.pitch[0]   = static_cast<unsigned int>(w * 3);

    nvjpegStatus_t st = nvjpegDecode(g_nvjpeg_handle, g_nvjpeg_dec_state,
                                      jpeg_in, in_size,
                                      NVJPEG_OUTPUT_RGBI,
                                      &out_img, g_cuda_stream);
    cudaStreamSynchronize(g_cuda_stream);

    if (st != NVJPEG_STATUS_SUCCESS) { cudaFree(d_rgb); return nullptr; }

    uint8_t* rgb = static_cast<uint8_t*>(malloc(static_cast<size_t>(w) * h * 3));
    if (!rgb) { cudaFree(d_rgb); return nullptr; }

    cudaMemcpy(rgb, d_rgb, static_cast<size_t>(w) * h * 3, cudaMemcpyDeviceToHost);
    cudaFree(d_rgb);

    *out_w = w; *out_h = h;
    return rgb;
}
#endif /* CPPDVR_HAS_NVJPEG */

/* ══════════════════════════════════════════════════════════════════════════════
 * Backend encode implementations
 * ══════════════════════════════════════════════════════════════════════════════ */

/* ── STB encode ──────────────────────────────────────────────────────────── */
struct StbWriteCtx { std::vector<uint8_t> buf; };
static void stb_write_cb(void* ctx, void* data, int size) {
    auto* c = static_cast<StbWriteCtx*>(ctx);
    c->buf.insert(c->buf.end(),
                  static_cast<uint8_t*>(data),
                  static_cast<uint8_t*>(data) + size);
}

static int encode_stb(const uint8_t* rgb, int w, int h, int quality,
                       uint8_t** out_data, size_t* out_size) {
    StbWriteCtx ctx;
    ctx.buf.reserve(static_cast<size_t>(w) * h / 4);
    if (!stbi_write_jpg_to_func(stb_write_cb, &ctx, w, h, 3, rgb, quality)
            || ctx.buf.empty()) return 0;
    *out_size = ctx.buf.size();
    *out_data = static_cast<uint8_t*>(malloc(*out_size));
    if (!*out_data) return 0;
    memcpy(*out_data, ctx.buf.data(), *out_size);
    return 1;
}

/* ── libjpeg-turbo encode ────────────────────────────────────────────────── */
#ifdef CPPDVR_HAS_LIBJPEG_TURBO
static int encode_libjpeg_turbo(const uint8_t* rgb, int w, int h, int quality,
                                  uint8_t** out_data, size_t* out_size) {
#ifdef CPPDVR_TJ_V3
    tjhandle tj = tj3Init(TJINIT_COMPRESS);
    if (!tj) return 0;

    tj3Set(tj, TJPARAM_SUBSAMP, TJSAMP_420);
    tj3Set(tj, TJPARAM_QUALITY, quality);

    unsigned char* tj_buf  = nullptr;
    size_t         tj_size = 0;

    /* tj3Compress8 accepts const source — no cast needed. */
    if (tj3Compress8(tj, rgb, w, /*pitch=*/0, h, TJPF_RGB,
                     &tj_buf, &tj_size) < 0 || !tj_buf) {
        tj3Destroy(tj); return 0;
    }
    tj3Destroy(tj);

    *out_data = static_cast<uint8_t*>(malloc(tj_size));
    if (!*out_data) { tj3Free(tj_buf); return 0; }
    memcpy(*out_data, tj_buf, tj_size);
    *out_size = tj_size;
    tj3Free(tj_buf);
    return 1;
#else /* libjpeg-turbo 2.x API */
    tjhandle tj = tjInitCompress();
    if (!tj) return 0;

    unsigned char* tj_buf  = nullptr;
    unsigned long  tj_size = 0;

    /* tjCompress2 expects non-const src but does not modify it. */
    if (tjCompress2(tj,
                    const_cast<unsigned char*>(rgb),
                    w, /*pitch=*/0, h, TJPF_RGB,
                    &tj_buf, &tj_size,
                    TJSAMP_420, quality, TJFLAG_FASTDCT) < 0) {
        tjDestroy(tj); return 0;
    }
    tjDestroy(tj);

    *out_data = static_cast<uint8_t*>(malloc(tj_size));
    if (!*out_data) { tjFree(tj_buf); return 0; }
    memcpy(*out_data, tj_buf, tj_size);
    *out_size = static_cast<size_t>(tj_size);
    tjFree(tj_buf);
    return 1;
#endif
}
#endif /* CPPDVR_HAS_LIBJPEG_TURBO */

/* ── nvJPEG encode ───────────────────────────────────────────────────────── */
#ifdef CPPDVR_HAS_NVJPEG
static int encode_nvjpeg(const uint8_t* rgb, int w, int h, int quality,
                          uint8_t** out_data, size_t* out_size) {
    std::lock_guard<std::mutex> lk(g_nvjpeg_mu);
    if (!nvjpeg_ensure_init_locked()) return 0;

    size_t   rgb_bytes = static_cast<size_t>(w) * h * 3;
    uint8_t* d_rgb     = nullptr;
    if (cudaMalloc(&d_rgb, rgb_bytes) != cudaSuccess) return 0;
    cudaMemcpy(d_rgb, rgb, rgb_bytes, cudaMemcpyHostToDevice);

    nvjpegImage_t input{};
    input.channel[0] = d_rgb;
    input.pitch[0]   = static_cast<unsigned int>(w * 3);

    nvjpegEncoderParamsSetQuality(g_nvjpeg_enc_params, quality, g_cuda_stream);
    nvjpegEncoderParamsSetSamplingFactors(g_nvjpeg_enc_params,
                                           NVJPEG_CSS_420, g_cuda_stream);

    nvjpegStatus_t st = nvjpegEncodeImage(
        g_nvjpeg_handle, g_nvjpeg_enc_state, g_nvjpeg_enc_params,
        &input, NVJPEG_INPUT_RGBI, w, h, g_cuda_stream);
    cudaStreamSynchronize(g_cuda_stream);
    cudaFree(d_rgb);

    if (st != NVJPEG_STATUS_SUCCESS) return 0;

    /* Retrieve encoded size, then data */
    size_t length = 0;
    nvjpegEncodeRetrieveBitstream(g_nvjpeg_handle, g_nvjpeg_enc_state,
                                   nullptr, &length, g_cuda_stream);
    cudaStreamSynchronize(g_cuda_stream);
    if (length == 0) return 0;

    *out_data = static_cast<uint8_t*>(malloc(length));
    if (!*out_data) return 0;

    nvjpegEncodeRetrieveBitstream(g_nvjpeg_handle, g_nvjpeg_enc_state,
                                   *out_data, &length, g_cuda_stream);
    cudaStreamSynchronize(g_cuda_stream);
    *out_size = length;
    return 1;
}
#endif /* CPPDVR_HAS_NVJPEG */

/* ══════════════════════════════════════════════════════════════════════════════
 * Public decode / encode — dispatch to active backend
 * ══════════════════════════════════════════════════════════════════════════════ */

uint8_t* jpeg_decode_rgb(const uint8_t* jpeg_in, size_t in_size,
                          int* out_width, int* out_height) {
    if (!jpeg_in || !in_size || !out_width || !out_height) return nullptr;
    switch (g_backend.load()) {
#ifdef CPPDVR_HAS_LIBJPEG_TURBO
        case JPEG_BACKEND_LIBJPEG_TURBO:
            return decode_libjpeg_turbo(jpeg_in, in_size, out_width, out_height);
#endif
#ifdef CPPDVR_HAS_NVJPEG
        case JPEG_BACKEND_NVJPEG:
            return decode_nvjpeg(jpeg_in, in_size, out_width, out_height);
#endif
        default:
            return decode_stb(jpeg_in, in_size, out_width, out_height);
    }
}

int jpeg_encode_rgb(const uint8_t* rgb, int width, int height, int quality,
                    uint8_t** out_data, size_t* out_size) {
    if (!rgb || !out_data || !out_size || width <= 0 || height <= 0) return 0;
    switch (g_backend.load()) {
#ifdef CPPDVR_HAS_LIBJPEG_TURBO
        case JPEG_BACKEND_LIBJPEG_TURBO:
            return encode_libjpeg_turbo(rgb, width, height, quality, out_data, out_size);
#endif
#ifdef CPPDVR_HAS_NVJPEG
        case JPEG_BACKEND_NVJPEG:
            return encode_nvjpeg(rgb, width, height, quality, out_data, out_size);
#endif
        default:
            return encode_stb(rgb, width, height, quality, out_data, out_size);
    }
}

/* ══════════════════════════════════════════════════════════════════════════════
 * Embedded 8×8 bitmap font (ASCII 0x20–0x7E, public domain)
 * Each character: 8 bytes (one per row).  Bit 0 = leftmost pixel.
 * ══════════════════════════════════════════════════════════════════════════════ */
static const uint8_t s_font[95][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 0x20   */
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, /* 0x21 ! */
    {0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00}, /* 0x22 " */
    {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00}, /* 0x23 # */
    {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00}, /* 0x24 $ */
    {0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00}, /* 0x25 % */
    {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00}, /* 0x26 & */
    {0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00}, /* 0x27 ' */
    {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00}, /* 0x28 ( */
    {0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00}, /* 0x29 ) */
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, /* 0x2A * */
    {0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00}, /* 0x2B + */
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06}, /* 0x2C , */
    {0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00}, /* 0x2D - */
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00}, /* 0x2E . */
    {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00}, /* 0x2F / */
    {0x1E,0x33,0x3B,0x37,0x33,0x33,0x1E,0x00}, /* 0x30 0 */
    {0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00}, /* 0x31 1 */
    {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00}, /* 0x32 2 */
    {0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00}, /* 0x33 3 */
    {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00}, /* 0x34 4 */
    {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00}, /* 0x35 5 */
    {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00}, /* 0x36 6 */
    {0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00}, /* 0x37 7 */
    {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00}, /* 0x38 8 */
    {0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00}, /* 0x39 9 */
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00}, /* 0x3A : */
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06}, /* 0x3B ; */
    {0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00}, /* 0x3C < */
    {0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00}, /* 0x3D = */
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, /* 0x3E > */
    {0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00}, /* 0x3F ? */
    {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00}, /* 0x40 @ */
    {0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00}, /* 0x41 A */
    {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00}, /* 0x42 B */
    {0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00}, /* 0x43 C */
    {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00}, /* 0x44 D */
    {0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0x00}, /* 0x45 E */
    {0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0x00}, /* 0x46 F */
    {0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00}, /* 0x47 G */
    {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00}, /* 0x48 H */
    {0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, /* 0x49 I */
    {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00}, /* 0x4A J */
    {0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00}, /* 0x4B K */
    {0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0x00}, /* 0x4C L */
    {0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00}, /* 0x4D M */
    {0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00}, /* 0x4E N */
    {0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00}, /* 0x4F O */
    {0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00}, /* 0x50 P */
    {0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00}, /* 0x51 Q */
    {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00}, /* 0x52 R */
    {0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00}, /* 0x53 S */
    {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, /* 0x54 T */
    {0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00}, /* 0x55 U */
    {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00}, /* 0x56 V */
    {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00}, /* 0x57 W */
    {0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00}, /* 0x58 X */
    {0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00}, /* 0x59 Y */
    {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00}, /* 0x5A Z */
    {0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00}, /* 0x5B [ */
    {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00}, /* 0x5C \ */
    {0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00}, /* 0x5D ] */
    {0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00}, /* 0x5E ^ */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, /* 0x5F _ */
    {0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00}, /* 0x60 ` */
    {0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00}, /* 0x61 a */
    {0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0x00}, /* 0x62 b */
    {0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x00}, /* 0x63 c */
    {0x38,0x30,0x30,0x3E,0x33,0x33,0x6E,0x00}, /* 0x64 d */
    {0x00,0x00,0x1E,0x33,0x3F,0x03,0x1E,0x00}, /* 0x65 e */
    {0x1C,0x36,0x06,0x0F,0x06,0x06,0x0F,0x00}, /* 0x66 f */
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F}, /* 0x67 g */
    {0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0x00}, /* 0x68 h */
    {0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00}, /* 0x69 i */
    {0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1E}, /* 0x6A j */
    {0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0x00}, /* 0x6B k */
    {0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, /* 0x6C l */
    {0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00}, /* 0x6D m */
    {0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00}, /* 0x6E n */
    {0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00}, /* 0x6F o */
    {0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F}, /* 0x70 p */
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78}, /* 0x71 q */
    {0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00}, /* 0x72 r */
    {0x00,0x00,0x3E,0x03,0x1E,0x30,0x1F,0x00}, /* 0x73 s */
    {0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0x00}, /* 0x74 t */
    {0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00}, /* 0x75 u */
    {0x00,0x00,0x33,0x33,0x33,0x1E,0x0C,0x00}, /* 0x76 v */
    {0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00}, /* 0x77 w */
    {0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00}, /* 0x78 x */
    {0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F}, /* 0x79 y */
    {0x00,0x00,0x3F,0x19,0x0C,0x26,0x3F,0x00}, /* 0x7A z */
    {0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00}, /* 0x7B { */
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, /* 0x7C | */
    {0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00}, /* 0x7D } */
    {0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00}, /* 0x7E ~ */
};

/* ── Glyph renderer ──────────────────────────────────────────────────────── */
static void draw_glyphs(uint8_t* rgb, int img_w, int img_h,
                         const char* text, int x0, int y0,
                         uint8_t r, uint8_t g, uint8_t b, int scale)
{
    if (scale < 1) scale = 1;
    int cx = x0, cy = y0;
    for (const unsigned char* p = (const unsigned char*)text; *p; ++p) {
        unsigned char c = *p;
        if (c == '\n') { cx = x0; cy += 8 * scale; continue; }
        if (c < 0x20 || c > 0x7E) c = '?';
        const uint8_t* glyph = s_font[c - 0x20];
        for (int row = 0; row < 8; ++row) {
            uint8_t bits = glyph[row];
            for (int col = 0; col < 8; ++col) {
                if (!(bits & (1 << col))) continue;
                for (int dy = 0; dy < scale; ++dy) {
                    for (int dx = 0; dx < scale; ++dx) {
                        int px = cx + col * scale + dx;
                        int py = cy + row * scale + dy;
                        if (px < 0 || px >= img_w || py < 0 || py >= img_h) continue;
                        uint8_t* pix = rgb + (py * img_w + px) * 3;
                        pix[0] = r; pix[1] = g; pix[2] = b;
                    }
                }
            }
        }
        cx += 8 * scale;
    }
}

/* ── Word-wrap helper ────────────────────────────────────────────────────── */
#define OVL_MAX_LINES    64
#define OVL_MAX_LINE_LEN 256

static int build_lines(const char* text, int max_chars,
                        char  out[OVL_MAX_LINES][OVL_MAX_LINE_LEN],
                        int   out_lens[OVL_MAX_LINES])
{
    int nlines = 0;
    char cur[OVL_MAX_LINE_LEN]; cur[0] = '\0';
    int  curlen = 0;

    const char* p = text;
    while (*p && nlines < OVL_MAX_LINES) {

        if (*p == '\n') {
            cur[curlen] = '\0';
            memcpy(out[nlines], cur, (size_t)curlen + 1);
            out_lens[nlines++] = curlen;
            curlen = 0; cur[0] = '\0';
            ++p; continue;
        }

        int wlen = 0;
        for (const char* q = p; *q && *q != ' ' && *q != '\n'; ++q) ++wlen;

        if (max_chars > 0 && wlen > max_chars) {
            for (int i = 0; i < wlen && nlines < OVL_MAX_LINES; ) {
                if (curlen >= max_chars) {
                    cur[curlen] = '\0';
                    memcpy(out[nlines], cur, (size_t)curlen + 1);
                    out_lens[nlines++] = curlen;
                    curlen = 0; cur[0] = '\0';
                }
                int room = max_chars - curlen;
                int copy = (wlen - i < room) ? (wlen - i) : room;
                memcpy(cur + curlen, p + i, (size_t)copy);
                curlen += copy; i += copy; cur[curlen] = '\0';
            }
            p += wlen;
        } else {
            int need = (curlen > 0) ? 1 + wlen : wlen;
            if (max_chars > 0 && curlen > 0 && curlen + need > max_chars) {
                cur[curlen] = '\0';
                memcpy(out[nlines], cur, (size_t)curlen + 1);
                out_lens[nlines++] = curlen;
                curlen = 0; cur[0] = '\0';
            }
            if (curlen > 0 && curlen < OVL_MAX_LINE_LEN - 1)
                cur[curlen++] = ' ';
            int copy = (wlen < OVL_MAX_LINE_LEN - 1 - curlen)
                        ? wlen : (OVL_MAX_LINE_LEN - 1 - curlen);
            memcpy(cur + curlen, p, (size_t)copy);
            curlen += copy; cur[curlen] = '\0';
            p += wlen;
        }

        if (*p == ' ') ++p;
    }

    if (curlen > 0 && nlines < OVL_MAX_LINES) {
        cur[curlen] = '\0';
        memcpy(out[nlines], cur, (size_t)curlen + 1);
        out_lens[nlines++] = curlen;
    }
    return nlines;
}

/* ══════════════════════════════════════════════════════════════════════════════
 * Public text-draw API
 * ══════════════════════════════════════════════════════════════════════════════ */

void jpeg_overlay_draw_text(uint8_t* rgb, int width, int height,
                             const char* text, int x, int y,
                             uint8_t r, uint8_t g, uint8_t b,
                             int scale)
{
    if (!rgb || !text || !*text) return;
    if (scale < 1) scale = 1;
    draw_glyphs(rgb, width, height, text, x + scale, y + scale, 0, 0, 0, scale);
    draw_glyphs(rgb, width, height, text, x,          y,          r, g, b, scale);
}

void jpeg_overlay_draw_textbox(uint8_t* rgb, int img_w, int img_h,
                                const char* text,
                                int x, int y, int box_w,
                                uint8_t r, uint8_t g, uint8_t b,
                                int scale, int align, int anchor)
{
    if (!rgb || !text || !*text) return;
    if (scale < 1) scale = 1;

    int max_chars = (box_w > 0) ? (box_w / (8 * scale)) : 0;
    if (max_chars < 1 && box_w > 0) max_chars = 1;

    char lines[OVL_MAX_LINES][OVL_MAX_LINE_LEN];
    int  lens [OVL_MAX_LINES];
    int  nlines = build_lines(text, max_chars, lines, lens);

    int block_h = nlines * 8 * scale;
    int ax, ay;
    switch (anchor) {
        case OVERLAY_ANCHOR_TOP_RIGHT:
            ax = img_w - (box_w > 0 ? box_w : 0) - x; ay = y; break;
        case OVERLAY_ANCHOR_BOTTOM_LEFT:
            ax = x; ay = img_h - block_h - y; break;
        case OVERLAY_ANCHOR_BOTTOM_RIGHT:
            ax = img_w - (box_w > 0 ? box_w : 0) - x;
            ay = img_h - block_h - y; break;
        default: /* OVERLAY_ANCHOR_TOP_LEFT */
            ax = x; ay = y; break;
    }

    for (int i = 0; i < nlines; ++i) {
        int lx;
        if (align == OVERLAY_ALIGN_RIGHT && box_w > 0) {
            lx = ax + box_w - lens[i] * 8 * scale;
            if (lx < ax) lx = ax;
        } else {
            lx = ax;
        }
        int ly = ay + i * 8 * scale;
        draw_glyphs(rgb, img_w, img_h, lines[i], lx + scale, ly + scale, 0, 0, 0, scale);
        draw_glyphs(rgb, img_w, img_h, lines[i], lx,          ly,          r, g, b, scale);
    }
}

int jpeg_overlay_text(const uint8_t* jpeg_in, size_t in_size,
                      const char* text, int x, int y,
                      uint8_t r, uint8_t g, uint8_t b,
                      int quality,
                      uint8_t** out_data, size_t* out_size)
{
    if (!jpeg_in || !in_size || !out_data || !out_size) return 0;
    int w = 0, h = 0;
    uint8_t* rgb = jpeg_decode_rgb(jpeg_in, in_size, &w, &h);
    if (!rgb) return 0;
    if (text && *text)
        jpeg_overlay_draw_text(rgb, w, h, text, x, y, r, g, b, 1);
    int result = jpeg_encode_rgb(rgb, w, h, quality, out_data, out_size);
    free(rgb);
    return result;
}
