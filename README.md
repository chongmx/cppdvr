# cppdvr

C++ client library for XiongMai DVR cameras using the DVRIP protocol. Builds as a shared library (`cppdvr.dll` on Windows, `libcppdvr.so` on Linux) with both C++ and pure-C APIs.

## Features

- **DVR camera client** — login, keep-alive, live H264/H265 monitor, JPEG snapshot, PTZ control
- **Stream server** — DVR → ffmpeg → real JPEG frames → HTTP MJPEG endpoint (browser-viewable)
- **Video recorder** — record to MP4 (lossless passthrough) or MJPEG AVI via ffmpeg
- **UDP stream server** — send JPEG frames to a Meta Quest headset; receive controller input (XiongMai XRRO v1 protocol)
- **Pure-C export API** (`include/cppdvr_api.h`) — callable from C, Python ctypes, P/Invoke, Unity, etc.
- **Cross-platform** — Windows (`.dll`) and Linux / WSL (`.so`) from the same source tree

## Repository Layout

```
include/            Public headers (C++ and C API)
  cppdvr_api.h      Pure-C export API
  dvrip.h           DVRIPCam C++ class
  stream_server.h   StreamServer C++ class
  video_recorder.h  VideoRecorder C++ class
  udp_stream_server.h
src/
  cppdvr_api.cpp    C API wrappers (cross-platform)
  dvrip.cpp
  stream_server.cpp
  video_recorder.cpp
  udp_stream_server.cpp
  dllmain.cpp       Windows DLL entry point only
  platform/
    platform_net.h / platform_process.h / platform_crypto.h
    windows/        Winsock2 + CryptoAPI + CreateProcess implementations
    posix/          BSD sockets + MD5 + fork/exec implementations
demo/
  main.cpp          DVR → ffmpeg → JPEG → UDP stream diagnostic tool
  test_frame.cpp    Capture one JPEG frame and save to disk
  test_rec_mp4.c    Integration test: record a short MP4 from a live stream
CMakeLists.txt
```

## Build Requirements

**All platforms**
- CMake 3.20+
- C++17 compiler
- Git (fetches `nlohmann/json` at configure time)
- `ffmpeg` on `PATH` at runtime (for streaming and recording features)

**Windows**
- MinGW-w64 / w64devkit, or MSVC / Visual Studio

**Linux / WSL**
- GCC or Clang
- `make` or `ninja`
- No system libraries required by default (OpenSSL is optional, see below)

## Building

### Windows (MinGW / w64devkit)

```powershell
cmake -B build -G "Ninja"
cmake --build build --parallel
```

### Windows (Visual Studio)

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

### Linux / WSL

```bash
cmake -B build
cmake --build build --parallel $(nproc)
```

Output: `build/libcppdvr.so`, `build/cppdvr_demo`, `build/cppdvr_test_frame`, `build/test_rec_mp4`

## Crypto Backend (Linux only)

MD5 is used for DVRIP authentication. The build selects an implementation automatically; override with `-DCPPDVR_CRYPTO=<value>`:

| Value | Behaviour |
|---|---|
| `AUTO` **(default)** | Use system OpenSSL if installed, otherwise fall back to built-in |
| `BUILTIN` | Always use the self-contained MD5 implementation (no dependencies) |
| `OPENSSL` | Require system OpenSSL — prints install instructions and stops if missing |
| `FETCH` | Use system OpenSSL if installed, otherwise clone and build it from source |

Install system OpenSSL if needed:
```bash
sudo apt install libssl-dev      # Debian / Ubuntu / WSL
sudo dnf install openssl-devel   # Fedora / RHEL
brew install openssl             # macOS
```

## Running the Demos

### Diagnostic stream demo

Connects to a DVR, starts ffmpeg, streams JPEG frames over UDP, and serves an HTTP MJPEG endpoint.

```bash
# Windows
build\cppdvr_demo.exe [host] [user] [password] [stream] [--run]

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

Waits up to 15 seconds for the first JPEG frame, saves it to `test_frame.jpg`, and exits 0 on success.

### MP4 recording integration test

```bash
./build/test_rec_mp4 [host] [user] [password] [stream] [output.mp4]
```

Records a short clip, verifies the file was written, and prints a pass/fail result.

## Library Usage

### C++ API

```cpp
#include "dvrip.h"
#include "stream_server.h"

// DVR camera
cppdvr::DVRIPCam cam("192.168.1.100", "admin", "password");
cam.login();
cam.start_monitor([](const uint8_t* data, size_t size, const cppdvr::FrameMeta& m) {
    // raw H264/H265 NAL bytes
}, "Main");
cam.stop_monitor();

// Stream server (DVR → ffmpeg → JPEG)
cppdvr::StreamServerConfig cfg;
cfg.dvr_host = "192.168.1.100";
cfg.dvr_user = "admin";
cfg.http_port = 8080;

cppdvr::StreamServer srv(cfg);
srv.set_jpeg_callback([](const uint8_t* jpeg, size_t size) {
    // real JPEG bytes, FF D8 ... FF D9
});
srv.start();
```

### C API

```c
#include "cppdvr_api.h"

// DVR
DVRHandle cam = dvr_create("192.168.1.100", 0, "admin", "");
dvr_login(cam);
dvr_start_monitor(cam, my_frame_cb, NULL, "Main");
dvr_stop_monitor(cam);
dvr_destroy(cam);

// Stream server
StreamHandle srv = stream_create("192.168.1.100", 0, "admin", "", 8080);
stream_set_jpeg_callback(srv, my_jpeg_cb, NULL);
stream_start(srv, "Main");
// ...
stream_stop(srv);
stream_destroy(srv);

// Recorder
RecorderHandle rec = recorder_create();
recorder_init_with_stream(rec, srv);
recorder_start(rec, "output.mp4", RECORDER_FORMAT_MP4, 0);
// ... stream runs, frames are recorded automatically ...
recorder_save(rec);
recorder_deinit(rec);
recorder_destroy(rec);
```

See [`include/cppdvr_api.h`](include/cppdvr_api.h) for the full API reference.

## Notes

- `ffmpeg` must be on `PATH` at runtime for `StreamServer` and `VideoRecorder` to function.
- The C API uses UTF-8 null-terminated strings and opaque handles. Every `*_create()` must be matched with a `*_destroy()`.
- On Windows, Winsock2 initialisation is handled internally — callers do not need to call `WSAStartup`.
