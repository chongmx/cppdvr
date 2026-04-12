#pragma once
/**
 * udp_stream_server.h — XrRobotOperator bidirectional UDP transceiver
 *
 * Wire protocol: XRRO v1  (byte-for-byte compatible with
 *                py_sample_2/udp_transciever.py)
 *
 * ┌─────────────────────────────────────────────────────────────────────────────┐
 * │  INBOUND  (headset → PC)   received on rx_port (default 9000)              │
 * │  0x01  InputAndGui    ControllerPacketState × 2 + GuiState  (218 B total)  │
 * │  0x02  JpegChunk      chunked JPEG frame from headset camera               │
 * │                                                                             │
 * │  OUTBOUND (PC → headset)   sent to target_ip : tx_port (default 9001)      │
 * │  0x02  JpegChunk      JPEG chunks  (same format as inbound)                │
 * │  0x03  Command        CommandId (4B) + Params[8] (32B) + FParams[4] (16B)  │
 * │  0x04  GuiUpdate      State[8] (32 B)                                      │
 * │  0x05  Composite      all fields in one datagram                           │
 * └─────────────────────────────────────────────────────────────────────────────┘
 *
 * Every datagram starts with an 18-byte header:
 *   [0]  4 B  Magic  'XRRO'
 *   [4]  1 B  Version  1
 *   [5]  1 B  PacketType
 *   [6]  4 B  SequenceNumber (uint32-LE)
 *  [10]  8 B  TimestampUs   (uint64-LE, µs monotonic)
 *  [18]  ?    Payload
 *
 * Typical lifecycle:
 *   UdpStreamServer srv;
 *   srv.setLocalIP("0.0.0.0");          // bind NIC (default: all interfaces)
 *   srv.setTargetIP("192.168.x.x");     // headset IP (omit for auto-discover)
 *   srv.setRXPort(9000);                // we listen here
 *   srv.setTXPort(9001);                // headset listens here
 *   srv.setLocalhostDebug(true);        // also mirror TX to 127.0.0.1:rx_port
 *   srv.setOnInputCallback([](const UdpInputEvent& e){ ... });
 *   srv.setOnJpegCallback([](uint32_t id, const uint8_t* d, size_t n){ ... });
 *   srv.setLogCallback([](const char* m){ puts(m); });
 *   if (!srv.init()) { // socket bind failed // }
 *   srv.start();
 *   srv.sendJpeg(jpeg, size);
 *   srv.sendCommand(1, nullptr, fparams);
 *   srv.stop();
 *   srv.deinit();
 */

#include "cppdvr_export.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace cppdvr {

// ── Protocol sizes (all values little-endian on the wire) ──────────────────────
static constexpr size_t kUdpHeaderSize      = 18;   // magic[4]+ver[1]+type[1]+seq[4]+ts[8]
static constexpr size_t kUdpCtrlSize        = 84;   // one ControllerPacketState
static constexpr size_t kUdpGuiSize         = 32;   // GuiStatePacket  (8 × int32)
static constexpr size_t kUdpChunkHdrSize    = 16;   // FrameId[4]+Idx[2]+Total[2]+TotalSz[4]+PayloadSz[4]
static constexpr size_t kUdpCommandSize     = 52;   // CommandId[4]+Params[8×4]+FParams[4×4]
static constexpr size_t kUdpInputTotal      = kUdpHeaderSize + 2*kUdpCtrlSize + kUdpGuiSize; // 218 B
static constexpr size_t kUdpCompFixedAfterHdr = 2*kUdpCtrlSize + kUdpGuiSize + kUdpCommandSize + 4; // 256 B

// Default ports (match py_sample_2/udp_transciever.py)
static constexpr int kUdpDefaultRXPort      = 9000;
static constexpr int kUdpDefaultTXPort      = 9001;

// Max JPEG payload bytes per JpegChunk datagram (60 KB)
static constexpr int kUdpChunkMaxBytes      = 60000;

// ── Packet type identifiers ─────────────────────────────────────────────────────
enum UdpPacketType : uint8_t {
    UDP_PKT_INPUT_AND_GUI = 0x01,   // headset → PC
    UDP_PKT_JPEG_CHUNK    = 0x02,   // bidirectional
    UDP_PKT_COMMAND       = 0x03,   // PC → headset
    UDP_PKT_GUI_UPDATE    = 0x04,   // PC → headset
    UDP_PKT_COMPOSITE     = 0x05,   // PC → headset  (or bidirectional)
};

// ── One Quest controller (84 bytes on wire) ─────────────────────────────────────
// Field names / order match ControllerPacketState in UDPSocketClientService.h
struct CPPDVR_API UdpControllerState {
    // Buttons
    uint8_t  primary_button     = 0;   // A / X
    uint8_t  secondary_button   = 0;   // B / Y
    uint8_t  menu_button        = 0;
    uint8_t  thumbstick_click   = 0;

    // Trigger
    float    trigger_value      = 0.f; // [0, 1]
    uint8_t  trigger_click      = 0;

    // Grip
    float    grip_value         = 0.f; // [0, 1]
    uint8_t  grip_click         = 0;

    // Thumbstick
    float    thumbstick_x       = 0.f; // [-1, 1]
    float    thumbstick_y       = 0.f;

    // Capacitive touches
    uint8_t  primary_touch      = 0;
    uint8_t  secondary_touch    = 0;
    uint8_t  thumbstick_touch   = 0;
    uint8_t  trigger_touch      = 0;
    uint8_t  thumbrest_touch    = 0;

    uint8_t  active             = 0;   // 1 = controller tracked

    // Pose — AIM ray origin/orientation (metres, quaternion x,y,z,w)
    float    aim_pos[3]         = {};
    float    aim_ori[4]         = {};

    // Pose — GRIP (physical grip centre)
    float    grip_pos[3]        = {};
    float    grip_ori[4]        = {};
};

// ── Input event — delivered by the receive thread ───────────────────────────────
struct CPPDVR_API UdpInputEvent {
    uint32_t           seq          = 0;
    uint64_t           timestamp_us = 0;
    UdpControllerState left;        // left  controller
    UdpControllerState right;       // right controller
    int32_t            gui[8]       = {};   // GUI state slots (Slot 0 = Armed flag)
};

// ── Callback signatures ─────────────────────────────────────────────────────────
// Fired on the receive thread for each InputAndGui (0x01) or Composite (0x05).
using UdpOnInputFn = std::function<void(const UdpInputEvent&)>;

// Fired when all JpegChunk (0x02) datagrams for one frame are reassembled.
using UdpOnJpegFn  = std::function<void(uint32_t frame_id,
                                        const uint8_t* data, size_t size)>;

// Status / error messages (safe to call from any thread).
using UdpLogFn     = std::function<void(const char* msg)>;

// ── UdpStreamServer ─────────────────────────────────────────────────────────────
class CPPDVR_API UdpStreamServer {
public:
    UdpStreamServer();
    ~UdpStreamServer();

    UdpStreamServer(const UdpStreamServer&)            = delete;
    UdpStreamServer& operator=(const UdpStreamServer&) = delete;

    // ── Configuration ──────────────────────────────────────────────────────────
    // Call these before init().  Changing after init() has no effect until the
    // next init() call.

    // Local NIC IP to bind the receive socket.  "0.0.0.0" = all interfaces.
    void setLocalIP(const std::string& ip);

    // Destination IP of the headset (Quest 3).
    // Leave empty to auto-detect from the first valid incoming packet.
    void setTargetIP(const std::string& ip);

    // UDP port this PC listens on.  Default: 9000.
    void setRXPort(int port);

    // UDP port the headset listens on (we send here).  Default: 9001.
    void setTXPort(int port);

    // When enabled, every outgoing datagram is also copied to
    // 127.0.0.1:<rx_port> so py_sample_2/udp_transciever.py running
    // locally can inspect / display the outbound stream.
    // Default: false.
    void setLocalhostDebug(bool enable);

    // ── Callbacks ──────────────────────────────────────────────────────────────
    // Safe to set/replace at any time; callbacks are always invoked under a lock.
    void setOnInputCallback(UdpOnInputFn fn);
    void setOnJpegCallback(UdpOnJpegFn  fn);
    void setLogCallback(UdpLogFn fn);

    // ── Lifecycle ───────────────────────────────────────────────────────────────

    // Creates and binds the UDP socket using the current configuration.
    // Returns false if binding fails (port in use, invalid IP, etc.).
    // Must be called before start() or any send_* function.
    bool init();

    // Starts the background receive thread.
    // init() must have succeeded.  Idempotent if already started.
    bool start();

    // Signals the receive thread to exit and waits for it to join.
    // The socket stays open; outbound sends continue to work.
    void stop();

    // Closes the socket and releases all resources.
    // Calls stop() automatically if the receive thread is still running.
    void deinit();

    bool isRunning()     const;
    bool isInitialized() const;

    // ── Outbound sends (PC → headset) ──────────────────────────────────────────

    // Split jpeg into ≤60 KB chunks and send each as a JpegChunk (0x02) datagram.
    // frame_id = 0 → auto-increment internal counter.
    void sendJpeg(const uint8_t* jpeg, size_t size, uint32_t frame_id = 0);

    // Send a Command (0x03) datagram.
    //   command_id   application-defined identifier
    //   params[8]    up to 8 int32 parameters  (nullptr → all zeros)
    //   fparams[4]   up to 4 float parameters  (nullptr → all zeros)
    void sendCommand(uint32_t       command_id,
                     const int32_t  params[8]  = nullptr,
                     const float    fparams[4] = nullptr);

    // Send a GuiUpdate (0x04) datagram — 8 int32 state slots.
    // state[0] = Armed flag (0=safe, 1=armed); slots 1-7 are app-defined.
    void sendGuiUpdate(const int32_t state[8]);

    // Send a Composite (0x05) datagram — all fields in one packet.
    // Best for small JPEG thumbnails (≤60 KB); larger frames need sendJpeg().
    void sendComposite(const int32_t  gui[8]     = nullptr,
                       uint32_t       command_id = 0,
                       const int32_t  params[8]  = nullptr,
                       const float    fparams[4] = nullptr,
                       const uint8_t* jpeg        = nullptr,
                       size_t         jpeg_size   = 0);

    // ── Getters ─────────────────────────────────────────────────────────────────
    std::string localIP()  const;   // configured local IP
    std::string targetIP() const;   // configured or auto-discovered headset IP
    int         rxPort()   const;
    int         txPort()   const;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

} // namespace cppdvr
