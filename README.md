# cppdvr

`cppdvr` is a Windows-focused DVR client library and diagnostic toolkit for XiongMai DVR cameras using the DVRIP protocol.

The repository builds:
- `cppdvr.dll` — a shared library exposing both C++ and C APIs for DVR access.
- `cppdvr_demo.exe` — a diagnostic demo that connects to a DVR, receives live frames, and prints frame metadata.

## Features

- Native Windows C++ DVRIP camera client
- `DVRIPCam` C++ wrapper with:
  - login/connect/keep-alive
  - live monitor callback for H264/H265 frames
  - snapshot capture
  - PTZ control
  - raw command send/get support
- Plain C export API in `include/cppdvr_api.h`
- Optional stream server support for DVR → ffmpeg → HTTP MJPEG

## Repository Layout

- `CMakeLists.txt` — build configuration
- `src/` — library source files
- `include/` — public headers
- `demo/main.cpp` — diagnostic demo executable
- `build/` — generated Visual Studio / CMake build artifacts

## Build Requirements

- Windows
- CMake 3.20 or newer
- Visual Studio with C++ support
- Git (for fetching `nlohmann/json` during configure)
- Optional: `ffmpeg` available on `PATH` if you use streaming server functionality

## Build Instructions

Open a Developer PowerShell for Visual Studio and run:

```powershell
cd C:\VS2022\cppdvr
mkdir build
cd build
cmake -G "Visual Studio 17 2022" -A x64 ..
cmake --build . --config Release
```

The build produces:
- `build\x64\Release\cppdvr.dll`
- `build\x64\Release\cppdvr.lib`
- `build\x64\Release\cppdvr_demo.exe`

## Run the Demo

The demo connects to a DVR and prints incoming frames. Example:

```powershell
cd build\x64\Release
cppdvr_demo.exe 172.20.80.12 admin password Main
```

If you omit arguments, the demo uses defaults from `demo/main.cpp`.

## Library Usage

### C++ API

Include headers and link `cppdvr.lib`:

```cpp
#include "dvrip.h"

int main() {
    cppdvr::DVRIPCam cam("192.168.1.100", "admin", "password");
    if (!cam.login()) {
        std::cerr << cam.last_error() << std::endl;
        return 1;
    }

    cam.set_log_callback([](const char* msg){
        std::cout << "DVR LOG: " << msg << std::endl;
    });

    cam.start_monitor([](const uint8_t* data, size_t size, const cppdvr::FrameMeta& meta){
        std::cout << "frame=" << meta.frame << " type=" << meta.type << " size=" << size << std::endl;
    });
    // ...
    cam.stop_monitor();
    return 0;
}
```

### C API

Use `include/cppdvr_api.h` and call the exported functions from C or other languages that can load a Windows DLL.

```c
#include "cppdvr_api.h"

int main() {
    DVRHandle h = dvr_create("192.168.1.100", 34567, "admin", "password");
    if (!dvr_login(h)) {
        // handle error
    }
    // ...
    dvr_destroy(h);
    return 0;
}
```

### Stream Server

The library also includes stream server support for DVR → ffmpeg → HTTP MJPEG streaming. Use `StreamServer` from `stream_server.h` to start an HTTP endpoint.

## Notes

- The project is targeted at Windows and uses WinSock2 plus Windows CryptoAPI.
- The C API uses UTF-8 null-terminated strings and opaque handles.

## Contributing

Contributions, issues, and pull requests are welcome. For build issues, verify that Visual Studio and CMake are installed correctly, and that `ffmpeg` is on `PATH` if needed.
