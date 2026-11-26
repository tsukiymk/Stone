#include "stub-libmediandk-format.h"
#include "cpu/arm64/Arm64Initializer.h"
#include "stub-libc.h"
#include "stub-libmediandk-image-reader.h"

#include <algorithm>
#include <map>
#include <new>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace stone::windows {
using ByteBuffer = std::vector<uint8_t>;

struct AndroidMediaFormat {
    std::map<std::string, std::variant<int32_t, std::string, ByteBuffer>, std::less<>> entries;
    std::map<std::string, std::string, std::less<>> returnedStrings;
    std::map<std::string, ByteBuffer, std::less<>> returnedBuffers;
};

namespace {
template <typename T> const T* FindValue(const AndroidMediaFormat& format, const char* name)
{
    const auto entry = format.entries.find(name);
    return entry == format.entries.end() ? nullptr : std::get_if<T>(&entry->second);
}

bool GetBuffer(AndroidMediaFormat* format, const char* name, void** data, size_t* size)
{
    if (!format || !name || !data || !size)
        return false;
    const auto* value = FindValue<ByteBuffer>(*format, name);
    if (!value)
        return false;
    try {
        auto& buffer = format->returnedBuffers.insert_or_assign(name, *value).first->second;
        *data = buffer.data();
        *size = buffer.size();
        return true;
    }
    catch (const std::bad_alloc&) {
        return false;
    }
}
}

AndroidMediaFormat* NewAndroidMediaFormat()
{
    return new (std::nothrow) AndroidMediaFormat;
}

int DeleteAndroidMediaFormat(AndroidMediaFormat* format)
{
    delete format;
    return 0;
}

bool GetAndroidMediaFormatInt(const AndroidMediaFormat* format, const char* name, int32_t* result)
{
    if (!format || !name || !result)
        return false;
    const auto* value = FindValue<int32_t>(*format, name);
    if (!value)
        return false;
    *result = *value;
    return true;
}

std::string_view GetAndroidMediaFormatString(const AndroidMediaFormat* format, const char* name)
{
    if (!format || !name)
        return {};
    const auto* value = FindValue<std::string>(*format, name);
    return value ? std::string_view(*value) : std::string_view{};
}

namespace {
bool GetString(AndroidMediaFormat* format, const char* name, const char** result)
{
    if (!format || !name || !result)
        return false;
    const auto* value = FindValue<std::string>(*format, name);
    if (!value) {
        const auto cached = format->returnedStrings.find(name);
        if (cached != format->returnedStrings.end())
            format->returnedStrings.erase(cached);
        return false;
    }
    try {
        // The returned pointer remains valid even if the entry is replaced
        // before the next getString call for this name.
        *result = format->returnedStrings.insert_or_assign(name, *value).first->second.c_str();
        return true;
    }
    catch (const std::bad_alloc&) {
        return false;
    }
    catch (const std::length_error&) {
        return false;
    }
}
}

void SetAndroidMediaFormatInt(AndroidMediaFormat* format, const char* name, int32_t value)
{
    if (!format || !name)
        return;
    try {
        format->entries.insert_or_assign(name, value);
    }
    catch (const std::bad_alloc&) {
    }
    catch (const std::length_error&) {
    }
}

void SetAndroidMediaFormatString(AndroidMediaFormat* format, const char* name, const char* value)
{
    if (!format || !name || !value)
        return;
    try {
        format->entries.insert_or_assign(name, std::string(value));
    }
    catch (const std::bad_alloc&) {
    }
    catch (const std::length_error&) {
    }
}

void SetAndroidMediaFormatBuffer(AndroidMediaFormat* format, const char* name, const void* data, size_t size)
{
    if (!format || !name || (!data && size) || size > PTRDIFF_MAX)
        return;
    try {
        ByteBuffer buffer;
        if (size) {
            const auto* bytes = static_cast<const uint8_t*>(data);
            buffer.assign(bytes, bytes + size);
        }
        format->entries.insert_or_assign(name, std::move(buffer));
    }
    catch (const std::bad_alloc&) {
    }
    catch (const std::length_error&) {
    }
}

uintptr_t AndroidMediaFormatImport(std::string_view symbol, bool hostAbi)
{
    constexpr std::string_view prefix = "AMEDIAFORMAT_KEY_";
    if (symbol.starts_with(prefix)) {
        const auto key = symbol.substr(prefix.size());
        const auto entry =
            std::ranges::find(AndroidMediaFormatKeys, key, &std::pair<std::string_view, const char*>::first);
        if (entry != std::end(AndroidMediaFormatKeys))
            return reinterpret_cast<uintptr_t>(&entry->second);
        return 0;
    }
#define BIND(name, ...)                                                                                                \
    if (symbol == #name) {                                                                                             \
        const auto target = reinterpret_cast<uintptr_t>(__VA_ARGS__);                                                  \
        return hostAbi ? target : BridgeAndroidImport(symbol, target);                                                 \
    }
    BIND(AMediaFormat_new, &NewAndroidMediaFormat);
    BIND(AMediaFormat_delete, &DeleteAndroidMediaFormat);
    BIND(AMediaFormat_getInt32, &GetAndroidMediaFormatInt);
    BIND(AMediaFormat_getString, &GetString);
    BIND(AMediaFormat_getBuffer, &GetBuffer);
    BIND(AMediaFormat_setInt32, &SetAndroidMediaFormatInt);
    BIND(AMediaFormat_setString, &SetAndroidMediaFormatString);
    BIND(AMediaFormat_setBuffer, &SetAndroidMediaFormatBuffer);
#undef BIND
    return 0;
}

uintptr_t AndroidMediaImport(std::string_view symbol)
{
    if (symbol.starts_with("AMediaCodec_"))
        return AndroidMediaCodecImport(symbol);
    if (symbol.starts_with("AImage"))
        return AndroidImageReaderImport(symbol);
    return AndroidMediaFormatImport(symbol);
}

uintptr_t Arm64MediaImport(std::string_view symbol)
{
    constexpr auto& keys = AndroidMediaFormatKeys;
    constexpr std::string_view prefix = "AMEDIAFORMAT_KEY_";
    if (symbol.starts_with(prefix)) {
        const auto key = symbol.substr(prefix.size());
        const auto found = std::ranges::find_if(keys, [&](const auto& entry) {
            return entry.first == key;
        });
        if (found == std::end(keys))
            return 0;
        static const auto slots = [] {
            constexpr auto& keys = AndroidMediaFormatKeys;
            const auto address = AllocateArm64GuestMemory(std::size(keys) * sizeof(uintptr_t), false);
            if (!address)
                throw std::bad_alloc();
            auto* pointers = reinterpret_cast<uintptr_t*>(address);
            for (size_t i = 0; i < std::size(keys); ++i)
                pointers[i] = CopyArm64GuestConstantString(keys[i].second);
            return pointers;
        }();
        return reinterpret_cast<uintptr_t>(slots + (found - std::begin(keys)));
    }
    static constexpr std::string_view names[]{
        "AMediaCodec_configure",
        "AMediaCodec_createDecoderByType",
        "AMediaCodec_createEncoderByType",
        "AMediaCodec_delete",
        "AMediaCodec_dequeueInputBuffer",
        "AMediaCodec_dequeueOutputBuffer",
        "AMediaCodec_flush",
        "AMediaCodec_getInputBuffer",
        "AMediaCodec_getOutputBuffer",
        "AMediaCodec_getOutputFormat",
        "AMediaCodec_queueInputBuffer",
        "AMediaCodec_releaseOutputBuffer",
        "AMediaCodec_start",
        "AMediaCodec_stop",
        "AMediaExtractor_advance",
        "AMediaExtractor_delete",
        "AMediaExtractor_getSampleTime",
        "AMediaExtractor_getTrackCount",
        "AMediaExtractor_getTrackFormat",
        "AMediaExtractor_new",
        "AMediaExtractor_readSampleData",
        "AMediaExtractor_seekTo",
        "AMediaExtractor_selectTrack",
        "AMediaExtractor_setDataSourceFd",
        "AMediaFormat_delete",
        "AMediaFormat_getInt32",
        "AMediaFormat_getString",
        "AMediaFormat_new",
        "AMediaFormat_setBuffer",
        "AMediaFormat_setFloat",
        "AMediaFormat_setInt32",
        "AMediaFormat_setString",
        "AMediaFormat_toString",
    };
    static const std::array<uint8_t, std::size(names)> tokens{};
    const auto found = std::ranges::find(names, symbol);
    return found == std::end(names) ? 0 : reinterpret_cast<uintptr_t>(&tokens[found - names]);
}

}
