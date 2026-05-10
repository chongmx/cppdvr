cmake_minimum_required(VERSION 3.20)

# Guard against being included twice (e.g. via add_subdirectory + find_package)
if(TARGET cppdvr)
    return()
endif()

project(cppdvr VERSION 1.0.0 LANGUAGES CXX C)

# ── C++ standard ────────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Platform-specific configuration ─────────────────────────────────────────────
if(WIN32)
    add_compile_definitions(
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _WIN32_WINNT=0x0601   # Windows 7+
        _CRT_SECURE_NO_WARNINGS
    )
    set(PLATFORM_SOURCES
        src/platform/windows/platform_net.cpp
        src/platform/windows/platform_crypto.cpp
        src/platform/windows/platform_process.cpp
        src/dllmain.cpp
    )
    set(PLATFORM_LIBS ws2_32 crypt32 iphlpapi winmm)
elseif(UNIX)
    add_compile_options(-fPIC)  # Position-independent code for .so

    # AUTO  — system OpenSSL if present, else built-in MD5 (default)
    # BUILTIN  — always use the self-contained 80-line MD5
    # OPENSSL  — require system OpenSSL; print install instructions and stop if missing
    # FETCH    — system OpenSSL if present, else build it from source via FetchContent
    set(CPPDVR_CRYPTO "AUTO" CACHE STRING
        "MD5 backend: AUTO | BUILTIN | OPENSSL (require system) | FETCH (build from source)")
    set_property(CACHE CPPDVR_CRYPTO PROPERTY STRINGS AUTO BUILTIN OPENSSL FETCH)

    set(PLATFORM_SOURCES
        src/platform/posix/platform_net.cpp
        src/platform/posix/platform_process.cpp
    )
    set(PLATFORM_LIBS pthread)

    if(CPPDVR_CRYPTO STREQUAL "AUTO")
        find_package(OpenSSL QUIET)
        if(OPENSSL_FOUND)
            message(STATUS "Crypto: system OpenSSL ${OPENSSL_VERSION}")
            list(APPEND PLATFORM_SOURCES src/platform/posix/platform_crypto_openssl.cpp)
            list(APPEND PLATFORM_LIBS OpenSSL::Crypto)
        else()
            message(STATUS "Crypto: OpenSSL not found — using built-in MD5")
            list(APPEND PLATFORM_SOURCES src/platform/posix/platform_crypto.cpp)
        endif()

    elseif(CPPDVR_CRYPTO STREQUAL "BUILTIN")
        message(STATUS "Crypto: built-in MD5")
        list(APPEND PLATFORM_SOURCES src/platform/posix/platform_crypto.cpp)

    elseif(CPPDVR_CRYPTO STREQUAL "OPENSSL")
        find_package(OpenSSL QUIET)
        if(NOT OPENSSL_FOUND)
            message(FATAL_ERROR
                "OpenSSL not found. Install the development headers and re-run cmake:\n"
                "  Debian / Ubuntu / WSL:  sudo apt install libssl-dev\n"
                "  Fedora / RHEL:          sudo dnf install openssl-devel\n"
                "  macOS (Homebrew):       brew install openssl\n"
                "Other options:\n"
                "  Build OpenSSL from source: cmake -DCPPDVR_CRYPTO=FETCH ..\n"
                "  Use built-in MD5 (no deps): cmake -DCPPDVR_CRYPTO=BUILTIN .."
            )
        endif()
        message(STATUS "Crypto: system OpenSSL ${OPENSSL_VERSION}")
        list(APPEND PLATFORM_SOURCES src/platform/posix/platform_crypto_openssl.cpp)
        list(APPEND PLATFORM_LIBS OpenSSL::Crypto)

    elseif(CPPDVR_CRYPTO STREQUAL "FETCH")
        find_package(OpenSSL QUIET)
        if(OPENSSL_FOUND)
            message(STATUS "Crypto: system OpenSSL ${OPENSSL_VERSION}")
            list(APPEND PLATFORM_SOURCES src/platform/posix/platform_crypto_openssl.cpp)
            list(APPEND PLATFORM_LIBS OpenSSL::Crypto)
        else()
            message(STATUS "Crypto: OpenSSL not found — fetching from source...")
            include(FetchContent)
            FetchContent_Declare(
                openssl_cmake
                GIT_REPOSITORY https://github.com/janbar/openssl-cmake.git
                GIT_TAG        1.1.1w-20231130
                GIT_SHALLOW    TRUE
            )
            FetchContent_MakeAvailable(openssl_cmake)
            message(STATUS "Crypto: OpenSSL built from source")
            list(APPEND PLATFORM_SOURCES src/platform/posix/platform_crypto_openssl.cpp)
            list(APPEND PLATFORM_LIBS ssl crypto)
        endif()

    else()
        message(FATAL_ERROR
            "Unknown CPPDVR_CRYPTO='${CPPDVR_CRYPTO}'. Valid values: AUTO | BUILTIN | OPENSSL | FETCH")
    endif()
endif()

# ── Third-party: nlohmann/json ───────────────────────────────────────────────────
# Guard so a parent project that already fetched nlohmann_json doesn't re-declare it.
if(NOT TARGET nlohmann_json::nlohmann_json)
    include(FetchContent)
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG        v3.11.3
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

# ── Library target ───────────────────────────────────────────────────────────────
add_library(cppdvr SHARED
    ${PLATFORM_SOURCES}
    src/cppdvr_api.cpp
    src/dvrip.cpp
    src/stream_server.cpp
    src/udp_stream_server.cpp
    src/video_recorder.cpp
    src/jpeg_overlay.cpp
)

# Namespaced alias — parent projects use: target_link_libraries(myapp PRIVATE cppdvr::cppdvr)
add_library(cppdvr::cppdvr ALIAS cppdvr)

target_include_directories(cppdvr
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/platform
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb
)

# ── JPEG backend: libjpeg-turbo (SIMD — cross-platform) ──────────────────────
# Enabled by default; disable with -DCPPDVR_LIBJPEG_TURBO=OFF
option(CPPDVR_LIBJPEG_TURBO "Enable libjpeg-turbo SIMD-accelerated JPEG backend" ON)
if(CPPDVR_LIBJPEG_TURBO)
    # 1. Try vcpkg / CMake-config install
    find_package(libjpeg-turbo CONFIG QUIET)
    if(libjpeg-turbo_FOUND)
        message(STATUS "JPEG backend: libjpeg-turbo (CMake config)")
        target_compile_definitions(cppdvr PRIVATE CPPDVR_HAS_LIBJPEG_TURBO)
        if(TARGET libjpeg-turbo::turbojpeg-static)
            target_link_libraries(cppdvr PRIVATE libjpeg-turbo::turbojpeg-static)
        else()
            target_link_libraries(cppdvr PRIVATE libjpeg-turbo::turbojpeg)
        endif()
    else()
        # 2. Try pkg-config (Linux/macOS system install)
        find_package(PkgConfig QUIET)
        if(PKG_CONFIG_FOUND)
            pkg_check_modules(TURBOJPEG QUIET IMPORTED_TARGET libturbojpeg)
        endif()
        if(TURBOJPEG_FOUND)
            message(STATUS "JPEG backend: libjpeg-turbo (pkg-config ${TURBOJPEG_VERSION})")
            target_compile_definitions(cppdvr PRIVATE CPPDVR_HAS_LIBJPEG_TURBO)
            target_link_libraries(cppdvr PRIVATE PkgConfig::TURBOJPEG)
        else()
            message(STATUS "JPEG backend: libjpeg-turbo NOT found — using stb_image fallback")
            message(STATUS "  Install hint (Windows/vcpkg): vcpkg install libjpeg-turbo:x64-windows")
            message(STATUS "  Install hint (Ubuntu):        sudo apt install libturbojpeg0-dev")
            message(STATUS "  Install hint (macOS):         brew install jpeg-turbo")
            set(CPPDVR_LIBJPEG_TURBO OFF CACHE BOOL "" FORCE)
        endif()
    endif()
endif()

# ── JPEG backend: nvJPEG (NVIDIA GPU) ────────────────────────────────────────
# Disabled by default; enable with -DCPPDVR_NVJPEG=ON (requires CUDA toolkit)
option(CPPDVR_NVJPEG "Enable nvJPEG GPU-accelerated JPEG backend (requires CUDA toolkit)" OFF)
if(CPPDVR_NVJPEG)
    find_package(CUDAToolkit QUIET)
    if(CUDAToolkit_FOUND)
        if(TARGET CUDA::nvjpeg)
            message(STATUS "JPEG backend: nvJPEG (CUDA ${CUDAToolkit_VERSION})")
            target_compile_definitions(cppdvr PRIVATE CPPDVR_HAS_NVJPEG)
            target_link_libraries(cppdvr PRIVATE CUDA::nvjpeg CUDA::cudart)
        else()
            message(WARNING "CPPDVR_NVJPEG=ON: CUDA ${CUDAToolkit_VERSION} found but nvJPEG target missing — nvJPEG disabled")
            set(CPPDVR_NVJPEG OFF CACHE BOOL "" FORCE)
        endif()
    else()
        message(WARNING "CPPDVR_NVJPEG=ON: CUDA toolkit not found — nvJPEG disabled")
        message(STATUS "  Install: https://developer.nvidia.com/cuda-downloads")
        set(CPPDVR_NVJPEG OFF CACHE BOOL "" FORCE)
    endif()
endif()

target_link_libraries(cppdvr PRIVATE
    nlohmann_json::nlohmann_json
    ${PLATFORM_LIBS}
)

target_compile_definitions(cppdvr PRIVATE CPPDVR_BUILDING_DLL)

# ── Output naming ───────────────────────────────────────────────────────────────
set_target_properties(cppdvr PROPERTIES
    OUTPUT_NAME   "cppdvr"
    DEBUG_POSTFIX "d"
)

# ── Install + CMake package export ───────────────────────────────────────────────
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS cppdvr
    EXPORT      cppdvrTargets
    RUNTIME     DESTINATION ${CMAKE_INSTALL_BINDIR}       # .dll
    LIBRARY     DESTINATION ${CMAKE_INSTALL_LIBDIR}       # .so
    ARCHIVE     DESTINATION ${CMAKE_INSTALL_LIBDIR}       # .lib / .a
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT cppdvrTargets
    FILE        cppdvrTargets.cmake
    NAMESPACE   cppdvr::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cppdvr
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/cppdvrConfigVersion.cmake"
    VERSION     ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cppdvrConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/cppdvrConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cppdvr
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/cppdvrConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/cppdvrConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cppdvr
)

# ── Demo executables ─────────────────────────────────────────────────────────────
# Default ON when building standalone, OFF when included as a submodule.
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    option(CPPDVR_BUILD_DEMO "Build diagnostic demo executables" ON)
else()
    option(CPPDVR_BUILD_DEMO "Build diagnostic demo executables" OFF)
endif()

# ── Integration test executables ─────────────────────────────────────────────────
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    option(CPPDVR_BUILD_TESTS "Build DVR integration test executables" ON)
else()
    option(CPPDVR_BUILD_TESTS "Build DVR integration test executables" OFF)
endif()

if(CPPDVR_BUILD_TESTS)
    add_subdirectory(tests)
endif()

if(CPPDVR_BUILD_DEMO)
    add_executable(cppdvr_demo demo/main.cpp)
    target_link_libraries(cppdvr_demo PRIVATE cppdvr)
    target_include_directories(cppdvr_demo PRIVATE include)
    add_custom_command(TARGET cppdvr_demo POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:cppdvr>
            $<TARGET_FILE_DIR:cppdvr_demo>
    )

    add_executable(cppdvr_test_frame demo/test_frame.cpp)
    target_link_libraries(cppdvr_test_frame PRIVATE cppdvr)
    target_include_directories(cppdvr_test_frame PRIVATE include)
    add_custom_command(TARGET cppdvr_test_frame POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:cppdvr>
            $<TARGET_FILE_DIR:cppdvr_test_frame>
    )

    add_executable(test_rec_mp4 demo/test_rec_mp4.c)
    target_link_libraries(test_rec_mp4 PRIVATE cppdvr)
    target_include_directories(test_rec_mp4 PRIVATE include)
    add_custom_command(TARGET test_rec_mp4 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:cppdvr>
            $<TARGET_FILE_DIR:test_rec_mp4>
    )
endif()
