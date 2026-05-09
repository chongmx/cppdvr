/**
 * tests/osd/test_jpeg_markers.c
 *
 * Probes JPEG frames from the camera for restart markers.
 * Grabs two frame sources and inspects both:
 *
 *   1. Snapshot JPEG  — pulled directly from the camera via dvr_snapshot()
 *   2. Stream JPEG    — decoded by the ffmpeg pipeline inside StreamServer
 *                       (i.e. the frame type our overlay will actually touch)
 *
 * For each frame, every JPEG segment is logged, then the entropy-coded
 * scan data is walked byte-by-byte looking for:
 *   - FF DD  DRI  (Define Restart Interval) — declares the MCU interval
 *   - FF D0..D7  RST0..RST7 — actual restart markers embedded in scan data
 *
 * Restart markers let us splice MCU rows independently.
 * Their absence means we still can splice, but need a DC-accumulation
 * pre-pass through the bitstream before the ROI (cheap; no pixel decode).
 *
 * Usage:
 *   test_jpeg_markers [host [user [password]]]
 */

#include "cppdvr_api.h"
#include "test_helpers.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ── JPEG helpers ───────────────────────────────────────────────────────────── */

static uint16_t u16be(const uint8_t* p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

static const char* marker_name(uint8_t m) {
    switch (m) {
    case 0xC0: return "SOF0 (baseline DCT)";
    case 0xC1: return "SOF1 (extended DCT)";
    case 0xC2: return "SOF2 (progressive DCT)";
    case 0xC4: return "DHT  (Huffman table)";
    case 0xD8: return "SOI";
    case 0xD9: return "EOI";
    case 0xDA: return "SOS  (start of scan)";
    case 0xDB: return "DQT  (quantization table)";
    case 0xDC: return "DNL";
    case 0xDD: return "DRI  (restart interval)";
    case 0xFE: return "COM  (comment)";
    default:
        if (m >= 0xE0 && m <= 0xEF) return "APPn";
        if (m >= 0xD0 && m <= 0xD7) return "RST";
        return "???";
    }
}

/* Scan one JPEG buffer and log everything. */
static void scan_jpeg(const uint8_t* data, size_t len, const char* label) {
    printf("\n");
    SECTION(label);

    if (len < 4 || data[0] != 0xFF || data[1] != 0xD8) {
        FAIL("Not a valid JPEG — SOI (FF D8) missing at byte 0");
        return;
    }

    int      width         = 0;
    int      height        = 0;
    int      components    = 0;
    int      dri_interval  = 0;   /* 0 = DRI segment absent */
    int      rst_count     = 0;
    size_t   scan_start    = 0;   /* byte offset where entropy data begins */
    size_t   prev_rst_off  = 0;
    size_t   first_rst_off = 0;

    INFO("  off    marker  name");
    INFO("  -----  ------  -----");
    INFO("  %5u  FF D8   SOI", 0);

    size_t i = 2;
    while (i + 1 < len) {

        /* ── inside entropy-coded data: look only for RST / EOI ────────────── */
        if (scan_start) {
            if (data[i] != 0xFF) { i++; continue; }
            uint8_t next = data[i + 1];

            if (next == 0x00) { i += 2; continue; }  /* byte-stuffed FF — skip */

            if (next >= 0xD0 && next <= 0xD7) {
                /* RST marker */
                size_t dist = rst_count ? (i - prev_rst_off)
                                        : (i - scan_start);
                INFO("  %5zu  FF D%u   RST%u   (+%zu bytes from %s)",
                     i, next - 0xD0, next - 0xD0, dist,
                     rst_count ? "prev RST" : "SOS data start");
                if (rst_count == 0) first_rst_off = i;
                prev_rst_off = i;
                rst_count++;
                i += 2;
                continue;
            }

            if (next == 0xD9) {
                INFO("  %5zu  FF D9   EOI", i);
                break;
            }

            /* Any other marker inside scan data is unusual but harmless */
            INFO("  %5zu  FF %02X   (in scan — unusual)", i, next);
            i += 2;
            continue;
        }

        /* ── outside scan data: parse segment headers ───────────────────────── */
        if (data[i] != 0xFF) { i++; continue; }
        uint8_t marker = data[i + 1];

        if (marker == 0xD9) {
            INFO("  %5zu  FF D9   EOI", i);
            break;
        }

        /* Standalone markers (no length field) */
        if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7)) {
            INFO("  %5zu  FF %02X   %s", i, marker, marker_name(marker));
            i += 2;
            continue;
        }

        /* Segments: 2-byte length field follows (includes itself, excludes FF XX) */
        if (i + 3 >= len) break;
        uint16_t seg_len = u16be(data + i + 2);

        switch (marker) {

        case 0xDB:   /* DQT */
            INFO("  %5zu  FF DB   DQT   len=%u", i, seg_len);
            break;

        case 0xC0:   /* SOF0 baseline */
        case 0xC1:   /* SOF1 */
        case 0xC2: { /* SOF2 progressive */
            if (i + 9 < len) {
                /* precision=data[i+4], height at [5..6], width at [7..8], Nf at [9] */
                height     = u16be(data + i + 5);
                width      = u16be(data + i + 7);
                components = data[i + 9];
            }
            INFO("  %5zu  FF %02X   %s  %d × %d  Nf=%d",
                 i, marker, marker_name(marker), width, height, components);

            /* Print MCU grid now that we know dimensions */
            {
                int mcu_cols = (width  + 7) / 8;
                int mcu_rows = (height + 7) / 8;
                INFO("         MCU grid (8×8 baseline): %d cols × %d rows = %d blocks",
                     mcu_cols, mcu_rows, mcu_cols * mcu_rows);
                if (dri_interval > 0) {
                    double rows_per_interval = (double)dri_interval / mcu_cols;
                    INFO("         DRI=%d MCUs => restart every %.1f MCU rows (%.0f px)",
                         dri_interval, rows_per_interval,
                         rows_per_interval * 8.0);
                }
            }
            break;
        }

        case 0xC4:   /* DHT */
            INFO("  %5zu  FF C4   DHT   len=%u", i, seg_len);
            break;

        case 0xDD: { /* DRI */
            if (i + 5 < len)
                dri_interval = u16be(data + i + 4);
            INFO("  %5zu  FF DD   DRI   restart_interval = %d MCUs", i, dri_interval);
            if (dri_interval > 0 && width > 0) {
                int mcu_cols = (width + 7) / 8;
                double rows_per_interval = (double)dri_interval / mcu_cols;
                INFO("         => restart every %.1f MCU rows (%.0f pixel rows)",
                     rows_per_interval, rows_per_interval * 8.0);
            }
            break;
        }

        case 0xDA: { /* SOS — scan header */
            INFO("  %5zu  FF DA   SOS   header_len=%u  (entropy data follows)",
                 i, seg_len);
            /* Entropy data starts immediately after the scan header */
            scan_start = i + 2 + seg_len;
            i += 2 + seg_len;
            continue;
        }

        default:
            if (marker >= 0xE0 && marker <= 0xEF)
                INFO("  %5zu  FF %02X   APP%-2d len=%u",
                     i, marker, marker - 0xE0, seg_len);
            else
                INFO("  %5zu  FF %02X   %s  len=%u",
                     i, marker, marker_name(marker), seg_len);
            break;
        }

        i += 2 + seg_len;
    }

    /* ── Summary ────────────────────────────────────────────────────────────── */
    printf("\n");
    INFO("── Summary: %s ──", label);

    if (width && height)
        INFO("Image size         : %d × %d  (%d components)", width, height, components);
    else
        FAIL("Could not determine image dimensions (no SOF segment found)");

    if (width && height) {
        int mcu_cols = (width  + 7) / 8;
        int mcu_rows = (height + 7) / 8;
        INFO("MCU grid (8×8)     : %d × %d = %d blocks", mcu_cols, mcu_rows, mcu_cols * mcu_rows);
    }

    /* DRI */
    if (dri_interval > 0) {
        PASSF("DRI segment        : PRESENT  interval=%d MCUs", dri_interval);
        if (width > 0) {
            int mcu_cols = (width + 7) / 8;
            double rows  = (double)dri_interval / mcu_cols;
            INFO("                     restart every %.1f MCU rows (%.0f pixel rows)",
                 rows, rows * 8.0);
        }
    } else {
        INFO("DRI segment        : ABSENT (no restart interval declared)");
    }

    /* RST markers */
    if (rst_count > 0) {
        PASSF("RST markers in scan: FOUND  count=%d", rst_count);
        if (width > 0 && dri_interval > 0) {
            int mcu_cols = (width + 7) / 8;
            int mcu_rows = (height + 7) / 8;
            int expected = (mcu_cols * mcu_rows) / dri_interval;
            INFO("                     expected ~%d for this frame size", expected);
        }
    } else if (scan_start > 0) {
        INFO("RST markers in scan: ABSENT (scan data was present but no RST found)");
    } else {
        INFO("RST markers        : UNKNOWN (no SOS found — scan not reached)");
    }

    /* Verdict */
    printf("\n");
    if (rst_count > 0 && dri_interval > 0) {
        PASS("VERDICT: Restart markers confirmed ✓");
        PASS("  MCU-row splice is independent — no DC accumulation pre-pass needed");
    } else if (dri_interval > 0 && rst_count == 0) {
        FAIL("VERDICT: DRI declared but no RST found — bitstream may be malformed");
    } else {
        INFO("VERDICT: No restart markers in this frame");
        INFO("  MCU-row splice still possible but requires DC accumulation pre-pass");
        INFO("  (parse entropy bits up to ROI start, track DC predictor, then splice)");
    }
}

/* ── Stream JPEG capture ─────────────────────────────────────────────────────── */

/* Minimal shared state for one-shot JPEG capture from stream callback */
static struct {
    uint8_t* data;
    size_t   size;
    volatile int ready;
} g_capture = { NULL, 0, 0 };

static void on_jpeg(const uint8_t* data, size_t size, uint32_t frame_id, void* ud) {
    (void)ud; (void)frame_id;
    if (g_capture.ready) return;   /* only capture once */
    g_capture.data = (uint8_t*)malloc(size);
    if (g_capture.data) {
        memcpy(g_capture.data, data, size);
        g_capture.size = size;
    }
    g_capture.ready = 1;
}

/* ── main ───────────────────────────────────────────────────────────────────── */

int main(int argc, char* argv[]) {
    const char* host = (argc >= 2) ? argv[1] : dvr_env("DVR_HOST", DVR_DEFAULT_HOST);
    const char* user = (argc >= 3) ? argv[2] : dvr_env("DVR_USER", DVR_DEFAULT_USER);
    const char* pass = (argc >= 4) ? argv[3] : dvr_env("DVR_PASS", DVR_DEFAULT_PASS);

    printf("=== JPEG Restart Marker Probe ===\n");
    INFO("Host: %s  User: %s", host, user);

    /* ── Login ──────────────────────────────────────────────────────────────── */
    DVRHandle h = dvr_create(host, DVR_DEFAULT_PORT, user, pass);
    if (!h || !dvr_login(h)) {
        FAIL("Login failed");
        dvr_destroy(h);
        EXIT_SUMMARY();
    }
    PASS("Login OK");

    /* ════════════════════════════════════════════════════════════════════════
     * Frame source 1: Snapshot JPEG (direct from camera HTTP endpoint)
     * ════════════════════════════════════════════════════════════════════════ */
    SECTION("Frame source 1 — Snapshot (camera HTTP snapshot endpoint)");
    INFO("Calling dvr_snapshot() ...");

    size_t   snap_len  = 0;
    uint8_t* snap_data = (uint8_t*)dvr_snapshot(h, &snap_len, 0);

    if (!snap_data || snap_len < 4) {
        FAIL("dvr_snapshot() returned no data");
    } else {
        PASSF("Snapshot received: %zu bytes (%.1f KB)", snap_len, snap_len / 1024.0);

        /* Save to disk */
        FILE* f = fopen("probe_snapshot.jpg", "wb");
        if (f) { fwrite(snap_data, 1, snap_len, f); fclose(f); }
        PASS("Saved: probe_snapshot.jpg");

        scan_jpeg(snap_data, snap_len, "SNAPSHOT (camera)");
        dvr_free_buffer(snap_data);
    }

    /* ════════════════════════════════════════════════════════════════════════
     * Frame source 2: Stream JPEG (ffmpeg-decoded from H.265 IP stream)
     * This is the frame type our overlay processor will receive.
     * ════════════════════════════════════════════════════════════════════════ */
    SECTION("Frame source 2 — Stream JPEG (H.265 -> ffmpeg -> JPEG)");
    INFO("Starting StreamServer, waiting for first decoded JPEG ...");

    StreamHandle sh = stream_create(host, DVR_DEFAULT_PORT, user, pass, 0 /* no HTTP */);
    if (!sh) {
        FAIL("stream_create() returned NULL");
        goto done;
    }

    stream_set_jpeg_callback(sh, on_jpeg, NULL);

    if (!stream_start(sh, "Main")) {
        FAIL("stream_start() failed");
        stream_destroy(sh);
        goto done;
    }

    /* Wait up to 15 s for the first JPEG */
    {
        int waited = 0;
        while (!g_capture.ready && waited < 150) {
            sleep_ms(100);
            waited++;
        }
    }

    stream_stop(sh);
    stream_destroy(sh);

    if (!g_capture.ready || !g_capture.data) {
        FAIL("No JPEG received from stream within 15 s");
        goto done;
    }

    PASSF("Stream JPEG received: %zu bytes (%.1f KB)",
          g_capture.size, g_capture.size / 1024.0);

    /* Save to disk */
    {
        FILE* f = fopen("probe_stream_frame.jpg", "wb");
        if (f) { fwrite(g_capture.data, 1, g_capture.size, f); fclose(f); }
        PASS("Saved: probe_stream_frame.jpg");
    }

    scan_jpeg(g_capture.data, g_capture.size, "STREAM JPEG (ffmpeg output)");
    free(g_capture.data);

done:
    dvr_destroy(h);
    EXIT_SUMMARY();
}
