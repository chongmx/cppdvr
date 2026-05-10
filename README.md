# cppdvr

C++ client library for XiongMai/Sofia IP cameras using the DVRIP protocol (TCP port 34567).  
Builds as `cppdvr.dll` on Windows and `libcppdvr.so` on Linux/macOS from the same source tree.  
Exposes both a C++ class API and a pure-C export API callable from any language that can load a DLL.

---

## Features

| Category | Capability |
|---|---|
| **Connection** | Login with Sofia MD5 auth, session keep-alive, multi-NIC bind |
| **Live streaming** | H264/H265/MPEG4 frame callbacks; HTTP MJPEG server via ffmpeg |
| **Recording** | MP4 (stream-copy or re-encode) and MJPEG AVI via ffmpeg; dual-stream (raw + overlay) |
| **Software overlay** | Push-based text; multi-box layout with word-wrap, alignment, and anchor; thread-safe double-buffer |
| **Snapshot** | JPEG snapshot capture |
| **PTZ** | All 8 directions, zoom, focus, iris, presets, tours |
| **Time** | Get and set device clock |
| **OSD** | Per-channel title text overlay; 1-bpp monochrome bitmap overlay |
| **Network** | Read and write device IP / mask / gateway / DNS / DHCP |
| **Config** | Generic `get_info` / `set_info` for any device config block |
| **Discovery** | UDP broadcast scan on port 34569 — no connected device needed |
| **Reboot** | Remote reboot |
| **UDP link** | XRRO v1 bidirectional protocol for Quest 3 VR headset |

---

## Requirements

**All platforms**
- CMake 3.20+
- C++17 compiler
- Git (fetches `nlohmann/json` automatically at configure time)
- `ffmpeg` on `PATH` at runtime (for streaming and recording features only)

**Windows**
- Visual Studio 2017+ or MinGW-w64 / w64devkit
- Winsock2, CryptoAPI — both are part of the Windows SDK, no extra install needed

**Linux / macOS**
- GCC 9+ or Clang 11+
- Optional: `libssl-dev` / `openssl-devel` for OpenSSL MD5; falls back to built-in MD5 automatically

---

## Build

### Windows (Visual Studio)

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Output: `build\Release\cppdvr.dll`, `build\Release\cppdvr.lib`

### Windows (MinGW / w64devkit)

```powershell
cmake -B build -G "Ninja"
cmake --build build --parallel
```

### Linux / macOS / WSL

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel $(nproc)
```

Output: `build/libcppdvr.so`, `build/cppdvr_demo`, `build/cppdvr_test_frame`, `build/test_rec_mp4`

### CMake options

| Option | Default | Description |
|---|---|---|
| `CPPDVR_BUILD_DEMO` | `ON` (standalone) | Build the demo executables |
| `CPPDVR_CRYPTO` | `AUTO` | Linux/macOS MD5 backend (see table below) |

`CPPDVR_CRYPTO` (Linux/macOS only):

| Value | Behaviour |
|---|---|
| `AUTO` | Use system OpenSSL if found; otherwise fall back to built-in MD5 |
| `BUILTIN` | Always use the self-contained MD5 — zero external dependencies |
| `OPENSSL` | Require system OpenSSL; prints install instructions and stops if missing |
| `FETCH` | Use system OpenSSL if found; otherwise build OpenSSL from source via FetchContent |

Install system OpenSSL if needed:
```bash
sudo apt install libssl-dev      # Debian / Ubuntu / WSL
sudo dnf install openssl-devel   # Fedora / RHEL
brew install openssl             # macOS
```

---

## Running the Demos

### Diagnostic stream demo

Connects to a DVR, starts ffmpeg, streams JPEG frames over UDP to a Quest 3, and serves an HTTP MJPEG endpoint.

```bash
# Windows
build\Release\cppdvr_demo.exe [host] [user] [password] [stream] [--run]

# Linux
./build/cppdvr_demo [host] [user] [password] [stream] [--run]
```

Defaults: host `172.20.80.12`, user `admin`, no password, stream `Main`, 30-second timed run.  
Add `--run` for continuous streaming (type `q` + Enter to quit).  
HTTP debug view: `http://localhost:8080/stream`

### Single-frame capture test

```bash
./build/cppdvr_test_frame [host] [user] [password] [stream]
```

Waits up to 15 seconds for the first JPEG frame, saves it to `test_frame.jpg`.

### MP4 recording integration test

```bash
./build/test_rec_mp4 [host] [user] [password] [stream] [output.mp4]
```

Records a short clip and prints a pass/fail result.

---

## Library Usage

### C++ API — connection and streaming

```cpp
#include "dvrip.h"
using namespace cppdvr;

DVRIPCam cam("192.168.1.100", "admin", "password");
if (!cam.login()) {
    fprintf(stderr, "%s\n", cam.last_error().c_str());
    return 1;
}

// Raw frame callback (H264/H265 NAL bytes)
cam.start_monitor([](const uint8_t* data, size_t size, const FrameMeta& m) {
    printf("%s %s-frame  %dx%d @ %d fps  %zu bytes\n",
           m.type.c_str(), m.frame.c_str(), m.width, m.height, m.fps, size);
}, "Main");

cam.stop_monitor();
cam.close();
```

### C++ API — time, OSD, network, discovery

```cpp
// Time
DVRIPCam::DeviceTime dt;
cam.get_time(dt);
printf("%04d-%02d-%02d %02d:%02d:%02d\n",
       dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
cam.set_time({2025, 6, 1, 12, 0, 0});

// PTZ (all directions + zoom + focus + iris)
cam.ptz("DirectionUp", 5);
cam.ptz("ZoomTile", 3);
cam.ptz("FocusNear", 2);
cam.ptz("SetPreset", 1, /*preset=*/1);
cam.ptz("GotoPreset", 1, 1);

// OSD
cam.set_channel_titles({"Front Door", "Backyard", "Garage"});

// Network settings
DVRIPCam::NetworkInfo net;
cam.get_network_info(net);
printf("IP: %s  mask: %s  DHCP: %s\n",
       net.ip.c_str(), net.mask.c_str(), net.dhcp ? "yes" : "no");

// Generic config (any DVRIP config block as JSON)
std::string enc = cam.get_info("Simplify.Encode");
std::string col = cam.get_info("AVEnc.VideoColor");
cam.set_info("Simplify.Encode", enc);          // round-trip example

// Device discovery — static method, no connected cam needed
auto devs = DVRIPCam::discover(2000 /*ms*/);
for (auto& d : devs)
    printf("%s  %s  port=%d  sn=%s\n",
           d.ip.c_str(), d.hostname.c_str(), d.tcp_port, d.sn.c_str());

// Snapshot and reboot
auto jpeg = cam.snapshot();                    // vector<uint8_t>
cam.reboot();
```

### C++ API — HTTP MJPEG streaming server

```cpp
#include "stream_server.h"

cppdvr::StreamServerConfig cfg;
cfg.dvr_host     = "192.168.1.100";
cfg.dvr_user     = "admin";
cfg.dvr_password = "password";
cfg.http_port    = 8080;

cppdvr::StreamServer srv(std::move(cfg));
srv.set_jpeg_callback([](const uint8_t* jpeg, size_t size) {
    // Called for every decoded JPEG frame (FF D8 ... FF D9)
});
srv.start();
// Browser: http://localhost:8080/stream
```

### C++ API — recording

```cpp
#include "video_recorder.h"

cppdvr::VideoRecorder rec;
rec.init(&srv);                                            // attach to StreamServer
rec.start_recording("clip.mp4", cppdvr::RecordingFormat::MP4, 25);
// ... stream runs, frames are captured automatically ...
rec.save_recording();
rec.deinit();
```

### C++ API — software text overlay

The overlay is push-based and thread-safe. Text is written to a back-buffer under a mutex; the render pipeline swaps it into a front-buffer without blocking your writer thread.

#### Single-region overlay (quick use)

```cpp
srv.overlay_set_scale(4);          // 32 px glyphs (8 px × scale 4)
srv.overlay_set_cursor(-1, -1);    // auto: bottom-left with proportional margin
srv.overlay_print("CGS Venture Inc.\n12.34 m");   // '\n' = new line
srv.overlay_clear();               // remove text
```

#### Multi-box overlay (word-wrap, alignment, anchor)

Up to `StreamServer::kOverlayMaxBoxes` (default 4, override with `-DSTREAM_OVERLAY_MAX_BOXES=N`) independent boxes can be configured:

```cpp
using Align  = cppdvr::StreamServer::OverlayAlign;
using Anchor = cppdvr::StreamServer::OverlayAnchor;

// Box 0 — company name, top-right, right-aligned
srv.overlay_box_configure(0,
    /*x_pad=*/16, /*y_pad=*/16,
    /*box_w=*/700,
    /*scale=*/4,
    Align::Right,
    Anchor::TopRight);
srv.overlay_box_print(0, "CGS Venture Inc.");

// Box 1 — live reading updated from any thread at any rate
srv.overlay_box_configure(1, 16, 56, 700, 4, Align::Right, Anchor::TopRight);
srv.overlay_box_print(1, "0.00 m");

// Box 2 — address; long text word-wraps within box_w=700 px
srv.overlay_box_configure(2, 16, 96, 700, 4, Align::Right, Anchor::TopRight);
srv.overlay_box_print(2, "Bandar Puteri, Puchong, Selangor");

// Later, from any thread (e.g. sensor callback):
char buf[64];
snprintf(buf, sizeof(buf), "%.2f m", distance);
srv.overlay_box_print(1, buf);         // atomic double-buffer swap
```

#### Dual recording (raw + overlay streams simultaneously)

```cpp
// GUI display — uses the standard JPEG callback (no overlay burned in)
srv.set_jpeg_callback([](const uint8_t* jpeg, size_t size) { /* display */ });

// Overlay recording — uses a dedicated post-overlay JPEG callback
srv.set_overlay_jpeg_callback([](const uint8_t* jpeg, size_t size) {
    // feed to VideoRecorder or any other sink
});
```

### C API

```c
#include "cppdvr_api.h"

// ── Connection ─────────────────────────────────────────────────────────────────
DVRHandle h = dvr_create("192.168.1.100", 0, "admin", "password");
if (!dvr_login(h)) { dvr_destroy(h); return 1; }

// ── Streaming ──────────────────────────────────────────────────────────────────
dvr_start_monitor(h, my_frame_cb, NULL, "Main");
dvr_stop_monitor(h);

size_t sz;
uint8_t* jpeg = dvr_snapshot(h, &sz, 0);
dvr_free_buffer(jpeg);

// ── PTZ ───────────────────────────────────────────────────────────────────────
dvr_ptz(h, "DirectionUp", 5, -1, 0);
dvr_ptz(h, "ZoomTile", 3, -1, 0);
dvr_ptz(h, "FocusNear", 2, -1, 0);

// ── Time ──────────────────────────────────────────────────────────────────────
int y, mo, d, hr, mi, sec;
dvr_get_time(h, &y, &mo, &d, &hr, &mi, &sec);
dvr_set_time(h, 2025, 6, 1, 12, 0, 0);

// ── OSD ───────────────────────────────────────────────────────────────────────
const char* titles[] = { "Front Door", "Backyard" };
dvr_set_channel_titles(h, titles, 2);

// ── Network settings ──────────────────────────────────────────────────────────
DVRNetworkInfoC net;
dvr_get_network_info(h, &net);
printf("IP: %s  DHCP: %d\n", net.ip, net.dhcp);

// ── Generic config ────────────────────────────────────────────────────────────
char* enc = dvr_get_info(h, "Simplify.Encode");   // heap-allocated JSON string
printf("%s\n", enc);
dvr_free_string(enc);

dvr_set_info(h, "Simplify.Encode", "{...}");

// ── Reboot ────────────────────────────────────────────────────────────────────
dvr_reboot(h);

// ── Device discovery ──────────────────────────────────────────────────────────
DVRDiscoveredDeviceC* devs;
int count;
dvr_discover(2000, &devs, &count);
for (int i = 0; i < count; i++)
    printf("%s  %s  port=%d\n", devs[i].ip, devs[i].hostname, devs[i].tcp_port);
dvr_free_discovered(devs);

dvr_destroy(h);

// ── Stream server + recorder ──────────────────────────────────────────────────
StreamHandle srv = stream_create("192.168.1.100", 0, "admin", "", 8080);
stream_set_jpeg_callback(srv, my_jpeg_cb, NULL);          // GUI display callback
stream_start(srv, "Main");

RecorderHandle rec = recorder_create();
recorder_init_with_stream(rec, srv);
recorder_start(rec, "output.mp4", RECORDER_FORMAT_MP4, 0);
// ... let it run ...
recorder_save(rec);
recorder_deinit(rec);
recorder_destroy(rec);

stream_stop(srv);
stream_destroy(srv);

// ── Software text overlay (push-based, thread-safe) ───────────────────────────
// Single-region overlay
stream_overlay_set_scale(srv, 4);           // 32 px glyphs
stream_overlay_set_cursor(srv, -1, -1);     // auto: bottom-left with margin
stream_overlay_print(srv, "CGS Venture Inc.\n12.34 m");
stream_overlay_clear(srv);

// Multi-box overlay: up to STREAM_OVERLAY_MAX_BOXES (default 4) boxes
stream_overlay_box_configure(srv,
    /*idx=*/0,
    /*x=*/16, /*y=*/16,
    /*box_w=*/700,
    /*scale=*/4,
    STREAM_OVERLAY_ALIGN_RIGHT,
    STREAM_OVERLAY_ANCHOR_TOP_RIGHT);
stream_overlay_box_print(srv, 0, "CGS Venture Inc.");

stream_overlay_box_configure(srv, 1, 16, 56, 700, 4,
    STREAM_OVERLAY_ALIGN_RIGHT, STREAM_OVERLAY_ANCHOR_TOP_RIGHT);
stream_overlay_box_print(srv, 1, "0.00 m");         // updated each sensor tick

stream_overlay_box_configure(srv, 2, 16, 96, 700, 4,
    STREAM_OVERLAY_ALIGN_RIGHT, STREAM_OVERLAY_ANCHOR_TOP_RIGHT);
stream_overlay_box_print(srv, 2, "Bandar Puteri, Puchong, Selangor");

// Dedicated post-overlay JPEG callback for recording without conflicting with GUI
stream_set_overlay_jpeg_callback(srv, my_overlay_jpeg_cb, NULL);

// ── Dual recording (raw NAL + overlay JPEG simultaneously) ────────────────────
RecorderHandle rec_raw     = recorder_create();
RecorderHandle rec_overlay = recorder_create();
recorder_init_with_stream(rec_raw, srv);         // hooks raw NAL frames
recorder_init_standalone(rec_overlay);           // fed manually via callback
stream_set_overlay_jpeg_callback(srv, overlay_jpeg_to_recorder_cb, rec_overlay);
recorder_start(rec_raw,     "raw.mp4",     RECORDER_FORMAT_MP4,   25);
recorder_start(rec_overlay, "overlay.mp4", RECORDER_FORMAT_MJPEG, 25);
```

---

## C API Quick Reference

### Camera lifecycle

| Function | Description |
|---|---|
| `dvr_create(host, port, user, password)` | Allocate a camera handle (port 0 = default 34567) |
| `dvr_login(h)` → int | Connect + authenticate + start keep-alive; returns 1 on success |
| `dvr_close(h)` | Close socket |
| `dvr_destroy(h)` | Free all resources |

### Streaming

| Function | Description |
|---|---|
| `dvr_start_monitor(h, cb, userdata, stream)` → int | Start frame callback on background thread |
| `dvr_stop_monitor(h)` | Stop loop and join thread |
| `dvr_snapshot(h, &size, channel)` → `uint8_t*` | JPEG; free with `dvr_free_buffer()` |
| `dvr_free_buffer(buf)` | Free snapshot buffer |

### PTZ

| Function | Description |
|---|---|
| `dvr_ptz(h, cmd, step, preset, channel)` → int | Send PTZ command |

PTZ commands: `DirectionUp/Down/Left/Right`, `DirectionLeftUp/LeftDown/RightUp/RightDown`,  
`ZoomTile/Wide`, `FocusNear/Far`, `IrisSmall/Large`, `SetPreset/GotoPreset/ClearPreset`, `StartTour/StopTour`

### Time

| Function | Description |
|---|---|
| `dvr_get_time(h, &y, &mo, &d, &hr, &mi, &sec)` → int | Read device clock |
| `dvr_set_time(h, y, mo, d, hr, mi, sec)` → int | Write device clock |

### OSD

| Function | Description |
|---|---|
| `dvr_set_channel_titles(h, titles[], count)` → int | Set per-channel text labels |
| `dvr_set_channel_bitmap(h, w, h, data, size)` → int | Upload 1-bpp bitmap overlay |

### Network settings

| Function | Description |
|---|---|
| `dvr_get_network_info(h, &DVRNetworkInfoC)` → int | Read IP/mask/gateway/DNS/DHCP |
| `dvr_set_network_info(h, &DVRNetworkInfoC)` → int | Write network settings |

`DVRNetworkInfoC` fields: `ip[64]`, `mask[64]`, `gateway[64]`, `dns[64]`, `hostname[128]`, `mac[32]`, `tcp_port`, `http_port`, `dhcp`

### Generic config

| Function | Description |
|---|---|
| `dvr_get_info(h, name)` → `char*` | Fetch named config block as JSON; free with `dvr_free_string()` |
| `dvr_set_info(h, name, json)` → int | Write named config block |
| `dvr_free_string(s)` | Free string returned by `dvr_get_info` |

Common config names: `"Camera"`, `"Simplify.Encode"`, `"AVEnc.VideoColor"`, `"NetWork.NetCommon"`, `"SystemInfo"`, `"General"`

### Reboot and discovery

| Function | Description |
|---|---|
| `dvr_reboot(h)` → int | Send remote reboot |
| `dvr_discover(timeout_ms, &arr, &count)` → int | UDP broadcast scan; fills `DVRDiscoveredDeviceC[]` |
| `dvr_free_discovered(arr)` | Free array from `dvr_discover` |

`DVRDiscoveredDeviceC` fields: `ip[64]`, `mac[32]`, `hostname[128]`, `sn[64]`, `tcp_port`, `http_port`

### Stream server — overlay

#### Constants

| Constant | Value | Description |
|---|---|---|
| `STREAM_OVERLAY_MAX_TEXT` | 512 | Maximum bytes per overlay text region (incl. null terminator) |
| `STREAM_OVERLAY_MAX_BOXES` | 4 | Maximum simultaneous text boxes (recompile with `-DSTREAM_OVERLAY_MAX_BOXES=N`) |
| `STREAM_OVERLAY_ALIGN_LEFT` | 0 | Left-align text within box_w |
| `STREAM_OVERLAY_ALIGN_RIGHT` | 1 | Right-align text within box_w |
| `STREAM_OVERLAY_ANCHOR_TOP_LEFT` | 0 | x, y measured inward from top-left corner |
| `STREAM_OVERLAY_ANCHOR_TOP_RIGHT` | 1 | x, y measured inward from top-right corner |
| `STREAM_OVERLAY_ANCHOR_BOTTOM_LEFT` | 2 | x, y measured inward from bottom-left corner |
| `STREAM_OVERLAY_ANCHOR_BOTTOM_RIGHT` | 3 | x, y measured inward from bottom-right corner |

#### Single-region overlay

| Function | Description |
|---|---|
| `stream_overlay_set_cursor(sh, x, y)` | Pixel origin for first character; `-1,-1` = auto bottom-left |
| `stream_overlay_set_scale(sh, scale)` | Glyph scale factor (1 = native 8×8 px); `0` = auto (`frame_height/400`) |
| `stream_overlay_print(sh, text)` | Replace overlay text; `'\n'` starts a new line |
| `stream_overlay_clear(sh)` | Remove overlay (no text drawn until next `stream_overlay_print`) |

#### Multi-box overlay

| Function | Description |
|---|---|
| `stream_overlay_box_configure(sh, idx, x, y, box_w, scale, align, anchor)` | Configure box `idx` (0…MAX_BOXES-1); call before printing |
| `stream_overlay_box_print(sh, idx, text)` | Set display text; `'\n'` forces line break; long lines word-wrap at `box_w` |
| `stream_overlay_box_clear(sh, idx)` | Hide box `idx` |
| `stream_overlay_box_clear_all(sh)` | Hide all boxes |

Parameters for `stream_overlay_box_configure`:

| Parameter | Description |
|---|---|
| `idx` | Box index (0 to `STREAM_OVERLAY_MAX_BOXES - 1`) |
| `x`, `y` | Inward pixel offset from the anchor corner |
| `box_w` | Text-box width in pixels; controls word-wrap and right-alignment |
| `scale` | Glyph scale factor; `0` = auto |
| `align` | `STREAM_OVERLAY_ALIGN_LEFT` or `STREAM_OVERLAY_ALIGN_RIGHT` |
| `anchor` | One of the `STREAM_OVERLAY_ANCHOR_*` corner constants |

#### Overlay JPEG callback

| Function | Description |
|---|---|
| `stream_set_overlay_jpeg_callback(sh, cb, userdata)` | Fired with the post-overlay JPEG for every frame; use for recording the overlaid stream without conflicting with the GUI's `stream_set_jpeg_callback` |

### Utility

| Function | Description |
|---|---|
| `dvr_sofia_hash(password, buf, buf_len)` | Compute the XiongMai login hash (8-char alphanumeric) |

---

## Project Layout

```
cppdvr/
├── include/
│   ├── dvrip.h                  C++ DVRIPCam class
│   ├── cppdvr_api.h             Pure-C export API
│   ├── stream_server.h          HTTP MJPEG streaming server + overlay API
│   ├── video_recorder.h         MP4 / MJPEG recording
│   └── udp_stream_server.h      XRRO UDP link for Quest 3
├── src/
│   ├── dvrip.cpp                DVRIP protocol implementation
│   ├── cppdvr_api.cpp           C wrappers around C++ classes
│   ├── stream_server.cpp        ffmpeg pipeline + HTTP server + overlay pipeline
│   ├── video_recorder.cpp       MP4/MJPEG recording via ffmpeg
│   ├── udp_stream_server.cpp    XRRO v1 UDP protocol
│   ├── jpeg_overlay.h           Internal: 8×8 bitmap font, text/textbox rendering (internal only)
│   ├── jpeg_overlay.cpp         JPEG decode → draw overlay → JPEG re-encode (stb_image)
│   ├── dllmain.cpp              Windows DLL entry point
│   └── platform/
│       ├── platform_net.h       Socket abstraction interface
│       ├── platform_crypto.h    MD5 abstraction interface
│       ├── platform_process.h   Process spawning abstraction
│       ├── windows/             Winsock2, CryptoAPI, CreateProcess
│       └── posix/               BSD sockets, OpenSSL/built-in MD5, fork/exec
├── third_party/
│   └── stb/
│       ├── stb_image.h          Single-header JPEG/PNG decode (Sean Barrett)
│       └── stb_image_write.h    Single-header JPEG/PNG encode
├── demo/
│   ├── main.cpp                 DVR → ffmpeg → MJPEG + UDP Quest 3 stream
│   ├── test_frame.cpp           Frame callback diagnostics
│   └── test_rec_mp4.c           C-API recording test
├── tests/
│   └── osd/
│       ├── test_dual_record.c   Dual-stream recording with 3-box HUD overlay
│       └── test_overlay_record.c  Push-based single-region overlay recording test
├── docs/
│   ├── feature-status.md        Protocol feature implementation status
│   └── CROSS_COMPILATION_PLAN.md  Linux porting design and notes
└── CMakeLists.txt
```

---

## Using as a CMake Submodule

```bash
git submodule add <repo-url> 3rdparty/cppdvr
git submodule update --init --recursive
```

```cmake
add_subdirectory(3rdparty/cppdvr)
target_link_libraries(myapp PRIVATE cppdvr::cppdvr)
```

Demo executables default to `OFF` when included this way. Override with `-DCPPDVR_BUILD_DEMO=ON`.  
If your project already has `nlohmann_json::nlohmann_json`, cppdvr reuses it and skips the fetch.

After `cmake --install`:

```cmake
find_package(cppdvr REQUIRED)
target_link_libraries(myapp PRIVATE cppdvr::cppdvr)
```

---

## Protocol Reference

| Item | Value |
|---|---|
| DVRIP TCP port | 34567 |
| DVRIP UDP port | 34568 |
| Discovery UDP port | 34569 |
| Header | 20 bytes, magic byte `0xFF` |
| Auth algorithm | Sofia MD5 — 8-char alphanumeric |
| Session ID format | `"0x"` + 8 hex digits |
| Return code OK | 100 or 515 |
| Generic GET message ID | 1042 |
| Generic SET message ID | 1040 |

See [docs/feature-status.md](docs/feature-status.md) for the full table of implemented vs. planned protocol features.

---

## Notes

- `ffmpeg` must be in `PATH` at runtime for `StreamServer` and `VideoRecorder` to function. `DVRIPCam` and all the new protocol functions (time, OSD, network, discovery, etc.) have no ffmpeg dependency.
- The C API uses UTF-8 null-terminated strings and opaque handles. Every `*_create()` must be matched with a `*_destroy()`.
- On Windows, Winsock2 is initialised internally — callers do not need to call `WSAStartup`.
- `dvr_get_info()` and `dvr_discover()` return heap-allocated memory. Always free with `dvr_free_string()` and `dvr_free_discovered()` respectively.
