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

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincrypt.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "crypt32.lib")

#include "dvrip.h"
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
            case 1: return "mpeg4";
            case 2: return "h264";
            case 3: return "h265";
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

// ── WSA RAII guard ─────────────────────────────────────────────────────────────
struct WsaGuard {
    WsaGuard()  { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); }
    ~WsaGuard() { WSACleanup(); }
};
static WsaGuard g_wsa;   // initialised at load time

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
    SOCKET      sock{INVALID_SOCKET};

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
        if (sock == INVALID_SOCKET) return false;
        int sent = 0;
        while (static_cast<size_t>(sent) < len) {
            int r = ::send(sock, reinterpret_cast<const char*>(data) + sent,
                           static_cast<int>(len - sent), 0);
            if (r == SOCKET_ERROR) return false;
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
            int r = ::recv(sock,
                           reinterpret_cast<char*>(buf.data()) + received,
                           static_cast<int>(len - received), 0);
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
        if (sock == INVALID_SOCKET) return { {"Ret", 101} };

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
            if (sock == INVALID_SOCKET) break;

            char session_str[16];
            std::snprintf(session_str, sizeof(session_str),
                          "0x%08X", session);

            auto ret = send_msg(
                1006,  // QCODES["KeepAlive"]
                { {"Name", "KeepAlive"}, {"SessionID", session_str} }
            );

            if (ret.empty() || ret.value("Ret", 101) == 101) {
                last_err = "keep-alive failed — closing";
                if (sock != INVALID_SOCKET) {
                    ::closesocket(sock);
                    sock = INVALID_SOCKET;
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
    uint8_t  md5[16] = {};
    DWORD    hashLen  = 16;
    HCRYPTPROV hProv  = 0;
    HCRYPTHASH hHash  = 0;

    if (CryptAcquireContextW(&hProv, nullptr, nullptr,
                              PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        if (CryptCreateHash(hProv, CALG_MD5, 0, 0, &hHash)) {
            if (!password.empty())
                CryptHashData(hHash,
                              reinterpret_cast<const BYTE*>(password.data()),
                              static_cast<DWORD>(password.size()), 0);
            CryptGetHashParam(hHash, HP_HASHVAL, md5, &hashLen, 0);
            CryptDestroyHash(hHash);
        }
        CryptReleaseContext(hProv, 0);
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

    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        d_->last_err = "socket() failed";
        return false;
    }

    // Set send/recv timeout
    DWORD ms = static_cast<DWORD>(timeout_sec * 1000);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));

    // Bind to a specific local IP when the host has multiple NICs.
    // Without this, Windows may route through the wrong interface (e.g., WiFi
    // instead of the wired adapter that's on the same subnet as the camera).
    if (!d_->bind_ip.empty()) {
        sockaddr_in local{};
        local.sin_family      = AF_INET;
        local.sin_port        = 0;  // OS assigns ephemeral port
        inet_pton(AF_INET, d_->bind_ip.c_str(), &local.sin_addr);
        if (::bind(s, reinterpret_cast<sockaddr*>(&local), sizeof(local))
                == SOCKET_ERROR) {
            ::closesocket(s);
            d_->last_err = "bind() to local IP " + d_->bind_ip + " failed";
            return false;
        }
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<u_short>(d_->port));
    inet_pton(AF_INET, d_->ip.c_str(), &addr.sin_addr);

    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr))
            == SOCKET_ERROR) {
        ::closesocket(s);
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

    if (d_->sock != INVALID_SOCKET) {
        ::closesocket(d_->sock);
        d_->sock = INVALID_SOCKET;
    }
}

bool DVRIPCam::login() {
    if (d_->sock == INVALID_SOCKET)
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

bool DVRIPCam::is_connected()  const { return d_->sock != INVALID_SOCKET; }
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
    if (d_->sock != INVALID_SOCKET) {
        ::shutdown(d_->sock, SD_BOTH);
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

} // namespace cppdvr
