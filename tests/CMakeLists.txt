# tests/CMakeLists.txt — DVR integration test executables
#
# Each test is a standalone C executable that connects to a real DVR camera.
# The camera address defaults to 172.20.80.12; override with DVR_HOST env var
# or by passing the host as the first CLI argument.
#
# Build:  cmake --build build --target test_discovery  (or any other target)
# Run:    build/tests/discovery/test_discovery.exe
#         DVR_HOST=192.168.1.50 build/tests/connection/test_connection.exe

cmake_minimum_required(VERSION 3.20)

# Helper: create one test executable and copy the DLL next to it on Windows
function(add_dvr_test target src)
    add_executable(${target} ${src})
    target_link_libraries(${target} PRIVATE cppdvr)
    target_include_directories(${target} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/common
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    # Keep test binaries in a predictable sub-tree
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    )
    # Windows: copy cppdvr.dll next to the test exe
    if(WIN32)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:cppdvr>
                $<TARGET_FILE_DIR:${target}>
            COMMENT "Copying cppdvr.dll to ${target} output dir"
        )
    endif()
endfunction()

# ── Test executables ────────────────────────────────────────────────────────────
add_dvr_test(test_discovery      discovery/test_discovery.c)
add_dvr_test(test_snapshot_diag  snapshot/test_snapshot_diag.c)
add_dvr_test(test_connection connection/test_connection.c)
add_dvr_test(test_snapshot   snapshot/test_snapshot.c)
add_dvr_test(test_streaming  streaming/test_streaming.c)
add_dvr_test(test_ptz        ptz/test_ptz.c)
add_dvr_test(test_time       time/test_time.c)
add_dvr_test(test_osd        osd/test_osd.c)
add_dvr_test(test_network    network/test_network.c)
add_dvr_test(test_config     config/test_config.c)
add_dvr_test(probe_params    config/probe_params.c)
add_dvr_test(test_encoding   encoding/test_encoding.c)
add_dvr_test(test_camera     camera/test_camera.c)
add_dvr_test(test_osd_record      osd/test_osd_record.c)
add_dvr_test(test_osd_text_record osd/test_osd_text_record.c)
add_dvr_test(test_jpeg_markers   osd/test_jpeg_markers.c)
add_dvr_test(test_overlay_record osd/test_overlay_record.c)
add_dvr_test(test_dual_record    osd/test_dual_record.c)
add_dvr_test(test_overlay_only   osd/test_overlay_only.c)
add_dvr_test(test_raw_only       osd/test_raw_only.c)
