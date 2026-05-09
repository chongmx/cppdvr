# Cross-Compilation: cppdvr Windows + Linux Shared Library

**Status**: **Implemented**  
**Target Platform**: Linux (Ubuntu 22.04 LTS+) + Windows  
**Build Type**: Dual-platform (Windows + Linux from single source)  
**Created**: May 3, 2026 | **Completed**: May 9, 2026

---

## Executive Summary

This document describes the platform abstraction strategy used to make cppdvr cross-compilable for Linux as a shared library (`.so`). The implementation is complete — both Windows and Linux build from the same source tree.

### Implemented State
- **Windows**: Winsock2 (networking), CryptoAPI (MD5), CreateProcess (ffmpeg spawn)
- **Linux/macOS**: POSIX sockets, OpenSSL or built-in MD5, fork/exec
- **Export Formats**: `.dll` (Windows) + `.so` (Linux/macOS)
- **APIs**: Both C++ and C APIs work identically on both platforms

---

## Architecture Overview

### Platform Abstraction Strategy

Create a platform abstraction layer in `src/platform/` with three core modules:

```
src/platform/
├── platform_net.h           (Network abstraction interface)
├── platform_crypto.h        (Crypto abstraction interface)
├── platform_process.h       (Process management abstraction)
├── windows/
│   ├── platform_net.cpp     (Winsock2 implementation)
│   ├── platform_crypto.cpp  (CryptoAPI implementation)
│   └── platform_process.cpp (CreateProcess implementation)
└── posix/
    ├── platform_net.cpp     (POSIX sockets implementation)
    ├── platform_crypto.cpp  (OpenSSL implementation)
    └── platform_process.cpp (fork/exec implementation)
```

**Design Principles**:
- C-style interface to minimize overhead (no virtual classes)
- Platform-agnostic function signatures
- Conditional compilation at build time (not runtime)
- Isolated OS-specific code for maintainability

### Dependency Mapping

| Functionality | Windows | Linux | Abstraction |
|---|---|---|---|
| Networking | Winsock2 | POSIX sockets | platform_net.h |
| MD5 Hash | CryptoAPI | OpenSSL EVP | platform_crypto.h |
| Process Spawn | CreateProcess | fork/exec | platform_process.h |
| Threading | std::thread | std::thread | ✓ Already portable |
| JSON | nlohmann/json | nlohmann/json | ✓ Already portable |

---

## Implementation Plan

### Phase 1: Platform Abstraction Foundation

**Objective**: Create cross-platform interfaces before implementation

**Steps**:

1. **Create Platform Abstraction Headers**
   - `src/platform/platform_net.h`
     - Function declarations: `platform_socket()`, `platform_connect()`, `platform_send()`, `platform_recv()`, `platform_setsockopt()`, `platform_get_local_ip_for_remote()`, `platform_net_init()`, `platform_net_cleanup()`
     - Socket handle typedef (abstracted SOCKET/int)
     - Error codes and constants
   
   - `src/platform/platform_crypto.h`
     - MD5 context typedef
     - Function declarations: `platform_md5_init()`, `platform_md5_update()`, `platform_md5_final()`
     - Digest size constant
   
   - `src/platform/platform_process.h`
     - Function declarations: `platform_spawn_process()`, `platform_wait_process()`, `platform_kill_process()`, `platform_read_pipe()`, `platform_write_pipe()`, `platform_close_pipe()`
     - Process handle and pipe handle typedefs
     - Return status codes

2. **Create Directory Structure**
   - `mkdir src/platform/windows`
   - `mkdir src/platform/posix`

3. **Establish Build Configuration Pattern**
   - CMake will conditionally include `windows/*.cpp` or `posix/*.cpp` based on platform

---

### Phase 2: POSIX/Linux Implementation

**Objective**: Implement Linux equivalents of Windows APIs

**2.1 platform_net_posix.cpp** (600-800 LOC)
- Replace WSAStartup/WSACleanup with no-op (POSIX sockets always available)
- Implement socket creation using POSIX `socket()`, `AF_INET`, `SOCK_STREAM`
- Implement socket options with `setsockopt()` using `SO_RCVTIMEO`/`SO_SNDTIMEO` or `select()`
- Implement multi-NIC local IP discovery using `getifaddrs()` instead of WSAIoctl
- Implement `platform_connect()` with error handling for `EINPROGRESS` and timeouts

**Key Differences**:
- Timeouts use `struct timeval` instead of milliseconds
- Multi-NIC query loops through `getifaddrs()` results
- Error values are `errno` instead of WSA error codes
- Socket closure uses `close()` instead of `closesocket()`

**2.2 platform_crypto_posix.cpp** (200-300 LOC)
- Replace CryptoAPI with OpenSSL EVP API
- Implement `MD5_CTX` wrapper around `EVP_MD_CTX`
- Implement `platform_md5_init()` → `EVP_DigestInit_ex(ctx, EVP_md5())`
- Implement `platform_md5_update()` → `EVP_DigestUpdate()`
- Implement `platform_md5_final()` → `EVP_DigestFinal_ex()`
- Use same digest size (16 bytes) and output format

**Dependency**: Link against libcrypto (`-lcrypto`)

**2.3 platform_process_posix.cpp** (400-600 LOC)
- Replace `CreateProcess()` with `fork()`/`execvp()`
- Implement `pipe()` instead of `CreatePipe()`
- Implement `dup2()` to redirect stdout/stderr
- Implement `waitpid()` for process waiting
- Implement `kill()` for process termination
- Handle zombie process cleanup

**Key Differences**:
- Use `pid_t` instead of `HANDLE`
- Use file descriptors (int) instead of `HANDLE`
- Use `std::string` and `std::vector<std::string>` instead of `LPWSTR` and `LPWSTR[]`
- Use `fork()` + `exec()` pattern instead of single CreateProcess call

---

### Phase 3: Windows Implementation (Migration)

**Objective**: Consolidate existing Windows code into platform abstraction layer

**3.1 platform_net_windows.cpp**
- Extract socket code from `src/dvrip.cpp` lines 97-102 (WsaGuard)
- Extract socket creation from `src/dvrip.cpp` line 568
- Extract setsockopt from `src/dvrip.cpp` lines 576-577
- Extract closesocket from `src/dvrip.cpp` line 617
- Keep existing error handling and timeouts

**3.2 platform_crypto_windows.cpp**
- Extract CryptoAPI code from `src/dvrip.cpp` lines 516-540
- Wrapper functions for CryptCreateHash, CryptGetHashParam, CryptDestroyHash

**3.3 platform_process_windows.cpp**
- Extract CreateProcess/CreatePipe code from `src/stream_server.cpp` (FfmpegProc struct)
- Extract similar code from `src/video_recorder.cpp`
- Wrapper functions for process spawning and pipe management

---

### Phase 4: CMakeLists.txt Updates

**Objective**: Enable conditional compilation for both platforms

**Changes**:

1. **Add Platform Detection**
   ```cmake
   # At top of CMakeLists.txt
   set(CMAKE_CXX_STANDARD 17)
   set(CMAKE_CXX_STANDARD_REQUIRED ON)
   
   if(WIN32)
       add_compile_definitions(
           WIN32_LEAN_AND_MEAN
           NOMINMAX
           _WIN32_WINNT=0x0601
           _CRT_SECURE_NO_WARNINGS
       )
   elseif(UNIX)
       add_compile_options(-fPIC)  # Position-independent code for .so
   endif()
   ```

2. **Conditionally Include Platform Sources**
   ```cmake
   if(WIN32)
       set(PLATFORM_SOURCES
           src/platform/windows/platform_net.cpp
           src/platform/windows/platform_crypto.cpp
           src/platform/windows/platform_process.cpp
           src/dllmain.cpp
       )
       set(PLATFORM_LIBS ws2_32 crypt32)
   elseif(UNIX)
       set(PLATFORM_SOURCES
           src/platform/posix/platform_net.cpp
           src/platform/posix/platform_crypto.cpp
           src/platform/posix/platform_process.cpp
       )
       find_package(OpenSSL REQUIRED)
       set(PLATFORM_LIBS OpenSSL::Crypto pthread)
   endif()
   
   add_library(cppdvr SHARED
       ${PLATFORM_SOURCES}
       src/dvrip.cpp
       src/stream_server.cpp
       src/udp_stream_server.cpp
       src/video_recorder.cpp
   )
   
   target_link_libraries(cppdvr PRIVATE ${PLATFORM_LIBS})
   target_include_directories(cppdvr PRIVATE src/platform)
   ```

3. **Export Configuration**
   - Verify `CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS` or use `CPPDVR_BUILDING_DLL` guard
   - Ensure Linux exports use `__attribute__((visibility("default")))`

---

### Phase 5: Source Code Refactoring

**Objective**: Replace Windows APIs with platform abstraction calls

#### 5.1 src/dvrip.cpp Changes

**Remove**:
- `#include <winsock2.h>`
- `#pragma comment(lib, "ws2_32.lib")`
- `#pragma comment(lib, "crypt32.lib")`
- WsaGuard struct (lines 97-102)

**Add**:
- `#include "platform/platform_net.h"`
- `#include "platform/platform_crypto.h"`

**Replace** (socket operations):
- Line 568: `::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)` → `platform_socket()`
- Lines 576-577: `setsockopt()` calls → `platform_setsockopt()`
- Line 617: `::closesocket()` → `platform_close()`
- Connection setup: Use `platform_connect()` wrapper

**Replace** (MD5 authentication):
- Lines 516-540: CryptCreateHash/CryptGetHashParam → platform_crypto functions

**Replace** (startup):
- WSAStartup/WSACleanup → `platform_net_init()` / `platform_net_cleanup()`

#### 5.2 src/stream_server.cpp Changes

**Remove**:
- `#include <winsock2.h>`
- Direct WSAIoctl calls

**Add**:
- `#include "platform/platform_net.h"`
- `#include "platform/platform_process.h"`

**Replace** (routing detection):
- Lines 51-65: WSAIoctl(SIO_ROUTING_INTERFACE_QUERY) → `platform_get_local_ip_for_remote(remote_ip, local_ip_out)`

**Replace** (ffmpeg spawning):
- FfmpegProc struct and CreateProcess logic → `platform_spawn_process()` and friends
- CreatePipe → `platform_create_pipe()`
- Thread management stays as-is (uses std::thread)

#### 5.3 src/udp_stream_server.cpp Changes

**Remove**:
- `#include <winsock2.h>` if present

**Add**:
- `#include "platform/platform_net.h"`

**Replace**:
- UDP socket creation and operations with platform_net functions

#### 5.4 src/video_recorder.cpp Changes

**Remove**:
- Direct CreateProcess/CreatePipe calls
- WCHAR and wide string handling

**Add**:
- `#include "platform/platform_process.h"`

**Replace**:
- Lines 92-170: Process spawning logic with `platform_spawn_process()` API
- Pipe management with `platform_create_pipe()`, `platform_read_pipe()`, etc.
- WCHAR paths with `std::string`

#### 5.5 src/dllmain.cpp

**Windows behavior**: Unchanged (Windows only)

**Linux behavior**: Create `src/platform/posix/dllmain_posix.cpp` with no-op or library constructor/destructor

---

### Phase 6: Verification & Testing

**Objective**: Validate builds and functionality on both platforms

#### 6.1 Build Verification

**Windows (Visual Studio 2022)**:
```bash
cd c:\cpp\cppdvr
mkdir build-win && cd build-win
cmake -G "Visual Studio 17" ..
cmake --build . --config Release
# Expected output: cppdvr.dll
```

**Linux (GCC/Clang)**:
```bash
cd /path/to/cppdvr
mkdir build-linux && cd build-linux
cmake -GNinja -DCMAKE_BUILD_TYPE=Release ..
ninja
# Expected output: libcppdvr.so
```

**Symbol Verification**:
- Windows: `dumpbin /exports cppdvr.dll` should show public API symbols
- Linux: `objdump -T libcppdvr.so | grep GLOBAL` should show public symbols

#### 6.2 Functional Testing

1. **Compile demo on both platforms**:
   - `demo/main.cpp` should compile against both `.dll` and `.so`

2. **Test network functionality** (if DVR available):
   - DVRIP login sequence
   - Frame callback delivery
   - Snapshot capture
   - PTZ control

3. **Test process spawning**:
   - ffmpeg MJPEG streaming on both Windows and Linux
   - Verify pipe communication works

4. **Test both C++ and C APIs**:
   - Link test program against C++ API
   - Link test program against C API
   - Verify both work identically on both platforms

#### 6.3 Platform-Specific Edge Cases

- **Timeouts**: Verify socket timeouts work on both (TCP read/write timeouts)
- **Multi-NIC**: Test local IP selection on machine with multiple NICs
- **Large frames**: Verify frame data > 64KB handled correctly
- **ffmpeg paths**: Test with ffmpeg in system PATH and absolute path

---

## File Changes Summary

### New Files (9 total)
```
src/platform/
├── platform_net.h                    [200-300 LOC]
├── platform_crypto.h                 [50-100 LOC]
├── platform_process.h                [100-150 LOC]
├── windows/
│   ├── platform_net.cpp              [200-300 LOC, extracted]
│   ├── platform_crypto.cpp           [100-150 LOC, extracted]
│   └── platform_process.cpp          [300-400 LOC, extracted]
└── posix/
    ├── platform_net.cpp              [600-800 LOC, new]
    ├── platform_crypto.cpp           [200-300 LOC, new]
    └── platform_process.cpp          [400-600 LOC, new]
```

### Modified Files (6 total)
- `CMakeLists.txt` — Add platform detection, OpenSSL dependency, conditional sources
- `src/dvrip.cpp` — Replace Winsock2/CryptoAPI with platform layer (~50 lines modified)
- `src/stream_server.cpp` — Replace WSAIoctl/CreateProcess with platform layer (~80 lines modified)
- `src/udp_stream_server.cpp` — Replace Winsock2 with platform layer (~20 lines modified)
- `src/video_recorder.cpp` — Replace CreateProcess with platform layer (~40 lines modified)
- `include/cppdvr_export.h` — Verify Linux export macro (minimal/no changes)

### Unchanged Files
- `include/cppdvr_api.h` — Public C API (unchanged)
- `include/dvrip.h` — DVRIP protocol (unchanged)
- `demo/main.cpp` — Demo application (unchanged)
- `demo/test_frame.cpp` — Frame test (unchanged)

---

## Dependencies & Requirements

### Windows
- **Compiler**: Visual Studio 2017+
- **Dependencies**: 
  - Winsock2 (built-in)
  - CryptoAPI (built-in)
  - Windows.h (built-in)

### Linux
- **Compiler**: GCC 9.0+, Clang 11.0+
- **Build Tools**: CMake 3.15+, Make or Ninja
- **Dependencies**:
  - libssl-dev / openssl-devel (OpenSSL headers)
  - libcrypto (shared library)
  - pthreads (usually built-in)
  - Standard POSIX socket API

### Both
- **Build System**: CMake 3.15+
- **Language**: C++17 (std::optional, std::string, std::thread)
- **Header-Only**: nlohmann/json v3.11+

---

## Decision Log

| Decision | Rationale | Alternatives Considered |
|----------|-----------|-------------------------|
| Single CMake project for both platforms | Reduces maintenance burden, one source tree | Separate Windows/Linux builds |
| C-style platform abstraction (no virtual classes) | Minimal runtime overhead | OOP with virtual base classes |
| Platform layer isolation in `src/platform/` | Simplifies code review and future porting | Scattered `#ifdef WIN32` throughout codebase |
| OpenSSL for Linux (not custom MD5) | Industry standard, well-tested, available | Implement MD5 from scratch |
| Keep dual API support (C++ and C) | Maintains backward compatibility | C-only to simplify |
| Ubuntu 22.04+ as target (not 20.04) | Modern glibc, recent toolchain | Support older Ubuntu |

---

## Risk Assessment

| Risk | Probability | Impact | Mitigation |
|------|-------------|--------|-----------|
| OpenSSL API mismatch | Low | Medium | Unit test MD5 output against known vectors |
| Socket timeout behavior difference | Medium | Medium | Test TCP timeouts across both platforms |
| Multi-NIC routing differences | Medium | Low | Fallback to interface enumeration |
| ffmpeg spawn issues on Linux | Low | Medium | Test with system PATH and absolute paths |
| ABI compatibility between Windows/Linux | Low | High | Verify symbol exports with nm/objdump |

---

## Timeline Estimate

| Phase | Effort | Duration |
|-------|--------|----------|
| Phase 1: Abstraction foundation | 4-6 hours | 1 day |
| Phase 2: POSIX implementation | 8-12 hours | 2 days |
| Phase 3: Windows migration | 2-4 hours | 1 day |
| Phase 4: CMake updates | 2-3 hours | 1 day |
| Phase 5: Source refactoring | 4-6 hours | 1 day |
| Phase 6: Testing & verification | 6-10 hours | 2 days |
| **Total** | **26-41 hours** | **~8 days** |

---

## Success Criteria

- [x] Windows build produces `.dll` with all public symbols exported
- [x] Linux build produces `.so` with all public symbols exported
- [x] C++ API callable from Windows and Linux
- [x] C API callable from Windows and Linux
- [x] DVRIP protocol login works identically on both platforms
- [x] Frame callbacks deliver data correctly on both platforms
- [ ] ffmpeg streaming verified on Linux (pending live test)
- [ ] No memory leaks detected (valgrind on Linux, DrMemory on Windows)
- [ ] No compilation warnings on either platform

---

## Future Enhancements

1. **CI/CD Pipeline**: GitHub Actions workflow to build and test on both platforms on each commit
2. **pkg-config Support**: Create `cppdvr.pc` for downstream projects
3. **CMake Export**: Generate `cppdvr-config.cmake` for `find_package()` integration
4. **Documentation**: API documentation generation (doxygen)
5. **Platform-Specific Optimizations**: SIMD, async I/O, etc. (out of current scope)

---

## References

- [POSIX Socket API](https://man7.org/linux/man-pages/man7/socket.7.html)
- [OpenSSL EVP Digest API](https://www.openssl.org/docs/man1.1.1/man3/EVP_DigestInit.html)
- [Linux fork/exec Pattern](https://man7.org/linux/man-pages/man2/fork.2.html)
- [CMake Platform Detection](https://cmake.org/cmake/help/latest/manual/cmake-properties.7.html#properties-on-targets)
- [getifaddrs(3)](https://man7.org/linux/man-pages/man3/getifaddrs.3.html)

---

**Document Version**: 1.1  
**Last Updated**: May 9, 2026  
**Status**: Implementation complete. All platform abstraction phases (1–5) are done. Phase 6 testing is ongoing.
