# DVR Protocol Feature Status

Reference: Python implementation in `python-dvr-master/` (XiongMai/Sofia DVRIP protocol).  
C++ implementation read from current source tree as of 2026-05-09.

---

## Summary Table

| Feature | C++ Status | C API | Python ref | Msg ID |
|---|---|---|---|---|
| Login / Authentication | **Complete** | `dvr_login` | Yes | 1000 |
| Keep-Alive (heartbeat) | **Complete** | — | Yes | 1006 |
| Live Video Streaming | **Complete** | `dvr_start_monitor` | Yes | 1413 |
| Snapshot (JPEG) | **Complete** | `dvr_snapshot` | Yes | 1560 |
| PTZ Control (all directions, zoom, focus, iris, preset, tour) | **Complete** | `dvr_ptz` | Yes | 1400 |
| Recording (MP4 stream-copy / re-encode / MJPEG) | **Complete** | `recorder_*` | No | — |
| UDP/XRRO Controller Link | **Complete** | `udp_*` | No | — |
| Generic get/set command interface | **Complete** | `dvr_get_info` / `dvr_set_info` | Yes | 1042/1040 |
| Error handling / Logging callbacks | **Complete** | — | Yes | — |
| Platform abstraction layer | **Complete** | N/A | N/A | — |
| Time get / set | **Complete** | `dvr_get_time` / `dvr_set_time` | Yes | 1452/1450 |
| OSD / Text Overlay (channel title) | **Complete** | `dvr_set_channel_titles` | Yes | 1046 |
| Bitmap / Logo Overlay | **Complete** | `dvr_set_channel_bitmap` | Yes | 0x041A |
| DVR Network settings (read + write) | **Complete** | `dvr_get_network_info` / `dvr_set_network_info` | Yes | 1042/1040 |
| Device discovery (UDP broadcast) | **Complete** | `dvr_discover` | Yes | 1530/1531 |
| Reboot | **Complete** | `dvr_reboot` | Yes | 1450 |
| System Info / General config (query only) | **Partial** | `dvr_get_info` | Yes | 1020/1042 |
| Alarm query/set | **Partial** | `dvr_get_info` / `dvr_set_info` | Yes | 1500/1504 |
| Alarm event push callbacks | **Not implemented** | — | Yes | 1504/1506 |
| Motion detection events | **Not implemented** | — | Yes | 1504 |
| User management | **Not implemented** | — | Yes | 1470/1472/1474 |
| Video color / camera parameters | **Complete** | `dvr_get_video_color` / `dvr_set_video_color` | Yes | — |
| Encoding settings read/write | **Complete** | `dvr_get_encode_config` / `dvr_set_encode_config` | Yes | 1360 |
| File query / download | **Not implemented** | — | Yes | 1440 |
| Video playback | **Not implemented** | — | Yes | 1420/1424 |
| Firmware upgrade | **Not implemented** | — | Yes | 0x5F5 |
| Keyboard / IR remote emulation | **Not implemented** | — | Yes | 1550 |
| Multi-channel support | **Not implemented** | — | Yes | — |
| Email / notification test | **Not implemented** | — | Yes | 1636 |

---

## File Map (current)

| File | Role |
|---|---|
| [include/dvrip.h](../include/dvrip.h) | `DVRIPCam` class declaration (unchanged) |
| [src/dvrip.cpp](../src/dvrip.cpp) | DVRIP protocol — now uses platform abstraction layer |
| [include/stream_server.h](../include/stream_server.h) | `StreamServer` declaration |
| [src/stream_server.cpp](../src/stream_server.cpp) | ffmpeg pipeline + HTTP MJPEG server |
| [include/video_recorder.h](../include/video_recorder.h) | `VideoRecorder` declaration |
| [src/video_recorder.cpp](../src/video_recorder.cpp) | MP4/MJPEG recording via ffmpeg |
| [include/udp_stream_server.h](../include/udp_stream_server.h) | `UdpStreamServer` declaration |
| [src/udp_stream_server.cpp](../src/udp_stream_server.cpp) | XRRO UDP bidirectional link |
| [include/cppdvr_api.h](../include/cppdvr_api.h) | Pure-C DLL export declarations |
| [src/cppdvr_api.cpp](../src/cppdvr_api.cpp) | C API wrappers (split out from dllmain in latest refactor) |
| [src/dllmain.cpp](../src/dllmain.cpp) | Windows DLL entry point only (stripped down) |
| [src/platform/platform_net.h](../src/platform/platform_net.h) | Cross-platform socket abstraction |
| [src/platform/platform_crypto.h](../src/platform/platform_crypto.h) | Cross-platform MD5 abstraction |
| [src/platform/platform_process.h](../src/platform/platform_process.h) | Cross-platform process abstraction |
| [src/platform/windows/](../src/platform/windows/) | Windows implementations (Winsock2, CryptoAPI) |
| [src/platform/posix/](../src/platform/posix/) | POSIX implementations (BSD sockets, OpenSSL) |

---

## Implemented Features (Complete)

### 1. Login / Authentication
- **Files:** [src/dvrip.cpp](../src/dvrip.cpp) lines 577–609
- `DVRIPCam::login()` — sends msg ID 1000 with EncryptType="MD5", LoginType="DVRIP-Web"
- `DVRIPCam::sofia_hash()` — MD5(password) → pair even/odd nibbles → sum mod 62 → 8-char alphanumeric
- MD5 now delegated to `platform_md5()` (Windows: CryptoAPI; POSIX: OpenSSL)
- Returns session ID (`"0x1234ABCD"`) and `AliveInterval`; starts keep-alive thread

### 2. Keep-Alive / Session Management
- **Files:** [src/dvrip.cpp](../src/dvrip.cpp) lines 370–397
- Background thread sends msg ID 1006 every `AliveInterval` seconds (default 20 s)
- Sleeps in 1-second increments for fast exit; closes socket if keep-alive is rejected
- Thread is paused before the monitor loop takes over the socket

### 3. Live Video Streaming
- **Files:** [src/dvrip.cpp](../src/dvrip.cpp), [src/stream_server.cpp](../src/stream_server.cpp)
- `start_monitor(callback, stream)` — msg ID 1413 (OPMonitor) Claim + Start
- Supports "Main" and "Extra" stream types; runs on a background thread
- Multi-packet frame reassembly: I-frame (0x1FC), P-frame (0x1FD), JPEG (0x1FE), Audio (0x1FA), Info (0x1F9)
- Codec detected from bitstream media byte (0/2=H264, 1=MPEG4, 3=H265; unknown bytes default to H264)
- **Tested 2026-05-09**: 9/9 pass — StreamServer+ffmpeg pipeline verified; 271 KB JPEG captured (FF D8…FF D9); 151 decoded frames in 5 s
- `StreamServer` wraps the above in a 3-thread pipeline: camera → ffmpeg → HTTP MJPEG
  - Endpoints: `GET /` (HTML), `GET /stream` (multipart MJPEG), `GET /snapshot` (JPEG)
  - Auto-detects NIC via SIO_ROUTING_INTERFACE_QUERY (Windows) for multi-NIC hosts
  - Configurable JPEG quality (`jpeg_quality`) and output scale (`jpeg_scale_w/h`)

### 4. Snapshot (JPEG)
- **Files:** [src/dvrip.cpp](../src/dvrip.cpp) lines 648–664
- `snapshot(channel)` — msg ID 1560 (OPSNAP), reassembles binary JPEG payload
- C API: `dvr_snapshot()` returns heap-allocated buffer; caller frees with `dvr_free_buffer()`

### 5. PTZ Control
- **Files:** [src/dvrip.cpp](../src/dvrip.cpp) lines 666–690
- `ptz(cmd, step, preset, channel)` — msg ID 1400 (OPPTZControl)
- `cmd` is passed directly into the JSON payload, so any command string the device supports is forwarded as-is
- Documented commands: `DirectionUp`, `DirectionDown`, `DirectionLeft`, `DirectionRight`, `DirectionLeftUp`, `DirectionLeftDown`, `DirectionRightUp`, `DirectionRightDown`, `ZoomTile`, `ZoomWide`, `FocusNear`, `FocusFar`, `IrisSmall`, `IrisLarge`, `SetPreset`, `GotoPreset`, `ClearPreset`, `StartTour`, `StopTour`
- **Note**: Hardware test camera has no PTZ actuator connected — commands send but physical motion not verifiable

### 6. Recording (MP4 / MJPEG)
- **Files:** [src/video_recorder.cpp](../src/video_recorder.cpp), [include/video_recorder.h](../include/video_recorder.h)
- Mode 1 — MP4 stream-copy: `-c:v copy` (lossless, `RECORDER_USE_COPY=1`)
- Mode 2 — MP4 re-encode: libx264/libx265 with CRF and preset (`RECORDER_CRF`, `RECORDER_PRESET`)
- Mode 3 — MJPEG AVI: decoded JPEG frames into MJPEG container
- NAL codec auto-detection: scans first 1 KB of I-frame for SPS/PPS/IDR NALs (H264 types 5-8, H265 types 16-23, 32-34)
- Controls: `start_recording`, `feed_raw_frame`, `feed_jpeg`, `save_recording`, `discard_recording`, `pause_recording`, `resume_recording`
- Frame buffer: 300 frames (`RECORDER_BUFFER_FRAMES`); oldest dropped on overflow

### 7. UDP / XRRO Controller Link
- **Files:** [src/udp_stream_server.cpp](../src/udp_stream_server.cpp), [include/udp_stream_server.h](../include/udp_stream_server.h)
- Custom XRRO v1 binary protocol over UDP (default RX 9000, TX 9001)
- 18-byte header: `XRRO` magic + version + packet type + seq + timestamp_us
- Inbound: `0x01` InputAndGui (2 × 84B controller + 32B GUI state), `0x02` JpegChunk
- Outbound: `sendJpeg` (chunked ≤60 KB), `sendCommand` (0x03), `sendGuiUpdate` (0x04), `sendComposite` (0x05)
- Auto-discovers headset IP from first valid inbound packet
- Supports localhost mirror mode for debugging

### 8. Generic get/set Command Interface
- **Files:** [src/dvrip.cpp](../src/dvrip.cpp) lines 692–755
- `get_command(name)` — looks up message ID from QCODES table, returns raw JSON string
- `set_command(name, json_payload)` — same table, sends arbitrary JSON body
- QCODES covered: KeepAlive(1006), SystemInfo(1020), General(1042), OPMonitor(1413), OPPTZControl(1400), OPSNAP(1560), OPTimeQuery(1452), ChannelTitle(1046), EncodeCapability(1360), SystemFunction(1360), AlarmSet(1500), AlarmInfo(1504), OPTimeSetting(1450), OPMachine(1450)
- Higher-level wrappers: `get_info(name)` (msg 1042) and `set_info(name, json)` (msg 1040) mirror Python `get_info`/`set_info`
- C API: `dvr_get_info()` returns a `char*` that must be freed with `dvr_free_string()`

### 11. Time Get / Set
- **Files:** [src/dvrip.cpp](../src/dvrip.cpp)
- `get_time(DeviceTime& out)` — msg ID 1452 (`OPTimeQuery`); parses `"YYYY-MM-DD HH:MM:SS"` from device response
- `set_time(const DeviceTime&)` — msg ID 1450 (`OPTimeSetting`); formats datetime string and sends it
- C API: `dvr_get_time(h, &year, &month, &day, &hour, &minute, &second)`, `dvr_set_time(h, …)`
- **Tested 2026-05-09**: 10/10 pass — device reported 2026-04-15 02:52:36; set round-trip verified

### 12. OSD / Text Overlay (Channel Title)
- **Files:** [src/dvrip.cpp](../src/dvrip.cpp)
- `set_channel_titles(vector<string> titles)` — msg ID 1046; sends JSON array `{"ChannelTitle": [...], "Name": "ChannelTitle", "SessionID": "..."}`
- Each element in the array maps to one DVR channel
- C API: `dvr_set_channel_titles(h, titles_array, count)`
- **Tested 2026-05-09**: 13/13 pass — set title "OSD TEST ACTIVE", recorded 8 s of MP4 (199 frames, 967 KB, H.265 2880×1616 @ 25 fps), verified with ffprobe and first-frame JPEG extraction (197 KB JPEG, FF D8)

### 13. Bitmap / Logo Overlay
- **Files:** [src/dvrip.cpp](../src/dvrip.cpp)
- `set_channel_bitmap(width, height, bitmap_data)` — msg ID 0x041A
- Binary payload: 16-byte header (`width` u16 LE + `height` u16 LE + 12 null bytes) followed by 1-bpp row-major bitmap
- Uses `send_raw_msg_impl` (free function taking `Impl&`) since the payload is binary, not JSON
- C API: `dvr_set_channel_bitmap(h, width, height, bitmap_data, bitmap_size)`

### 14. DVR Network Settings (Read + Write)
- **Files:** [src/dvrip.cpp](../src/dvrip.cpp)
- `get_network_info(NetworkInfo& out)` — calls `get_info("NetWork.NetCommon")`, parses hex-encoded IPs (`"0x0101A8C0"` → `"192.168.1.1"`)
- `set_network_info(const NetworkInfo& info)` — get-then-update-set pattern: reads existing config, patches only the provided fields, writes back with `set_info`; preserves unknown/undocumented device fields
- C API: `dvr_get_network_info(h, &out)`, `dvr_set_network_info(h, &info)` using `DVRNetworkInfoC` struct

### 15. Device Discovery (UDP Broadcast)
- **Files:** [src/dvrip.cpp](../src/dvrip.cpp)
- `DVRIPCam::discover(timeout_ms, bind_ip)` — static method, no connected instance required
- Sends 20-byte DVRIP discovery packet (msg ID 1530) to `255.255.255.255:34569`; collects responses (msg ID 1531) for `timeout_ms` milliseconds
- Parses `NetWork.NetCommon` from each response; deduplicates by MAC address
- Returns `vector<DiscoveredDevice>` with `{ip, mac, hostname, sn, tcp_port, http_port}`
- C API: `dvr_discover(timeout_ms, &out_arr, &out_count)`; caller frees with `dvr_free_discovered(arr)`

### 17. Encoding Settings (Read / Write)
- **Files:** [src/dvrip.cpp](../src/dvrip.cpp), [include/dvrip.h](../include/dvrip.h), [src/cppdvr_api.cpp](../src/cppdvr_api.cpp)
- `get_encode_config(EncodeConfig& out, int channel)` — reads `Simplify.Encode` via msg 1042; parses `MainFormat[0]` and `ExtraFormat[0]` → `VideoStreamFormat` (codec, resolution, bitrate_ctrl, bitrate, fps, gop, quality, audio/video enable)
- `set_encode_config(const EncodeConfig&, int channel)` — read-modify-write: fetches existing JSON, patches only the matching fields, writes back via msg 1040
- C API: `dvr_get_encode_config(h, &cfg, ch)` / `dvr_set_encode_config(h, &cfg, ch)` using `DVREncodeConfigC` / `DVRVideoStreamFormatC`
- **Tested 2026-05-09**: 19/19 pass — camera reports H.265 Main 5M 6656 kbps VBR 25 fps, Extra HD1 665 kbps VBR; round-trip write + read-back verified

### 18. Video Color / Camera Parameters
- **Files:** [src/dvrip.cpp](../src/dvrip.cpp), [include/dvrip.h](../include/dvrip.h), [src/cppdvr_api.cpp](../src/cppdvr_api.cpp)
- `get_video_color(VideoColorParam& out, int channel, int time_section)` — reads `AVEnc.VideoColor[ch][ts].VideoColorParam` via msg 1042; fields: brightness, contrast, saturation, hue, sharpness (Acutance), gain, whitebalance
- `set_video_color(const VideoColorParam&, int channel, int time_section)` — read-modify-write pattern
- C API: `dvr_get_video_color(h, &params, ch)` / `dvr_set_video_color(h, &params, ch)` using `DVRVideoColorC`
- **Tested 2026-05-09**: 13/13 pass — camera at brightness=50, contrast=50, saturation=50, hue=50, whitebalance=128; round-trip + brightness nudge +1 + restore all verified

### 16. Reboot
- **Files:** [src/dvrip.cpp](../src/dvrip.cpp)
- `reboot()` — sends `set_command("OPMachine", {{"OPMachine", {{"Action", "Reboot"}}}})` on msg ID 1450
- C API: `dvr_reboot(h)`

### 9. Error Handling / Logging
- Every component exposes `set_log_callback(fn)` / `setLogCallback(fn)`
- `DVRIPCam::last_error()` returns last socket/protocol error string
- ffmpeg stderr forwarded through log callback in recorder and pipeline threads

### 10. Platform Abstraction Layer (new in latest refactor)
- **Files:** [src/platform/](../src/platform/)
- `platform_net.h` — `PlatformSocket`, `platform_socket()`, `platform_connect()`, `platform_bind()`, `platform_send()`, `platform_recv()`, `platform_close()`, `platform_net_init()`
- `platform_crypto.h` — `platform_md5()`, `PLATFORM_MD5_DIGEST_SIZE`
- `platform_process.h` — cross-platform child process (used by ffmpeg pipeline)
- Windows backend: Winsock2 + CryptoAPI + CreateProcess
- POSIX backend: BSD sockets + OpenSSL + fork/exec (Linux/macOS)
- `dvrip.cpp`, `stream_server.cpp`, `video_recorder.cpp` now use these abstractions instead of direct Win32 calls

---

## Partial Implementations

### 17. System Info / General Config (Partial)
- **Files:** [src/dvrip.cpp](../src/dvrip.cpp) lines 692–724
- `get_command("SystemInfo")` (ID 1020) and `get_info("General")` (ID 1042) work correctly
- Response is returned as a JSON string via `get_info()` — callers must parse it themselves
- Missing: typed struct extracting firmware version, serial number, channel counts, timezone, language, video standard (PAL/NTSC)

### 18. Alarm Query / Set (Partial)
- `get_info("AlarmInfo")` (ID 1504) and `set_info("AlarmSet", json)` (ID 1500) work at the wire level
- Responses returned as raw JSON strings — **no typed parsing or convenience helpers**
- Missing: alarm event push listener thread, motion detection callbacks, threshold configuration helpers

---

## Not Implemented

### 19. Alarm Event Push Callbacks / Motion Detection
- **Python:** `setAlarm(fn)` / `alarmStart()` — dedicated listener thread, fires callback on ID 1504 push events
- Event types: MotionDetect, LossDetect, BlindDetect, HumanDetect, StorageFailure, NetAbort, …
- **C++ status:** only synchronous poll via `get_info("AlarmInfo")`; no push listener

### 20. User Management
- **Python:** `getUsers`, `addUser`, `modifyUser`, `delUser`, `changePasswd`, `getGroups`, `addGroup`, `modifyGroup`, `delGroup`, `getAuthorityList`
- Message IDs: 1470 (AuthorityList), 1472 (Users), 1474 (Groups)
- **C++ status:** not implemented; can query raw JSON via `get_info` but no typed helpers

### 21. File Query / Download / Playback
- **Python:** `list_local_files(start, end, filetype, channel)` (ID 1440, up to 511 files per page), `download_file(…)` (IDs 1420/1424), full binary reassembly
- **C++ status:** not implemented

### 22. Firmware Upgrade
- **Python:** `upgrade(filename, packetsize)` (ID 0x5F5) — chunked binary upload, progress callback, device reboots on completion
- **C++ status:** not implemented

### 23. Keyboard / IR Remote Emulation
- **Python:** `keyDown(key)`, `keyUp(key)`, `keyPress(key)`, `keyScript(keys)` (ID 1550)
- **C++ status:** not implemented

### 24. Multi-Channel Live Streaming
- Monitor loop is hardcoded to channel 0; snapshot and PTZ accept a channel parameter
- **C++ status:** partial — only snapshot and PTZ expose the channel parameter end-to-end

### 25. Email / Notification Test
- **Python:** `OPMailTest` (ID 1636)
- **C++ status:** not implemented; could be sent via `set_command` but no helper exists

---

## Architectural Notes (latest refactor)

The codebase underwent a platform abstraction refactor since the initial version:

| Before | After |
|---|---|
| Winsock2 headers in `dvrip.cpp` directly | `#include "platform/platform_net.h"` |
| `SOCKET` / `INVALID_SOCKET` | `PlatformSocket` / `INVALID_PLATFORM_SOCKET` |
| `::send()` / `::recv()` | `platform_send()` / `platform_recv()` |
| `::closesocket()` | `platform_close()` |
| Windows CryptoAPI MD5 in `dvrip.cpp` | `platform_md5()` |
| C API wrappers in `dllmain.cpp` | Moved to `cppdvr_api.cpp` |
| `dllmain.cpp` (515 lines) | `dllmain.cpp` (14 lines, DLL entry only) |

This makes the core library (dvrip, stream_server, video_recorder) compilable on Linux/macOS using the POSIX backends, with the POSIX implementations in `src/platform/posix/`.

---

## Protocol Quick Reference

| Item | Value |
|---|---|
| Default TCP port | 34567 |
| Default UDP port (protocol) | 34568 |
| Discovery UDP port (XiongMai) | 34569 |
| Binary header size | 20 bytes |
| Header magic byte | 0xFF |
| Auth algorithm | Sofia MD5 hash (8-char alphanumeric) |
| Session ID format | `"0x"` + 8 hex digits |
| Packet tail (v0) | `\x0a\x00` |
| Packet tail (v1) | `\x00` |
| Return code OK | 100 or 515 |
| XRRO UDP RX port (default) | 9000 |
| XRRO UDP TX port (default) | 9001 |
