cmake_minimum_required(VERSION 3.20)
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

# ── Third-party: nlohmann/json (header-only, fetched at configure time) ─────────
include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# ── DLL target ──────────────────────────────────────────────────────────────────
add_library(cppdvr SHARED
    ${PLATFORM_SOURCES}
    src/cppdvr_api.cpp
    src/dvrip.cpp
    src/stream_server.cpp
    src/udp_stream_server.cpp
    src/video_recorder.cpp
)

target_include_directories(cppdvr
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/platform
)

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

# ── Install rules ───────────────────────────────────────────────────────────────
install(TARGETS cppdvr
    RUNTIME DESTINATION bin   # .dll
    ARCHIVE DESTINATION lib   # .lib import library
)
install(DIRECTORY include/ DESTINATION include)

# ── Diagnostic demo executable (built by default) ───────────────────────────────
option(CPPDVR_BUILD_DEMO "Build diagnostic demo executable" ON)
if(CPPDVR_BUILD_DEMO)
    add_executable(cppdvr_demo demo/main.cpp)
    target_link_libraries(cppdvr_demo PRIVATE cppdvr)
    target_include_directories(cppdvr_demo PRIVATE include)
    # Copy the DLL next to the demo exe after build
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

    # Recorder MP4 integration test (records from a live DVR stream)
    add_executable(test_rec_mp4 demo/test_rec_mp4.c)
    target_link_libraries(test_rec_mp4 PRIVATE cppdvr)
    target_include_directories(test_rec_mp4 PRIVATE include)
    add_custom_command(TARGET test_rec_mp4 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:cppdvr>
            $<TARGET_FILE_DIR:test_rec_mp4>
    )
endif()
