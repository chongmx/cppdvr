/**
 * dvrip.cpp — XiongMai DVRIP protocol implementation
 *
 * C++ port of py_sample/dvrip.py (DVRIPCam class).
 * Protocol details:
 *   - TCP port 34567 (default)
 *   - 20-byte binary header + JSON payload + 2-byte tail (\x0a\x00)
 *   - Binary video stream: 20-byte header + typed binary frames
 *   - Auth: Sofia hash (custom MD5-derived encoding)
 */

#include "dvrip.h"
#include "platform/platform_net.h"
#include "platform/platform_crypto.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstring>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>

using json = nlohmann::json;

namespace cppdvr {

// ── DVRIP packet header (20 bytes, matches struct.pack("BB2xII2xHI")) ──────────
#pragma pack(push, 1)
struct DVRIPHeader {
    uint8_t  head;       // 0xFF
    uint8_t  version;    // 0 or 1
    uint8_t  pad1[2];    // 0x00 0x00
    uint32_t session;    // little-endian session ID
    uint32_t sequence;   // little-endian packet count
    uint8_t  total_pkt;  // total sub-packets  (0 for single-packet msgs)
    uint8_t  cur_pkt;    // current sub-packet (0 for single-packet msgs)
    uint16_t msg_id;     // little-endian message code
    uint32_t data_len;   // little-endian payload length (including tail)
};
static_assert(sizeof(DVRIPHeader) == 20, "DVRIPHeader must be 20 bytes");
#pragma pack(pop)

// ── Binary frame header (data_type at bytes 0-3, big-endian) ──────────────────
static constexpr uint32_t kFrmIFrame  = 0x1FC;  // I-frame (H264/H265/MPEG4)
static constexpr uint32_t kFrmPFrame  = 0x1FD;  // P-frame
static constexpr uint32_t kFrmAudio   = 0x1FA;  // Audio
static constexpr uint32_t kFrmInfo    = 0x1F9;  // Info/meta
static constexpr uint32_t kFrmJpeg    = 0x1FE;  // JPEG snapshot I-frame
static constexpr uint32_t kFrmJpegSig = 0xFFD8FFE0; // Raw JPEG in payload

// ── Codec type mapping ─────────────────────────────────────────────────────────
static std::string codec_from_media(uint32_t data_type, uint8_t media_byte) {
    if (data_type == kFrmIFrame || data_type == kFrmPFrame || data_type == kFrmJpeg) {
        switch (media_byte) {
            case 0: return "h264";   // some cameras use 0 for H264
            case 1: return "mpeg4";
            case 2: return "h264";
            case 3: return "h265";
            default: return "h264";  // unknown — assume H264 (most common)
        }
    } else if (data_type == kFrmInfo) {
        if (media_byte == 1 || media_byte == 6) return "info";
    } else if (data_type == kFrmAudio) {
        if (media_byte == 0x0E) return "g711a";
    }
    return "";
}

// ── Unpack DVR datetime (32-bit packed field) ─────────────────────────────────
static void unpack_datetime(uint32_t v, FrameMeta::dt_type& dt) {
    dt.second = v & 0x3F;
    dt.minute = (v >> 6)  & 0x3F;
    dt.hour   = (v >> 12) & 0x1F;
    dt.day    = (v >> 17) & 0x1F;
    dt.month  = (v >> 22) & 0x0F;
    dt.year   = ((v >> 26) & 0x3F) + 2000;
}

// ════════════════════════════════════════════════════════════════════════════════
// DVRIPCam::Impl — private implementation (PIMPL)
// ════════════════════════════════════════════════════════════════════════════════
struct DVRIPCam::Impl {
    // Config
    std::string ip;
    std::string user;
    std::string hash_pass;
    std::string proto;     // "tcp" or "udp"
    int         port;
    int         timeout_sec{20};
    std::string bind_ip;   // local IP to bind before connect (multi-NIC)

    // Socket
    PlatformSocket sock{INVALID_PLATFORM_SOCKET};

    // Protocol state
    uint32_t    session{0};
    uint32_t    packet_count{0};
    int         alive_time{20};

    // Threading
    mutable std::mutex  send_mutex;   // serialise send+recv pairs
    std::atomic<bool>   monitoring{false};
    std::atomic<bool>   keepalive_run{false};
    std::atomic<bool>   recv_failed{false};  // set by tcp_recv_exact on socket error
    std::thread         monitor_thread;
    std::thread         keepalive_thread;

    std::string last_err;

    // ── Log callback ──────────────────────────────────────────────────────────
    mutable std::mutex               log_mutex;
    std::function<void(const char*)> log_fn;

    void log(const char* fmt, ...) {
        std::function<void(const char*)> fn;
        {
            std::lock_guard<std::mutex> lk(log_mutex);
            fn = log_fn;
        }
        if (!fn) return;
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        fn(buf);
    }

    // ── Low-level I/O ──────────────────────────────────────────────────────────

    bool tcp_send(const uint8_t* data, size_t len) {
        if (sock == INVALID_PLATFORM_SOCKET) return false;
        int sent = 0;
        while (static_cast<size_t>(sent) < len) {
            int r = platform_send(sock, data + sent, static_cast<int>(len - sent), timeout_sec * 1000);
            if (r <= 0) return false;
            sent += r;
        }
        return true;
    }

    // Read exactly 'len' bytes; returns empty on timeout/error.
    // Sets recv_failed=true when the error is a socket-level failure
    // (connection closed/reset) so the monitor loop can distinguish it
    // from an unrecognised frame type (which is recoverable).
    std::vector<uint8_t> tcp_recv_exact(size_t len) {
        std::vector<uint8_t> buf(len);
        size_t received = 0;
        auto deadline = std::chrono::steady_clock::now()
                      + std::chrono::seconds(timeout_sec);
        while (received < len) {
            int r = platform_recv(sock, buf.data() + received, 
                                 static_cast<int>(len - received), timeout_sec * 1000);
            if (r <= 0) {
                recv_failed.store(true);   // socket-level error / EOF
                buf.clear();
                return buf;
            }
            received += r;
            if (received < len &&
                std::chrono::steady_clock::now() > deadline) {
                buf.clear(); return buf;
            }
        }
        return buf;
    }

    // ── Packet building ────────────────────────────────────────────────────────

    // Build a DVRIP packet with version=0 tail (\x0a\x00).
    std::vector<uint8_t> build_packet(uint16_t msg_id,
                                       const std::string& payload,
                                       uint8_t version = 0) {
        const uint8_t tail_v0[2] = { 0x0a, 0x00 };
        const uint8_t tail_v1[1] = { 0x00 };
        const uint8_t* tail      = (version == 0) ? tail_v0 : tail_v1;
        size_t         tail_len  = (version == 0) ? 2 : 1;

        uint32_t data_len = static_cast<uint32_t>(payload.size() + tail_len);

        DVRIPHeader hdr{};
        hdr.head     = 0xFF;
        hdr.version  = version;
        hdr.session  = session;
        hdr.sequence = packet_count;
        hdr.msg_id   = msg_id;
        hdr.data_len = data_len;

        std::vector<uint8_t> pkt;
        pkt.reserve(sizeof(hdr) + payload.size() + tail_len);
        pkt.insert(pkt.end(),
                   reinterpret_cast<const uint8_t*>(&hdr),
                   reinterpret_cast<const uint8_t*>(&hdr) + sizeof(hdr));
        pkt.insert(pkt.end(), payload.begin(), payload.end());
        pkt.insert(pkt.end(), tail, tail + tail_len);
        return pkt;
    }

    // ── Send + receive a JSON message ─────────────────────────────────────────

    // Sends a DVRIP message, optionally waits for and returns the JSON reply.
    // wait_response=false: send only (no reply read). Returns empty json {}.
    json send_msg(uint16_t msg_id, const json& data = json::object(),
                  bool wait_response = true, uint8_t version = 0) {
        if (sock == INVALID_PLATFORM_SOCKET) return { {"Ret", 101} };

        std::unique_lock<std::mutex> lock(send_mutex);

        // Serialise payload
        std::string payload;
        if (!data.is_null() && !data.empty()) {
            payload = data.dump(-1, ' ', false,
                                json::error_handler_t::ignore);
        }

        auto pkt = build_packet(msg_id, payload, version);
        ++packet_count;

        if (!tcp_send(pkt.data(), pkt.size())) {
            last_err = "send failed";
            return { {"Ret", 101} };
        }

        if (!wait_response) return json::object();

        // Read 20-byte reply header
        auto hdr_buf = tcp_recv_exact(sizeof(DVRIPHeader));
        if (hdr_buf.empty()) { last_err = "recv header failed"; return {}; }

        DVRIPHeader rhdr;
        std::memcpy(&rhdr, hdr_buf.data(), sizeof(rhdr));
        session = rhdr.session;  // update session from response

        // Read JSON payload
        if (rhdr.data_len == 0) return json::object();

        auto payload_buf = tcp_recv_exact(rhdr.data_len);
        if (payload_buf.empty()) { last_err = "recv payload failed"; return {}; }

        // Strip trailing \x0a\x00 and parse
        size_t json_len = payload_buf.size();
        while (json_len > 0 &&
               (payload_buf[json_len - 1] == 0x00 ||
                payload_buf[json_len - 1] == 0x0a))
            --json_len;

        try {
            return json::parse(payload_buf.begin(),
                               payload_buf.begin() + json_len);
        } catch (...) {
            // Return raw bytes as a string field for debugging
            return { {"raw", std::string(payload_buf.begin(),
                                         payload_buf.end())} };
        }
    }

    // Like send_msg but payload is raw binary bytes rather than JSON.
    // Used for the bitmap overlay (msg 0x041A).
    json send_raw_msg(uint16_t msg_id, const std::vector<uint8_t>& payload) {
        if (sock == INVALID_PLATFORM_SOCKET) return { {"Ret", 101} };

        std::unique_lock<std::mutex> lock(send_mutex);

        uint32_t data_len = static_cast<uint32_t>(payload.size() + 2);

        DVRIPHeader hdr{};
        hdr.head     = 0xFF;
        hdr.version  = 0;
        hdr.session  = session;
        hdr.sequence = packet_count;
        hdr.msg_id   = msg_id;
        hdr.data_len = data_len;

        std::vector<uint8_t> pkt;
        pkt.reserve(sizeof(hdr) + payload.size() + 2);
        pkt.insert(pkt.end(),
                   reinterpret_cast<const uint8_t*>(&hdr),
                   reinterpret_cast<const uint8_t*>(&hdr) + sizeof(hdr));
        pkt.insert(pkt.end(), payload.begin(), payload.end());
        pkt.push_back(0x0a);
        pkt.push_back(0x00);
        ++packet_count;

        if (!tcp_send(pkt.data(), pkt.size())) {
            last_err = "send_raw_msg: send failed";
            return { {"Ret", 101} };
        }

        auto hdr_buf = tcp_recv_exact(sizeof(DVRIPHeader));
        if (hdr_buf.empty()) { last_err = "send_raw_msg: recv header failed"; return {}; }

        DVRIPHeader rhdr;
        std::memcpy(&rhdr, hdr_buf.data(), sizeof(rhdr));
        session = rhdr.session;

        if (rhdr.data_len == 0) return json::object();

        auto pbuf = tcp_recv_exact(rhdr.data_len);
        if (pbuf.empty()) { last_err = "send_raw_msg: recv payload failed"; return {}; }

        size_t jlen = pbuf.size();
        while (jlen > 0 && (pbuf[jlen-1] == 0x00 || pbuf[jlen-1] == 0x0a)) --jlen;

        try { return json::parse(pbuf.begin(), pbuf.begin() + jlen); }
        catch (...) { return { {"raw", std::string(pbuf.begin(), pbuf.end())} }; }
    }

    // ── Reassemble binary payload (video frames, snapshots) ───────────────────
    // Mirrors Python reassemble_bin_payload().
    std::vector<uint8_t> reassemble_bin_payload(FrameMeta& meta) {
        size_t remaining = 0;
        std::vector<uint8_t> buf;
        auto deadline = std::chrono::steady_clock::now()
                      + std::chrono::seconds(timeout_sec);

        while (true) {
            // Read 20-byte sub-packet header
            auto hdr_buf = tcp_recv_exact(sizeof(DVRIPHeader));
            if (hdr_buf.empty()) return {};

            DVRIPHeader rhdr;
            std::memcpy(&rhdr, hdr_buf.data(), sizeof(rhdr));
            uint32_t pkt_len = rhdr.data_len;

            auto pkt = tcp_recv_exact(pkt_len);
            if (pkt.empty()) return {};

            if (remaining == 0) {
                // First sub-packet: parse frame header
                if (pkt.size() < 4) return {};

                uint32_t data_type = (static_cast<uint32_t>(pkt[0]) << 24) |
                                     (static_cast<uint32_t>(pkt[1]) << 16) |
                                     (static_cast<uint32_t>(pkt[2]) <<  8) |
                                      static_cast<uint32_t>(pkt[3]);

                size_t frame_hdr_len = 8; // default

                if (data_type == kFrmIFrame || data_type == kFrmJpeg) {
                    // 16-byte frame header: [data_type(4)] [media(1) fps(1) w(1) h(1) dt(4) len(4)]
                    if (pkt.size() < 16) return {};
                    frame_hdr_len = 16;

                    uint8_t media_byte = pkt[4];
                    meta.fps          = pkt[5];
                    meta.width        = pkt[6] * 8;
                    meta.height       = pkt[7] * 8;

                    uint32_t dt_val;
                    std::memcpy(&dt_val, &pkt[8], 4);
                    unpack_datetime(dt_val, meta.dt);

                    std::memcpy(&remaining, &pkt[12], 4);

                    meta.type  = codec_from_media(data_type, media_byte);
                    meta.frame = (data_type == kFrmIFrame) ? "I" : "I"; // JPEG is also keyframe

                } else if (data_type == kFrmPFrame) {
                    // 8-byte header: [data_type(4)] [len(4)]
                    if (pkt.size() < 8) return {};
                    std::memcpy(&remaining, &pkt[4], 4);
                    meta.frame = "P";

                } else if (data_type == kFrmAudio) {
                    // 8-byte header: [data_type(4)] [media(1) samp_rate(1) len(2)]
                    if (pkt.size() < 8) return {};
                    uint8_t media_byte  = pkt[4];
                    uint16_t len16;
                    std::memcpy(&len16, &pkt[6], 2);
                    remaining  = len16;
                    meta.type  = codec_from_media(data_type, media_byte);

                } else if (data_type == kFrmInfo) {
                    if (pkt.size() < 8) return {};
                    uint8_t media_byte = pkt[4];
                    uint16_t len16;
                    std::memcpy(&len16, &pkt[6], 2);
                    remaining = len16;
                    meta.type = codec_from_media(data_type, media_byte);

                } else if (data_type == kFrmJpegSig) {
                    // Raw JPEG already in packet — return it directly
                    return std::vector<uint8_t>(pkt.begin(), pkt.end());

                } else {
                    last_err = "unknown frame data_type: " +
                               std::to_string(data_type);
                    return {};
                }

                // Append payload bytes (after frame header)
                size_t payload_bytes = pkt_len > frame_hdr_len
                                     ? pkt_len - frame_hdr_len : 0;
                buf.insert(buf.end(),
                           pkt.begin() + frame_hdr_len,
                           pkt.begin() + frame_hdr_len + payload_bytes);
                if (remaining <= payload_bytes) return buf;
                remaining -= payload_bytes;

            } else {
                // Continuation sub-packets
                buf.insert(buf.end(), pkt.begin(), pkt.end());
                if (remaining <= pkt_len) return buf;
                remaining -= pkt_len;
            }

            if (std::chrono::steady_clock::now() > deadline) {
                last_err = "reassemble timeout";
                return {};
            }
        }
    }

    // ── Keep-alive loop ────────────────────────────────────────────────────────
    void keepalive_loop() {
        while (keepalive_run.load()) {
            // Sleep alive_time seconds in small chunks so we can exit quickly
            for (int i = 0; i < alive_time && keepalive_run.load(); ++i)
                std::this_thread::sleep_for(std::chrono::seconds(1));

            if (!keepalive_run.load()) break;
            if (sock == INVALID_PLATFORM_SOCKET) break;

            char session_str[16];
            std::snprintf(session_str, sizeof(session_str),
                          "0x%08X", session);

            auto ret = send_msg(
                1006,  // QCODES["KeepAlive"]
                { {"Name", "KeepAlive"}, {"SessionID", session_str} }
            );

            if (ret.empty() || ret.value("Ret", 101) == 101) {
                last_err = "keep-alive failed — closing";
                if (sock != INVALID_PLATFORM_SOCKET) {
                    platform_close(sock);
                    sock = INVALID_PLATFORM_SOCKET;
                }
                break;
            }
        }
    }

    // ── Monitor loop ──────────────────────────────────────────────────────────
    void monitor_loop(FrameCallback callback, const std::string& stream) {
        // Build params
        json params = {
            {"Channel",    0},
            {"CombinMode", "NONE"},
            {"StreamType", stream},
            {"TransMode",  "TCP"}
        };

        char session_str[16];
        std::snprintf(session_str, sizeof(session_str), "0x%08X", session);

        // Claim
        auto claim_ret = send_msg(
            1413,  // QCODES["OPMonitor"]
            { {"Name",      "OPMonitor"},
              {"SessionID", session_str},
              {"OPMonitor", { {"Action", "Claim"}, {"Parameter", params} }} }
        );

        if (claim_ret.empty()) {
            last_err = "OPMonitor Claim failed";
            log("DVR: OPMonitor Claim failed — no response");
            monitoring.store(false);
            return;
        }
        int ret_code = claim_ret.value("Ret", 101);
        if (ret_code != 100 && ret_code != 515) {
            last_err = "OPMonitor Claim rejected: " + std::to_string(ret_code);
            log("DVR: OPMonitor Claim rejected Ret=%d", ret_code);
            monitoring.store(false);
            return;
        }
        log("DVR: OPMonitor Claim OK (Ret=%d)", ret_code);

        // Start (no response expected)
        {
            std::unique_lock<std::mutex> lock(send_mutex);
            std::snprintf(session_str, sizeof(session_str), "0x%08X", session);
            json start_data = {
                {"Name",      "OPMonitor"},
                {"SessionID", session_str},
                {"OPMonitor", { {"Action", "Start"}, {"Parameter", params} }}
            };
            std::string payload = start_data.dump(-1, ' ', false,
                                                   json::error_handler_t::ignore);
            auto pkt = build_packet(1410, payload);
            ++packet_count;
            tcp_send(pkt.data(), pkt.size());
            // no response read intentionally
        }
        log("DVR: OPMonitor Start sent — receiving frames ...");

        // Ensure monitoring flag is cleared when we exit (natural or error path)
        struct MonitoringGuard {
            std::atomic<bool>& flag;
            ~MonitoringGuard() { flag.store(false); }
        } guard{ monitoring };

        // Receive loop — exits when:
        //   a) stop_monitor() sets monitoring=false, OR
        //   b) a socket-level error occurs (recv_failed flag set by tcp_recv_exact)
        recv_failed.store(false);
        bool logged_first_frame = false;
        while (monitoring.load()) {
            FrameMeta meta;
            auto frame = reassemble_bin_payload(meta);

            if (frame.empty()) {
                if (!monitoring.load()) break;
                // Socket closed / connection reset — exit so run_camera reconnects
                if (recv_failed.load()) {
                    log("DVR: socket error — will reconnect");
                    break;
                }
                continue;  // unrecognised frame type — skip and keep going
            }

            if (!logged_first_frame) {
                log("DVR: first frame received  type=%s frame=%s  %dx%d @ %d fps",
                    meta.type.c_str(), meta.frame.c_str(),
                    meta.width, meta.height, meta.fps);
                logged_first_frame = true;
            }

            if (callback)
                callback(frame.data(), frame.size(), meta);
        }
    }
};

// ════════════════════════════════════════════════════════════════════════════════
// Sofia hash (XiongMai proprietary password hash)
// MD5(password)  →  pair even/odd bytes  →  sum mod 62  →  alphanumeric char
// ════════════════════════════════════════════════════════════════════════════════
std::string DVRIPCam::sofia_hash(const std::string& password) {
    uint8_t md5[PLATFORM_MD5_DIGEST_SIZE] = {};
    
    if (platform_md5(password.data(), static_cast<int>(password.size()), md5) != 0) {
        return "";
    }

    static const char chars[] =
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    std::string result;
    result.reserve(8);
    for (int i = 0; i < 8; ++i)
        result += chars[(md5[2*i] + md5[2*i+1]) % 62];
    return result;
}

// ════════════════════════════════════════════════════════════════════════════════
// DVRIPCam public API
// ════════════════════════════════════════════════════════════════════════════════

DVRIPCam::DVRIPCam(std::string ip, std::string user,
                   std::string password, std::string proto, int port)
    : d_(std::make_unique<Impl>())
{
    d_->ip        = std::move(ip);
    d_->user      = std::move(user);
    d_->hash_pass = sofia_hash(password);
    d_->proto     = std::move(proto);
    d_->port      = (port > 0) ? port
                  : (d_->proto == "udp" ? kUdpPort : kTcpPort);
}

DVRIPCam::~DVRIPCam() {
    stop_monitor();
    close();
}

bool DVRIPCam::connect(int timeout_sec) {
    d_->timeout_sec = timeout_sec;

    // Initialize platform networking
    platform_net_init();

    PlatformSocket s = platform_socket();
    if (s == INVALID_PLATFORM_SOCKET) {
        d_->last_err = "socket() failed";
        return false;
    }

    // Bind to a specific local IP when the host has multiple NICs.
    // Without this, routing may happen through the wrong interface.
    if (!d_->bind_ip.empty()) {
        if (platform_bind(s, d_->bind_ip, 0) != PlatformSocketError::Success) {
            platform_close(s);
            d_->last_err = "bind() to local IP " + d_->bind_ip + " failed";
            return false;
        }
    }

    // Connect to DVR
    PlatformSocketError err = platform_connect(s, d_->ip, d_->port, timeout_sec * 1000);
    if (err != PlatformSocketError::Success) {
        platform_close(s);
        d_->last_err = "connect() failed";
        return false;
    }

    d_->sock = s;
    return true;
}

void DVRIPCam::close() {
    d_->keepalive_run.store(false);
    if (d_->keepalive_thread.joinable())
        d_->keepalive_thread.join();

    if (d_->sock != INVALID_PLATFORM_SOCKET) {
        platform_close(d_->sock);
        d_->sock = INVALID_PLATFORM_SOCKET;
    }
}

bool DVRIPCam::login() {
    if (d_->sock == INVALID_PLATFORM_SOCKET)
        if (!connect()) return false;

    char session_str[16] = "0x00000000";

    auto resp = d_->send_msg(
        1000,   // login message
        { {"EncryptType", "MD5"},
          {"LoginType",   "DVRIP-Web"},
          {"PassWord",    d_->hash_pass},
          {"UserName",    d_->user} }
    );

    if (resp.empty()) return false;

    int ret = resp.value("Ret", 101);
    if (ret != 100 && ret != 515) {
        d_->last_err = "login failed Ret=" + std::to_string(ret);
        return false;
    }

    // Parse session ID ("0x1234ABCD")
    std::string sid = resp.value("SessionID", "0x00000000");
    d_->session     = static_cast<uint32_t>(std::stoul(sid, nullptr, 16));
    d_->alive_time  = resp.value("AliveInterval", 20);

    // Start keep-alive thread
    d_->keepalive_run.store(true);
    d_->keepalive_thread = std::thread([this]{ d_->keepalive_loop(); });

    return true;
}

bool DVRIPCam::is_connected()  const { return d_->sock != INVALID_PLATFORM_SOCKET; }
bool DVRIPCam::is_monitoring() const { return d_->monitoring.load(); }

bool DVRIPCam::start_monitor(FrameCallback callback,
                              const std::string& stream) {
    if (!is_connected()) return false;
    if (d_->monitoring.load()) return false;  // already running

    // Stop the keepalive thread BEFORE entering the monitor receive loop.
    // Both share the same TCP socket: if keepalive calls send_msg() while
    // the monitor loop is reading binary video frames, it will consume NAL
    // bytes from the socket instead of the JSON keepalive response —
    // corrupting the stream so no I-frames ever reach the pipeline.
    // The DVR maintains session liveness while video data is flowing.
    d_->keepalive_run.store(false);
    if (d_->keepalive_thread.joinable())
        d_->keepalive_thread.join();

    d_->monitoring.store(true);
    d_->monitor_thread = std::thread([this, callback, stream](){
        d_->monitor_loop(callback, stream);
    });
    return true;
}

void DVRIPCam::stop_monitor() {
    d_->monitoring.store(false);
    // Unblock any pending recv by closing the socket
    if (d_->sock != INVALID_PLATFORM_SOCKET) {
        // Close socket to unblock any pending recv()
        platform_close(d_->sock);
        d_->sock = INVALID_PLATFORM_SOCKET;
    }
    if (d_->monitor_thread.joinable())
        d_->monitor_thread.join();
}

std::vector<uint8_t> DVRIPCam::snapshot(int channel) {
    char session_str[16];
    std::snprintf(session_str, sizeof(session_str), "0x%08X", d_->session);

    // Request snapshot (no response expected from send_msg)
    d_->send_msg(
        1560,   // QCODES["OPSNAP"]
        { {"Name",      "OPSNAP"},
          {"SessionID", session_str},
          {"OPSNAP",    { {"Channel", channel} }} },
        false   // wait_response = false
    );

    // Read the binary JPEG payload
    FrameMeta meta;
    return d_->reassemble_bin_payload(meta);
}

bool DVRIPCam::ptz(const std::string& cmd, int step, int preset, int channel) {
    json ptz_param = {
        {"AUX",      { {"Number", 0}, {"Status", "On"} }},
        {"Channel",  channel},
        {"MenuOpts", "Enter"},
        {"Pattern",  "Start"},
        {"Preset",   preset},
        {"Step",     step},
        {"Tour",     (cmd.find("Tour") != std::string::npos) ? 1 : 0}
    };

    char session_str[16];
    std::snprintf(session_str, sizeof(session_str), "0x%08X", d_->session);

    auto resp = d_->send_msg(
        1400,   // QCODES["OPPTZControl"]
        { {"Name",          "OPPTZControl"},
          {"SessionID",     session_str},
          {"OPPTZControl",  { {"Command",   cmd},
                              {"Parameter", ptz_param} }} }
    );

    int ret = resp.value("Ret", 101);
    return (ret == 100 || ret == 515);
}

std::string DVRIPCam::get_command(const std::string& command, int code) {
    // Code table mirrors Python QCODES
    static const std::map<std::string, int> QCODES = {
        {"KeepAlive",         1006},
        {"SystemInfo",        1020},
        {"General",           1042},
        {"OPMonitor",         1413},
        {"OPPTZControl",      1400},
        {"OPSNAP",            1560},
        {"OPTimeQuery",       1452},
        {"ChannelTitle",      1046},
        {"EncodeCapability",  1360},
        {"SystemFunction",    1360},
        {"AlarmSet",          1500},
        {"AlarmInfo",         1504},
    };

    int msg_code = code;
    if (msg_code == 0) {
        auto it = QCODES.find(command);
        msg_code = (it != QCODES.end()) ? it->second : 1042;
    }

    char session_str[16];
    std::snprintf(session_str, sizeof(session_str), "0x%08X", d_->session);

    auto resp = d_->send_msg(
        static_cast<uint16_t>(msg_code),
        { {"Name", command}, {"SessionID", session_str} }
    );

    return resp.dump();
}

std::string DVRIPCam::set_command(const std::string& command,
                                   const std::string& data_json, int code) {
    static const std::map<std::string, int> QCODES = {
        {"OPMonitor",    1413},
        {"OPPTZControl", 1400},
        {"OPSNAP",       1560},
        {"OPTimeSetting",1450},
        {"OPMachine",    1450},
        {"AlarmSet",     1500},
    };

    int msg_code = code;
    if (msg_code == 0) {
        auto it = QCODES.find(command);
        msg_code = (it != QCODES.end()) ? it->second : 1042;
    }

    char session_str[16];
    std::snprintf(session_str, sizeof(session_str), "0x%08X", d_->session);

    json data = json::parse(data_json, nullptr, false);
    if (data.is_discarded()) data = json::object();

    auto resp = d_->send_msg(
        static_cast<uint16_t>(msg_code),
        { {"Name", command}, {"SessionID", session_str}, {command, data} }
    );

    return resp.dump();
}

uint32_t DVRIPCam::session_id() const { return d_->session; }

std::string DVRIPCam::last_error() const { return d_->last_err; }

void DVRIPCam::set_log_callback(LogFn fn) {
    std::lock_guard<std::mutex> lk(d_->log_mutex);
    d_->log_fn = std::move(fn);
}

void DVRIPCam::set_bind_ip(const std::string& local_ip) {
    d_->bind_ip = local_ip;
}

// ── Helpers ────────────────────────────────────────────────────────────────────

static std::string hex_to_ip(const std::string& hex) {
    if (hex.empty()) return "";
    try {
        uint32_t v = static_cast<uint32_t>(std::stoul(hex, nullptr, 16));
        const auto* b = reinterpret_cast<const uint8_t*>(&v);
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
        return buf;
    } catch (...) { return hex; }
}

static std::string ip_to_hex(const std::string& ip) {
    unsigned int a = 0, b = 0, c = 0, d = 0;
    if (std::sscanf(ip.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return ip;
    uint32_t v = a | (b << 8) | (c << 16) | (d << 24);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08X", v);
    return buf;
}

// ════════════════════════════════════════════════════════════════════════════════
// Time
// ════════════════════════════════════════════════════════════════════════════════

bool DVRIPCam::get_time(DeviceTime& out) {
    char session_str[16];
    std::snprintf(session_str, sizeof(session_str), "0x%08X", d_->session);

    auto resp = d_->send_msg(1452,
        { {"Name", "OPTimeQuery"}, {"SessionID", session_str} });

    if (resp.empty()) return false;
    if (resp.value("Ret", 101) != 100 && resp.value("Ret", 101) != 515) return false;

    std::string ts = resp.value("OPTimeQuery", "");
    if (ts.empty()) return false;

    return std::sscanf(ts.c_str(), "%d-%d-%d %d:%d:%d",
                       &out.year, &out.month, &out.day,
                       &out.hour, &out.minute, &out.second) == 6;
}

bool DVRIPCam::set_time(const DeviceTime& dt) {
    char ts[32];
    std::snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d",
                  dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);

    char session_str[16];
    std::snprintf(session_str, sizeof(session_str), "0x%08X", d_->session);

    auto resp = d_->send_msg(1450,
        { {"Name", "OPTimeSetting"}, {"SessionID", session_str},
          {"OPTimeSetting", std::string(ts)} });

    int ret = resp.value("Ret", 101);
    return (ret == 100 || ret == 515);
}

// ════════════════════════════════════════════════════════════════════════════════
// OSD / Text Overlay
// ════════════════════════════════════════════════════════════════════════════════

bool DVRIPCam::set_channel_titles(const std::vector<std::string>& titles) {
    char session_str[16];
    std::snprintf(session_str, sizeof(session_str), "0x%08X", d_->session);

    json titles_json = json::array();
    for (const auto& t : titles)
        titles_json.push_back(t);

    auto resp = d_->send_msg(1046,
        { {"Name", "ChannelTitle"}, {"SessionID", session_str},
          {"ChannelTitle", titles_json} });

    int ret = resp.value("Ret", 101);
    return (ret == 100 || ret == 515);
}

bool DVRIPCam::set_channel_bitmap(int width, int height,
                                   const std::vector<uint8_t>& bitmap_data) {
    std::vector<uint8_t> payload(16, 0);
    payload[0] = static_cast<uint8_t>(width  & 0xFF);
    payload[1] = static_cast<uint8_t>(width  >> 8);
    payload[2] = static_cast<uint8_t>(height & 0xFF);
    payload[3] = static_cast<uint8_t>(height >> 8);
    payload.insert(payload.end(), bitmap_data.begin(), bitmap_data.end());

    auto resp = d_->send_raw_msg(0x041A, payload);
    int ret = resp.value("Ret", 101);
    return (ret == 100 || ret == 515);
}

// ════════════════════════════════════════════════════════════════════════════════
// Reboot
// ════════════════════════════════════════════════════════════════════════════════

bool DVRIPCam::reboot() {
    char session_str[16];
    std::snprintf(session_str, sizeof(session_str), "0x%08X", d_->session);

    auto resp = d_->send_msg(1450,
        { {"Name", "OPMachine"}, {"SessionID", session_str},
          {"OPMachine", { {"Action", "Reboot"} }} });

    int ret = resp.value("Ret", 101);
    return (ret == 100 || ret == 515);
}

// ════════════════════════════════════════════════════════════════════════════════
// Generic config get/set
// ════════════════════════════════════════════════════════════════════════════════

std::string DVRIPCam::get_info(const std::string& name) {
    char session_str[16];
    std::snprintf(session_str, sizeof(session_str), "0x%08X", d_->session);

    auto resp = d_->send_msg(1042,
        { {"Name", name}, {"SessionID", session_str} });

    if (resp.empty()) return "{}";
    if (resp.contains(name))
        return resp[name].dump();
    return resp.dump();
}

bool DVRIPCam::set_info(const std::string& name, const std::string& data_json) {
    json data = json::parse(data_json, nullptr, false);
    if (data.is_discarded()) data = json::object();

    char session_str[16];
    std::snprintf(session_str, sizeof(session_str), "0x%08X", d_->session);

    auto resp = d_->send_msg(1040,
        { {"Name", name}, {"SessionID", session_str}, {name, data} });

    int ret = resp.value("Ret", 101);
    return (ret == 100 || ret == 515);
}

// ════════════════════════════════════════════════════════════════════════════════
// Network settings
// ════════════════════════════════════════════════════════════════════════════════

bool DVRIPCam::get_network_info(NetworkInfo& out) {
    std::string raw = get_info("NetWork.NetCommon");
    json j = json::parse(raw, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return false;

    out.ip        = hex_to_ip(j.value("HostIP",   ""));
    out.mask      = hex_to_ip(j.value("Submask",  ""));
    out.gateway   = hex_to_ip(j.value("GateWay",  ""));
    out.dns       = hex_to_ip(j.value("DNS",       ""));
    out.hostname  = j.value("HostName", "");
    out.mac       = j.value("MAC",      "");
    out.tcp_port  = j.value("TCPPort",  34567);
    out.http_port = j.value("HttpPort", 80);
    out.dhcp      = j.value("DHCP", false);
    return true;
}

bool DVRIPCam::set_network_info(const NetworkInfo& info) {
    std::string raw = get_info("NetWork.NetCommon");
    json cfg = json::parse(raw, nullptr, false);
    if (cfg.is_discarded()) cfg = json::object();

    if (!info.ip.empty())       cfg["HostIP"]   = ip_to_hex(info.ip);
    if (!info.mask.empty())     cfg["Submask"]  = ip_to_hex(info.mask);
    if (!info.gateway.empty())  cfg["GateWay"]  = ip_to_hex(info.gateway);
    if (!info.dns.empty())      cfg["DNS"]      = ip_to_hex(info.dns);
    if (!info.hostname.empty()) cfg["HostName"] = info.hostname;
    if (info.tcp_port  > 0)     cfg["TCPPort"]  = info.tcp_port;
    if (info.http_port > 0)     cfg["HttpPort"] = info.http_port;
    cfg["DHCP"] = info.dhcp;

    return set_info("NetWork.NetCommon", cfg.dump());
}

// ════════════════════════════════════════════════════════════════════════════════
// Device discovery
// ════════════════════════════════════════════════════════════════════════════════

std::vector<DVRIPCam::DiscoveredDevice> DVRIPCam::discover(int timeout_ms,
                                                            const std::string& bind_ip) {
    platform_net_init();

    PlatformSocket sock = platform_udp_socket();
    if (sock == INVALID_PLATFORM_SOCKET)
        return {};

    platform_set_reuseaddr(sock);

    const std::string bind_addr = bind_ip.empty() ? "0.0.0.0" : bind_ip;
    platform_bind(sock, bind_addr, 34569);

    uint8_t pkt[20] = {};
    pkt[0]  = 0xFF;
    pkt[14] = 0xFA;
    pkt[15] = 0x05;

    {
        int bcast = 1;
        platform_setsockopt(sock, "SO_BROADCAST", &bcast, sizeof(bcast));
    }

    platform_sendto(sock, "255.255.255.255", 34569, pkt, sizeof(pkt));

    std::vector<DiscoveredDevice> result;
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(timeout_ms);

    while (std::chrono::steady_clock::now() < deadline) {
        int remaining_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count());
        if (remaining_ms <= 0) break;

        uint8_t buf[2048] = {};
        std::string src_ip;
        uint16_t    src_port = 0;
        int n = platform_recvfrom(sock, buf, static_cast<int>(sizeof(buf)),
                                  remaining_ms, src_ip, src_port);
        if (n < 20) continue;

        DVRIPHeader rhdr;
        std::memcpy(&rhdr, buf, sizeof(rhdr));
        if (rhdr.head != 0xFF) continue;
        if (rhdr.msg_id != 1531) continue;
        if (rhdr.data_len == 0) continue;
        if (static_cast<int>(sizeof(DVRIPHeader) + rhdr.data_len) > n) continue;

        std::string js(reinterpret_cast<const char*>(buf + sizeof(DVRIPHeader)),
                       rhdr.data_len);
        for (auto& ch : js) if (ch == '\0') ch = ' ';

        json j = json::parse(js, nullptr, false);
        if (j.is_discarded()) continue;

        json net = j.value("NetWork.NetCommon", json::object());
        if (!net.is_object()) continue;

        std::string mac = net.value("MAC", "");
        bool seen = false;
        for (const auto& d : result) if (d.mac == mac) { seen = true; break; }
        if (seen) continue;

        DiscoveredDevice dev;
        dev.ip        = hex_to_ip(net.value("HostIP",   ""));
        dev.mac       = mac;
        dev.hostname  = net.value("HostName", "");
        dev.sn        = net.value("SN",       "");
        dev.tcp_port  = net.value("TCPPort",  34567);
        dev.http_port = net.value("HttpPort", 80);
        result.push_back(std::move(dev));
    }

    platform_close(sock);
    return result;
}

// ════════════════════════════════════════════════════════════════════════════════
// Encoding settings  (Simplify.Encode)
// ════════════════════════════════════════════════════════════════════════════════

static void parse_stream_format(const json& j, DVRIPCam::VideoStreamFormat& f) {
    if (!j.is_object()) return;
    f.audio_en  = j.value("AudioEnable", false);
    f.video_en  = j.value("VideoEnable", true);
    const json& v = j.value("Video", json::object());
    f.compression  = v.value("Compression",    "H.264");
    f.resolution   = v.value("Resolution",     "");
    f.bitrate_ctrl = v.value("BitRateControl", "VBR");
    f.bitrate      = v.value("BitRate",  0);
    f.fps          = v.value("FPS",      25);
    f.gop          = v.value("GOP",      2);
    f.quality      = v.value("Quality",  4);
}

static json build_stream_format(const json& existing,
                                 const DVRIPCam::VideoStreamFormat& f) {
    json j = existing.is_object() ? existing : json::object();
    j["AudioEnable"] = f.audio_en;
    j["VideoEnable"] = f.video_en;
    json v = j.value("Video", json::object());
    if (!f.compression.empty())  v["Compression"]    = f.compression;
    if (!f.resolution.empty())   v["Resolution"]      = f.resolution;
    if (!f.bitrate_ctrl.empty()) v["BitRateControl"]  = f.bitrate_ctrl;
    if (f.bitrate > 0)           v["BitRate"]         = f.bitrate;
    if (f.fps > 0)               v["FPS"]             = f.fps;
    if (f.gop > 0)               v["GOP"]             = f.gop;
    if (f.quality > 0)           v["Quality"]         = f.quality;
    j["Video"] = v;
    return j;
}

bool DVRIPCam::get_encode_config(EncodeConfig& out, int channel) {
    std::string raw = get_info("Simplify.Encode");
    json arr = json::parse(raw, nullptr, false);
    if (arr.is_discarded() || !arr.is_array()) return false;
    if (channel < 0 || channel >= static_cast<int>(arr.size())) return false;
    const json& ch = arr[channel];
    parse_stream_format(ch.value("MainFormat",  json::object()), out.main);
    parse_stream_format(ch.value("ExtraFormat", json::object()), out.extra);
    return true;
}

bool DVRIPCam::set_encode_config(const EncodeConfig& cfg, int channel) {
    std::string raw = get_info("Simplify.Encode");
    json arr = json::parse(raw, nullptr, false);
    if (arr.is_discarded() || !arr.is_array()) arr = json::array();
    while (static_cast<int>(arr.size()) <= channel)
        arr.push_back(json::object());

    json& ch = arr[channel];
    ch["MainFormat"]  = build_stream_format(ch.value("MainFormat",  json::object()), cfg.main);
    ch["ExtraFormat"] = build_stream_format(ch.value("ExtraFormat", json::object()), cfg.extra);

    return set_info("Simplify.Encode", arr.dump());
}

// ════════════════════════════════════════════════════════════════════════════════
// Camera / video-color parameters  (AVEnc.VideoColor)
// ════════════════════════════════════════════════════════════════════════════════

bool DVRIPCam::get_video_color(VideoColorParam& out, int channel, int time_section) {
    std::string raw = get_info("AVEnc.VideoColor");
    json arr = json::parse(raw, nullptr, false);
    // Structure: [[{...},{...}], [...]]  — outer=channel, inner=time_section
    if (arr.is_discarded() || !arr.is_array()) return false;
    if (channel < 0 || channel >= static_cast<int>(arr.size())) return false;
    const json& ch = arr[channel];
    if (!ch.is_array()) return false;
    if (time_section < 0 || time_section >= static_cast<int>(ch.size())) return false;
    const json& sec = ch[time_section];
    const json& p = sec.value("VideoColorParam", json::object());
    out.brightness   = p.value("Brightness",   50);
    out.contrast     = p.value("Contrast",     50);
    out.saturation   = p.value("Saturation",   50);
    out.hue          = p.value("Hue",          50);
    out.sharpness    = p.value("Acutance",      0);
    out.gain         = p.value("Gain",          0);
    out.whitebalance = p.value("Whitebalance", 128);
    return true;
}

bool DVRIPCam::set_video_color(const VideoColorParam& p, int channel, int time_section) {
    std::string raw = get_info("AVEnc.VideoColor");
    json arr = json::parse(raw, nullptr, false);
    if (arr.is_discarded() || !arr.is_array()) return false;
    if (channel < 0 || channel >= static_cast<int>(arr.size())) return false;
    json& ch = arr[channel];
    if (!ch.is_array()) return false;
    if (time_section < 0 || time_section >= static_cast<int>(ch.size())) return false;
    json& sec = ch[time_section];
    json& vcp = sec["VideoColorParam"];
    vcp["Brightness"]   = p.brightness;
    vcp["Contrast"]     = p.contrast;
    vcp["Saturation"]   = p.saturation;
    vcp["Hue"]          = p.hue;
    vcp["Acutance"]     = p.sharpness;
    vcp["Gain"]         = p.gain;
    vcp["Whitebalance"] = p.whitebalance;
    return set_info("AVEnc.VideoColor", arr.dump());
}

} // namespace cppdvr
