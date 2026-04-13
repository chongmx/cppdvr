cmake_minimum_required(VERSION 3.20)
project(cppdvr VERSION 1.0.0 LANGUAGES CXX C)

# ── C++ standard ────────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Windows definitions ─────────────────────────────────────────────────────────
if(WIN32)
    add_compile_definitions(
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _WIN32_WINNT=0x0601   # Windows 7+
        _CRT_SECURE_NO_WARNINGS
    )
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
    src/dllmain.cpp
    src/dvrip.cpp
    src/stream_server.cpp
    src/udp_stream_server.cpp
    src/video_recorder.cpp
)

target_include_directories(cppdvr
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(cppdvr PRIVATE
    nlohmann_json::nlohmann_json
    ws2_32      # Winsock2
    crypt32     # Windows CryptoAPI (MD5 for Sofia hash)
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
