/**
 * udp_stream_server.cpp — XrRobotOperator bidirectional UDP transceiver
 *
 * Implements the XRRO v1 wire protocol (byte-for-byte compatible with
 * py_sample_2/udp_transciever.py).
 *
 * One UDP socket is bound to <local_ip>:<rx_port> for receiving.
 * The same socket is used for sendto() to <target_ip>:<tx_port>.
 *
 * Inbound packets parsed:
 *   0x01  InputAndGui   → UdpOnInputFn callback
 *   0x02  JpegChunk     → reassembled, then UdpOnJpegFn callback
 *   0x05  Composite     → UdpOnInputFn callback (input section)
 *                         + UdpOnJpegFn callback (if JPEG present)
 *
 * Outbound packets sent:
 *   0x02  JpegChunk     sendJpeg()
 *   0x03  Command       sendCommand()
 *   0x04  GuiUpdate     sendGuiUpdate()
 *   0x05  Composite     sendComposite()
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

#include "udp_stream_server.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cppdvr {

// ════════════════════════════════════════════════════════════════════════════════
// Endian / wire helpers  (x86/x64 Windows is natively little-endian)
// ════════════════════════════════════════════════════════════════════════════════

static inline uint16_t read_u16le(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}
static inline uint32_t read_u32le(const uint8_t* p) {
    return  static_cast<uint32_t>(p[0])
        | ( static_cast<uint32_t>(p[1]) << 8)
        | ( static_cast<uint32_t>(p[2]) << 16)
        | ( static_cast<uint32_t>(p[3]) << 24);
}
static inline uint64_t read_u64le(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}
static inline float read_f32le(const uint8_t* p) {
    float f; std::memcpy(&f, p, 4);   // safe for unaligned reads on x86
    return f;
}

static inline void write_u16le(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}
static inline void write_u32le(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >>  8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}
static inline void write_u64le(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i, v >>= 8) p[i] = static_cast<uint8_t>(v);
}
static inline void write_f32le(uint8_t* p, float f) {
    std::memcpy(p, &f, 4);
}

static uint64_t timestamp_us_now() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

// ════════════════════════════════════════════════════════════════════════════════
// Controller parsing — mirrors Python FMT_CTRL = '<4BfBfBff5BB3f4f3f4f'
//
// Wire layout (84 bytes, tightly packed, all little-endian):
//   offset  size  field
//      0      1   primary_button
//      1      1   secondary_button
//      2      1   menu_button
//      3      1   thumbstick_click
//      4      4   trigger_value  (float32)
//      8      1   trigger_click
//      9      4   grip_value     (float32, unaligned!)
//     13      1   grip_click
//     14      4   thumbstick_x   (float32, unaligned!)
//     18      4   thumbstick_y   (float32)
//     22      5   primary/secondary/thumbstick/trigger/thumbrest touch (uint8 each)
//     27      1   active
//     28     12   aim_pos[3]
//     40     16   aim_ori[4]
//     56     12   grip_pos[3]
//     68     16   grip_ori[4]
// ════════════════════════════════════════════════════════════════════════════════

static size_t parse_controller(const uint8_t* data, size_t off,
                                UdpControllerState& c)
{
    c.primary_button    = data[off++];
    c.secondary_button  = data[off++];
    c.menu_button       = data[off++];
    c.thumbstick_click  = data[off++];
    c.trigger_value     = read_f32le(data + off); off += 4;
    c.trigger_click     = data[off++];
    c.grip_value        = read_f32le(data + off); off += 4;
    c.grip_click        = data[off++];
    c.thumbstick_x      = read_f32le(data + off); off += 4;
    c.thumbstick_y      = read_f32le(data + off); off += 4;
    c.primary_touch     = data[off++];
    c.secondary_touch   = data[off++];
    c.thumbstick_touch  = data[off++];
    c.trigger_touch     = data[off++];
    c.thumbrest_touch   = data[off++];
    c.active            = data[off++];
    // aim pose
    for (int i = 0; i < 3; ++i) { c.aim_pos[i] = read_f32le(data + off); off += 4; }
    for (int i = 0; i < 4; ++i) { c.aim_ori[i] = read_f32le(data + off); off += 4; }
    // grip pose
    for (int i = 0; i < 3; ++i) { c.grip_pos[i] = read_f32le(data + off); off += 4; }
    for (int i = 0; i < 4; ++i) { c.grip_ori[i] = read_f32le(data + off); off += 4; }
    return off;   // consumed exactly kUdpCtrlSize (84) bytes
}

// Serialise a (zeroed) controller state into 84 wire bytes
static size_t write_controller(uint8_t* data, size_t off,
                                const UdpControllerState& c = {})
{
    data[off++] = c.primary_button;
    data[off++] = c.secondary_button;
    data[off++] = c.menu_button;
    data[off++] = c.thumbstick_click;
    write_f32le(data + off, c.trigger_value);  off += 4;
    data[off++] = c.trigger_click;
    write_f32le(data + off, c.grip_value);     off += 4;
    data[off++] = c.grip_click;
    write_f32le(data + off, c.thumbstick_x);   off += 4;
    write_f32le(data + off, c.thumbstick_y);   off += 4;
    data[off++] = c.primary_touch;
    data[off++] = c.secondary_touch;
    data[off++] = c.thumbstick_touch;
    data[off++] = c.trigger_touch;
    data[off++] = c.thumbrest_touch;
    data[off++] = c.active;
    for (int i = 0; i < 3; ++i) { write_f32le(data + off, c.aim_pos[i]);  off += 4; }
    for (int i = 0; i < 4; ++i) { write_f32le(data + off, c.aim_ori[i]);  off += 4; }
    for (int i = 0; i < 3; ++i) { write_f32le(data + off, c.grip_pos[i]); off += 4; }
    for (int i = 0; i < 4; ++i) { write_f32le(data + off, c.grip_ori[i]); off += 4; }
    return off;
}

// ════════════════════════════════════════════════════════════════════════════════
// Inbound JPEG reassembler
// ════════════════════════════════════════════════════════════════════════════════

struct JpegReassembler {
    uint32_t                                 frame_id    = 0;
    uint16_t                                 total       = 0;
    std::map<uint16_t, std::vector<uint8_t>> chunks;

    void reset() { chunks.clear(); total = 0; }

    // Returns a complete JPEG (empty if not yet assembled).
    std::vector<uint8_t> feed(uint32_t fid, uint16_t idx, uint16_t ntotal,
                               const uint8_t* payload, size_t payload_size) {
        if (chunks.empty() || frame_id != fid) {
            reset();
            frame_id = fid;
            total    = ntotal;
        }
        chunks[idx].assign(payload, payload + payload_size);

        if (static_cast<uint16_t>(chunks.size()) != total) return {};

        // All chunks present — assemble in order
        std::vector<uint8_t> frame;
        for (uint16_t i = 0; i < total; ++i) {
            auto it = chunks.find(i);
            if (it == chunks.end()) { reset(); return {}; }
            frame.insert(frame.end(), it->second.begin(), it->second.end());
        }
        reset();
        return frame;
    }
};

// ════════════════════════════════════════════════════════════════════════════════
// UdpStreamServer::Impl
// ════════════════════════════════════════════════════════════════════════════════

struct UdpStreamServer::Impl {
    // ── Config (set before init) ────────────────────────────────────────────────
    std::string  local_ip        = "0.0.0.0";
    std::string  target_ip;              // static override; empty = auto
    int          rx_port         = kUdpDefaultRXPort;
    int          tx_port         = kUdpDefaultTXPort;
    bool         localhost_debug = false;

    // ── State ───────────────────────────────────────────────────────────────────
    std::atomic<bool>     initialized{false};
    std::atomic<bool>     running{false};
    SOCKET                sock{INVALID_SOCKET};
    std::thread           recv_thread;

    // Target IP discovered from first valid inbound packet
    mutable std::mutex    target_mtx;
    std::string           target_ip_discovered;

    // ── Sequence counters (one per packet type) ─────────────────────────────────
    std::atomic<uint32_t> seq_jpeg{0};
    std::atomic<uint32_t> seq_command{0};
    std::atomic<uint32_t> seq_gui{0};
    std::atomic<uint32_t> seq_composite{0};

    // ── Callbacks ───────────────────────────────────────────────────────────────
    mutable std::mutex    cb_mtx;
    UdpOnInputFn          on_input_fn;
    UdpOnJpegFn           on_jpeg_fn;
    UdpLogFn              log_fn;

    // ── JPEG reassembler ────────────────────────────────────────────────────────
    JpegReassembler jpeg_reasm;

    // ────────────────────────────────────────────────────────────────────────────
    void log(const char* fmt, ...) {
        char buf[512];
        va_list ap; va_start(ap, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        std::lock_guard<std::mutex> lk(cb_mtx);
        if (log_fn) log_fn(buf);
    }

    // ── Header builder ──────────────────────────────────────────────────────────
    // Writes 18 bytes starting at buf[0].
    static void make_header(uint8_t* buf, UdpPacketType ptype, uint32_t seq) {
        buf[0] = 'X'; buf[1] = 'R'; buf[2] = 'R'; buf[3] = 'O';
        buf[4] = 1;         // XRRO version
        buf[5] = static_cast<uint8_t>(ptype);
        write_u32le(buf + 6,  seq);
        write_u64le(buf + 10, timestamp_us_now());
    }

    // ── Resolved target IP (config override or auto-discovered) ─────────────────
    std::string get_target_ip() const {
        if (!target_ip.empty()) return target_ip;
        std::lock_guard<std::mutex> lk(target_mtx);
        return target_ip_discovered;
    }

    void maybe_set_discovered_ip(const sockaddr_in& from) {
        if (!target_ip.empty()) return;  // static override — ignore
        std::lock_guard<std::mutex> lk(target_mtx);
        if (!target_ip_discovered.empty()) return;  // already set
        char ipbuf[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &from.sin_addr, ipbuf, sizeof(ipbuf));
        // Skip our own loopback debug reflections
        if (std::strcmp(ipbuf, "127.0.0.1") == 0) return;
        target_ip_discovered = ipbuf;
        log("UDP: auto-discovered target IP %s  (will TX to port %d)", ipbuf, tx_port);
    }

    // ── Low-level send ──────────────────────────────────────────────────────────
    // Sends datagram to the headset (and optionally to localhost for debug).
    bool do_send(const uint8_t* data, int len) {
        if (sock == INVALID_SOCKET || len <= 0) return false;

        bool ok = false;
        const std::string hip = get_target_ip();

        if (!hip.empty()) {
            sockaddr_in dst{};
            dst.sin_family = AF_INET;
            dst.sin_port   = htons(static_cast<u_short>(tx_port));
            inet_pton(AF_INET, hip.c_str(), &dst.sin_addr);
            int r = ::sendto(sock, reinterpret_cast<const char*>(data), len, 0,
                             reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
            ok = (r != SOCKET_ERROR);
        }

        if (localhost_debug) {
            // Mirror to 127.0.0.1:rx_port so a local Python viewer can inspect it.
            // NOTE: these loopback packets arrive on our own recv socket; they are
            // filtered out in run_recv() by checking the source address.
            sockaddr_in lo{};
            lo.sin_family      = AF_INET;
            lo.sin_port        = htons(static_cast<u_short>(rx_port));
            lo.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            ::sendto(sock, reinterpret_cast<const char*>(data), len, 0,
                     reinterpret_cast<sockaddr*>(&lo), sizeof(lo));
        }

        return ok;
    }

    // ── Receive loop (background thread) ────────────────────────────────────────
    void run_recv() {
        log("UDP RX: receive thread started on port %d", rx_port);
        std::vector<uint8_t> buf(65536);

        while (running.load()) {
            sockaddr_in from{};
            int from_len = sizeof(from);

            int n = ::recvfrom(sock,
                               reinterpret_cast<char*>(buf.data()),
                               static_cast<int>(buf.size()),
                               0,
                               reinterpret_cast<sockaddr*>(&from),
                               &from_len);

            if (n <= 0) {
                int err = WSAGetLastError();
                // WSAETIMEDOUT / WSAEWOULDBLOCK are normal (SO_RCVTIMEO)
                if (err == WSAETIMEDOUT || err == WSAEWOULDBLOCK) continue;
                if (!running.load()) break;   // socket closed by deinit()
                // Real error — log and keep trying
                log("UDP RX: recvfrom error %d", err);
                continue;
            }

            // Discard our own loopback debug reflections
            if (from.sin_addr.s_addr == htonl(INADDR_LOOPBACK)) continue;

            if (n < static_cast<int>(kUdpHeaderSize)) continue;

            // ── Header validation ────────────────────────────────────────────
            if (buf[0]!='X' || buf[1]!='R' || buf[2]!='R' || buf[3]!='O') continue;
            if (buf[4] != 1) continue;   // unknown version

            const uint8_t  ptype = buf[5];
            const uint32_t seq   = read_u32le(buf.data() + 6);
            const uint64_t ts_us = read_u64le(buf.data() + 10);

            // Auto-discover headset IP
            maybe_set_discovered_ip(from);

            // ── Dispatch by packet type ──────────────────────────────────────
            if (ptype == UDP_PKT_INPUT_AND_GUI) {
                // ── 0x01 InputAndGui: ControllerState×2 + GuiState ──────────
                if (n < static_cast<int>(kUdpInputTotal)) continue;

                UdpInputEvent evt;
                evt.seq          = seq;
                evt.timestamp_us = ts_us;
                size_t off       = kUdpHeaderSize;
                off = parse_controller(buf.data(), off, evt.left);
                off = parse_controller(buf.data(), off, evt.right);
                for (int i = 0; i < 8; ++i, off += 4)
                    evt.gui[i] = static_cast<int32_t>(read_u32le(buf.data() + off));

                UdpOnInputFn fn;
                { std::lock_guard<std::mutex> lk(cb_mtx); fn = on_input_fn; }
                if (fn) fn(evt);
            }
            else if (ptype == UDP_PKT_JPEG_CHUNK) {
                // ── 0x02 JpegChunk: chunked inbound JPEG frame ───────────────
                const size_t min_sz = kUdpHeaderSize + kUdpChunkHdrSize;
                if (n < static_cast<int>(min_sz)) continue;

                const uint8_t* h   = buf.data() + kUdpHeaderSize;
                const uint32_t fid = read_u32le(h);
                const uint16_t idx = read_u16le(h + 4);
                const uint16_t tot = read_u16le(h + 6);
                // h+8  : TotalFrameSize (informational)
                const uint32_t psz = read_u32le(h + 12);

                const uint8_t* payload = buf.data() + min_sz;
                const size_t   avail   = static_cast<size_t>(n) - min_sz;
                const size_t   actual  = std::min(static_cast<size_t>(psz), avail);

                auto frame = jpeg_reasm.feed(fid, idx, tot, payload, actual);
                if (!frame.empty()) {
                    UdpOnJpegFn fn;
                    { std::lock_guard<std::mutex> lk(cb_mtx); fn = on_jpeg_fn; }
                    if (fn) fn(fid, frame.data(), frame.size());
                }
            }
            else if (ptype == UDP_PKT_COMPOSITE) {
                // ── 0x05 Composite: Input + Gui + Command + optional JPEG ────
                const size_t min_sz = kUdpHeaderSize + 2*kUdpCtrlSize + kUdpGuiSize;
                if (n < static_cast<int>(min_sz)) continue;

                UdpInputEvent evt;
                evt.seq          = seq;
                evt.timestamp_us = ts_us;
                size_t off       = kUdpHeaderSize;
                off = parse_controller(buf.data(), off, evt.left);
                off = parse_controller(buf.data(), off, evt.right);
                for (int i = 0; i < 8; ++i, off += 4)
                    evt.gui[i] = static_cast<int32_t>(read_u32le(buf.data() + off));

                {
                    UdpOnInputFn fn;
                    { std::lock_guard<std::mutex> lk(cb_mtx); fn = on_input_fn; }
                    if (fn) fn(evt);
                }

                // Optional embedded JPEG — skip CommandPacket, then read JpegSize
                off += kUdpCommandSize;  // jump over Command section
                if (static_cast<size_t>(n) >= off + 4) {
                    const uint32_t jpeg_sz = read_u32le(buf.data() + off); off += 4;
                    if (jpeg_sz > 0 &&
                        static_cast<size_t>(n) >= off + jpeg_sz) {
                        UdpOnJpegFn fn;
                        { std::lock_guard<std::mutex> lk(cb_mtx); fn = on_jpeg_fn; }
                        if (fn) fn(seq, buf.data() + off, jpeg_sz);
                    }
                }
            }
            // Other packet types (0x03, 0x04) originate from this PC — ignore.
        }

        log("UDP RX: receive thread stopped");
    }
};

// ════════════════════════════════════════════════════════════════════════════════
// UdpStreamServer — public API
// ════════════════════════════════════════════════════════════════════════════════

UdpStreamServer::UdpStreamServer()  : d_(std::make_unique<Impl>()) {}
UdpStreamServer::~UdpStreamServer() { deinit(); }

// ── Configuration setters ──────────────────────────────────────────────────────

void UdpStreamServer::setLocalIP(const std::string& ip) {
    d_->local_ip = ip.empty() ? "0.0.0.0" : ip;
}
void UdpStreamServer::setTargetIP(const std::string& ip) {
    d_->target_ip = ip;
}
void UdpStreamServer::setRXPort(int port) {
    d_->rx_port = port;
}
void UdpStreamServer::setTXPort(int port) {
    d_->tx_port = port;
}
void UdpStreamServer::setLocalhostDebug(bool enable) {
    d_->localhost_debug = enable;
}

// ── Callbacks ──────────────────────────────────────────────────────────────────

void UdpStreamServer::setOnInputCallback(UdpOnInputFn fn) {
    std::lock_guard<std::mutex> lk(d_->cb_mtx);
    d_->on_input_fn = std::move(fn);
}
void UdpStreamServer::setOnJpegCallback(UdpOnJpegFn fn) {
    std::lock_guard<std::mutex> lk(d_->cb_mtx);
    d_->on_jpeg_fn = std::move(fn);
}
void UdpStreamServer::setLogCallback(UdpLogFn fn) {
    std::lock_guard<std::mutex> lk(d_->cb_mtx);
    d_->log_fn = std::move(fn);
}

// ── Lifecycle ──────────────────────────────────────────────────────────────────

bool UdpStreamServer::init() {
    if (d_->initialized.load()) return true;   // already open

    // Create UDP socket
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        d_->log("UDP init: socket() failed (WSA %d)", WSAGetLastError());
        return false;
    }

    // SO_REUSEADDR — allows quick restart without "port in use" errors
    int opt = 1;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&opt), sizeof(opt));

    // SO_RCVTIMEO — unblocks recvfrom periodically so the thread can check 'running'
    DWORD timeout_ms = 200;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));

    // Bind to local_ip:rx_port
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<u_short>(d_->rx_port));
    inet_pton(AF_INET,
              d_->local_ip.empty() ? "0.0.0.0" : d_->local_ip.c_str(),
              &addr.sin_addr);

    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        d_->log("UDP init: bind() failed on %s:%d (WSA %d)",
                d_->local_ip.c_str(), d_->rx_port, WSAGetLastError());
        ::closesocket(s);
        return false;
    }

    d_->sock = s;
    d_->initialized.store(true);
    d_->log("UDP: socket bound to %s:%d  TX→%s:%d",
            d_->local_ip.c_str(), d_->rx_port,
            d_->target_ip.empty() ? "(auto)" : d_->target_ip.c_str(),
            d_->tx_port);
    return true;
}

bool UdpStreamServer::start() {
    if (!d_->initialized.load()) {
        d_->log("UDP start: call init() first");
        return false;
    }
    if (d_->running.load()) return true;   // already running

    d_->running.store(true);
    d_->recv_thread = std::thread([this]{ d_->run_recv(); });
    return true;
}

void UdpStreamServer::stop() {
    if (!d_->running.load()) return;
    d_->running.store(false);
    if (d_->recv_thread.joinable())
        d_->recv_thread.join();
}

void UdpStreamServer::deinit() {
    stop();   // join receive thread first

    if (d_->sock != INVALID_SOCKET) {
        ::closesocket(d_->sock);
        d_->sock = INVALID_SOCKET;
    }
    d_->initialized.store(false);
    d_->target_ip_discovered.clear();
}

bool UdpStreamServer::isRunning()     const { return d_->running.load(); }
bool UdpStreamServer::isInitialized() const { return d_->initialized.load(); }

// ── Outbound sends ─────────────────────────────────────────────────────────────

void UdpStreamServer::sendJpeg(const uint8_t* jpeg, size_t size,
                                uint32_t frame_id)
{
    if (!d_->initialized.load() || !jpeg || size == 0) return;

    if (frame_id == 0)
        frame_id = d_->seq_jpeg.fetch_add(1, std::memory_order_relaxed);

    const int chunk_size = kUdpChunkMaxBytes;
    const int total_chunks = static_cast<int>(
        (size + static_cast<size_t>(chunk_size) - 1) / static_cast<size_t>(chunk_size));

    // Pre-allocate packet buffer: header + chunk-header + payload
    std::vector<uint8_t> pkt(kUdpHeaderSize + kUdpChunkHdrSize + chunk_size);

    for (int idx = 0; idx < total_chunks; ++idx) {
        const size_t offset      = static_cast<size_t>(idx) * chunk_size;
        const size_t payload_sz  = std::min(
            static_cast<size_t>(chunk_size), size - offset);

        // Header (18 B)
        const uint32_t seq = d_->seq_jpeg.fetch_add(1, std::memory_order_relaxed);
        Impl::make_header(pkt.data(), UDP_PKT_JPEG_CHUNK, seq);

        // Chunk header (16 B)
        uint8_t* ch = pkt.data() + kUdpHeaderSize;
        write_u32le(ch + 0, frame_id);
        write_u16le(ch + 4, static_cast<uint16_t>(idx));
        write_u16le(ch + 6, static_cast<uint16_t>(total_chunks));
        write_u32le(ch + 8, static_cast<uint32_t>(size));
        write_u32le(ch + 12, static_cast<uint32_t>(payload_sz));

        // Payload
        std::memcpy(pkt.data() + kUdpHeaderSize + kUdpChunkHdrSize,
                    jpeg + offset, payload_sz);

        d_->do_send(pkt.data(),
                    static_cast<int>(kUdpHeaderSize + kUdpChunkHdrSize + payload_sz));
    }
}

void UdpStreamServer::sendCommand(uint32_t      command_id,
                                   const int32_t params[8],
                                   const float   fparams[4])
{
    if (!d_->initialized.load()) return;

    // Header (18 B) + Command (52 B) = 70 B
    uint8_t pkt[kUdpHeaderSize + kUdpCommandSize];
    const uint32_t seq = d_->seq_command.fetch_add(1, std::memory_order_relaxed);
    Impl::make_header(pkt, UDP_PKT_COMMAND, seq);

    uint8_t* p = pkt + kUdpHeaderSize;
    write_u32le(p, command_id); p += 4;
    for (int i = 0; i < 8; ++i, p += 4)
        write_u32le(p, params ? static_cast<uint32_t>(params[i]) : 0u);
    for (int i = 0; i < 4; ++i, p += 4)
        write_f32le(p, fparams ? fparams[i] : 0.f);

    d_->do_send(pkt, static_cast<int>(sizeof(pkt)));
}

void UdpStreamServer::sendGuiUpdate(const int32_t state[8]) {
    if (!d_->initialized.load()) return;

    // Header (18 B) + GuiState (32 B) = 50 B
    uint8_t pkt[kUdpHeaderSize + kUdpGuiSize];
    const uint32_t seq = d_->seq_gui.fetch_add(1, std::memory_order_relaxed);
    Impl::make_header(pkt, UDP_PKT_GUI_UPDATE, seq);

    uint8_t* p = pkt + kUdpHeaderSize;
    for (int i = 0; i < 8; ++i, p += 4)
        write_u32le(p, state ? static_cast<uint32_t>(state[i]) : 0u);

    d_->do_send(pkt, static_cast<int>(sizeof(pkt)));
}

void UdpStreamServer::sendComposite(const int32_t  gui[8],
                                     uint32_t       command_id,
                                     const int32_t  params[8],
                                     const float    fparams[4],
                                     const uint8_t* jpeg,
                                     size_t         jpeg_size)
{
    if (!d_->initialized.load()) return;

    // Fixed part: Header(18) + Input×2(168) + Gui(32) + Command(52) + JpegSize(4)
    //           = 18 + 256 = 274 bytes, then JPEG payload appended.
    const size_t fixed_sz = kUdpHeaderSize + kUdpCompFixedAfterHdr;
    std::vector<uint8_t> pkt(fixed_sz + jpeg_size);

    const uint32_t seq = d_->seq_composite.fetch_add(1, std::memory_order_relaxed);
    Impl::make_header(pkt.data(), UDP_PKT_COMPOSITE, seq);

    size_t off = kUdpHeaderSize;

    // Input section — zeroed (PC has no controller state)
    off = write_controller(pkt.data(), off);   // left  = zeroed
    off = write_controller(pkt.data(), off);   // right = zeroed

    // GUI state
    for (int i = 0; i < 8; ++i, off += 4)
        write_u32le(pkt.data() + off, gui ? static_cast<uint32_t>(gui[i]) : 0u);

    // Command
    write_u32le(pkt.data() + off, command_id); off += 4;
    for (int i = 0; i < 8; ++i, off += 4)
        write_u32le(pkt.data() + off,
                    params ? static_cast<uint32_t>(params[i]) : 0u);
    for (int i = 0; i < 4; ++i, off += 4)
        write_f32le(pkt.data() + off, fparams ? fparams[i] : 0.f);

    // JPEG size + payload
    write_u32le(pkt.data() + off, static_cast<uint32_t>(jpeg_size)); off += 4;
    if (jpeg && jpeg_size > 0)
        std::memcpy(pkt.data() + off, jpeg, jpeg_size);

    d_->do_send(pkt.data(), static_cast<int>(pkt.size()));
}

// ── Getters ────────────────────────────────────────────────────────────────────

std::string UdpStreamServer::localIP()  const { return d_->local_ip; }
std::string UdpStreamServer::targetIP() const { return d_->get_target_ip(); }
int         UdpStreamServer::rxPort()   const { return d_->rx_port; }
int         UdpStreamServer::txPort()   const { return d_->tx_port; }

} // namespace cppdvr
