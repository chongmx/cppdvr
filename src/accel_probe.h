#pragma once
#include <string>

namespace cppdvr {

// Decode hwaccel probe (implemented in stream_server.cpp).
// Runs `ffmpeg -hwaccels` once; result cached for the process lifetime.
bool ffmpeg_has_hwaccel(const std::string& name);

// Encode capability probe (implemented in video_recorder.cpp).
// Runs `ffmpeg -encoders` list-check + a smoke-test encode; both cached.
bool ffmpeg_has_encoder(const std::string& name);

} // namespace cppdvr
