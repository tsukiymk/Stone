#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

namespace stone::windows {
// These are NDK data exports. ELF callers load a pointer from each slot.
inline constexpr std::pair<std::string_view, const char*> AndroidMediaFormatKeys[]{
    {"AAC_PROFILE", "aac-profile"},
    {"BITRATE_MODE", "bitrate-mode"},
    {"BIT_RATE", "bitrate"},
    {"CHANNEL_COUNT", "channel-count"},
    {"COLOR_FORMAT", "color-format"},
    {"COLOR_RANGE", "color-range"},
    {"COLOR_STANDARD", "color-standard"},
    {"COLOR_TRANSFER", "color-transfer"},
    {"CSD_0", "csd-0"},
    {"FRAME_RATE", "frame-rate"},
    {"HEIGHT", "height"},
    {"IS_ADTS", "is-adts"},
    {"I_FRAME_INTERVAL", "i-frame-interval"},
    {"LATENCY", "latency"},
    {"LEVEL", "level"},
    {"MAX_INPUT_SIZE", "max-input-size"},
    {"MIME", "mime"},
    {"PRIORITY", "priority"},
    {"PROFILE", "profile"},
    {"SAMPLE_RATE", "sample-rate"},
    {"SLICE_HEIGHT", "slice-height"},
    {"STRIDE", "stride"},
    {"TEMPORAL_LAYERING", "ts-schema"},
    {"WIDTH", "width"},
};

uintptr_t AndroidMediaFormatImport(std::string_view symbol, bool hostAbi = false);
uintptr_t AndroidMediaImport(std::string_view symbol);
uintptr_t Arm64MediaImport(std::string_view symbol);
uintptr_t AndroidMediaCodecImport(std::string_view symbol);
uintptr_t AndroidCodecCallbackThunk(uintptr_t target);
void CallAndroidCodecError(uintptr_t callback, void* codec, void* context, int error, const char* detail);

struct AndroidMediaFormat;
AndroidMediaFormat* NewAndroidMediaFormat();
int DeleteAndroidMediaFormat(AndroidMediaFormat* format);
bool GetAndroidMediaFormatInt(const AndroidMediaFormat* format, const char* name, int32_t* result);
std::string_view GetAndroidMediaFormatString(const AndroidMediaFormat* format, const char* name);
void SetAndroidMediaFormatInt(AndroidMediaFormat* format, const char* name, int32_t value);
void SetAndroidMediaFormatString(AndroidMediaFormat* format, const char* name, const char* value);
void SetAndroidMediaFormatBuffer(AndroidMediaFormat* format, const char* name, const void* data, size_t size);
}
