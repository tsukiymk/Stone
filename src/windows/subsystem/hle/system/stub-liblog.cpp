#include "hle/system/stub-libc.h"
#include "stub-liblog.h"
#include "../../../client/ClientApplicationLogs.h"
#include <windows.h>
#include <format>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <memory>
#include <new>

namespace stone::windows {
extern "C" void AndroidVarargsLog();
extern "C" void AndroidVarargsLogAssert();
extern "C" int StoneAndroidFormat(char* buffer, size_t size, const char* format, AndroidVaList* args);

namespace {
constexpr int MainLog = 0, DefaultLog = INT_MAX;
constexpr int DefaultPriority = 1, VerbosePriority = 2, FatalPriority = 7;
std::atomic<int> minimumLogPriority{DefaultPriority};
std::atomic<uintptr_t> logCallback{}, abortCallback{};
using TranslatedLogger = std::function<void(const AndroidLogMessage*)>;
using TranslatedAborter = std::function<void(const char*)>;
std::atomic<std::shared_ptr<const TranslatedLogger>> translatedLogger;
std::atomic<std::shared_ptr<const TranslatedAborter>> translatedAborter;
std::mutex logTagMutex;
constexpr size_t MaxLogTag = 4068;
std::array<char, MaxLogTag + 1> defaultLogTag{'S', 't', 'o', 'n', 'e'};

std::wstring LogText(const char* text, size_t maximum)
{
    if (!text)
        return {};
    const auto count = static_cast<int>(strnlen(text, maximum));
    if (!count)
        return {};
    // JNI's GetStringUTFChars uses modified UTF-8. Normalize encoded surrogate
    // pairs before Windows decodes the bytes; native UTF-8 passes through.
    std::string normalized;
    const auto input = reinterpret_cast<const unsigned char*>(text);
    for (int i = 0; i < count;) {
        if (i + 5 < count && input[i] == 0xed && input[i + 1] >= 0xa0 && input[i + 1] <= 0xaf &&
            (input[i + 2] & 0xc0) == 0x80 && input[i + 3] == 0xed && input[i + 4] >= 0xb0 && input[i + 4] <= 0xbf &&
            (input[i + 5] & 0xc0) == 0x80) {
            const auto high = ((input[i + 1] & 15) << 6) | (input[i + 2] & 63);
            const auto low = ((input[i + 4] & 15) << 6) | (input[i + 5] & 63);
            const auto code = 0x10000 + (high << 10) + low;
            normalized += static_cast<char>(0xf0 | (code >> 18));
            normalized += static_cast<char>(0x80 | ((code >> 12) & 63));
            normalized += static_cast<char>(0x80 | ((code >> 6) & 63));
            normalized += static_cast<char>(0x80 | (code & 63));
            i += 6;
        }
        else if (i + 1 < count && input[i] == 0xc0 && input[i + 1] == 0x80) {
            normalized += "\xe2\x90\x80";
            i += 2; // visible NUL, never an IPC delimiter
        }
        else
            normalized += text[i++];
    }
    const auto length = static_cast<int>(normalized.size());
    const auto size = MultiByteToWideChar(CP_UTF8, 0, normalized.data(), length, nullptr, 0);
    std::wstring value(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, normalized.data(), length, value.data(), size);
    return value;
}

void RetainLogMessage(const AndroidLogMessage* message) noexcept
{
    try {
        SYSTEMTIME now{};
        GetLocalTime(&now);
        constexpr std::wstring_view priorities = L"??VDIWEF?";
        const auto priority =
            message->priority >= 0 && message->priority < priorities.size() ? priorities[message->priority] : L'?';
        auto tag = LogText(message->tag, 128);
        std::replace_if(
            tag.begin(), tag.end(),
            [](wchar_t ch) {
                return ch < L' ';
            },
            L' ');
        const auto prefix =
            std::format(L"{:02}-{:02} {:02}:{:02}:{:02}.{:03} {:5} {:5} {:5} {} {}: ", now.wMonth, now.wDay, now.wHour,
                        now.wMinute, now.wSecond, now.wMilliseconds, AndroidApplicationUid(), GetCurrentProcessId(),
                        GetCurrentThreadId(), priority, tag);
        auto text = LogText(message->text, 4068);
        std::wstring record;
        size_t at = 0;
        do {
            auto end = text.find(L'\n', at);
            if (end == text.npos)
                end = text.size();
            auto line = text.substr(at, end - at);
            if (!line.empty() && line.back() == L'\r')
                line.pop_back();
            if (record.size() + prefix.size() + line.size() + 1 > 7000) {
                record += L"StoneLog: remaining lines of this record were truncated\n";
                break;
            }
            record += prefix;
            record += line;
            record += L'\n';
            at = end + 1;
        } while (at < text.size());
        QueueClientApplicationLog(std::move(record));
    }
    catch (...) {
    }
}

int LogIsEnabled(int priority, const char*, int fallback)
{
    const auto minimum = minimumLogPriority.load(std::memory_order_relaxed);
    return priority >= (minimum == DefaultPriority ? fallback : minimum);
}

void WriteLogToStderr(const AndroidLogMessage* message)
{
    const char* tag = message->tag ? message->tag : "Stone";
    if (message->file) {
        std::fprintf(stderr, "[android-log %d] [%s] %s:%u %s\n", message->priority, tag, message->file, message->line,
                     message->text);
    }
    else {
        std::fprintf(stderr, "[android-log %d] [%s] %s\n", message->priority, tag, message->text);
    }
}

void WriteLogMessage(const AndroidLogMessage* message)
{
    if (message->size < sizeof(*message))
        return;
    const auto id = message->buffer;
    if (id != DefaultLog && id != MainLog && id != 1 && id != 3 && id != 4)
        return;
    const int saved = errno;
    std::array<char, MaxLogTag + 1> tag;
    AndroidLogMessage entry;
    if (!message->tag) {
        const std::lock_guard lock(logTagMutex);
        tag = defaultLogTag;
        entry = *message;
        entry.size = sizeof(entry);
        entry.tag = tag.data();
        message = &entry;
    }
    RetainLogMessage(message);
    if (const auto translated = translatedLogger.load(std::memory_order_acquire)) {
        (*translated)(message);
    }
    else if (const auto callback = logCallback.load(std::memory_order_acquire)) {
        CallAndroidCallback(callback, const_cast<AndroidLogMessage*>(message));
    }
    else {
        WriteLogToStderr(message);
    }
    errno = saved;
}

int WriteLogBuffer(int buffer, int priority, const char* tag, const char* text)
{
    if (!LogIsEnabled(priority, tag, VerbosePriority))
        return -EPERM;
    AndroidLogMessage message{sizeof(message), buffer, priority, tag, nullptr, 0, text};
    WriteLogMessage(&message);
    return 1;
}

void CallLogAborter(const char* message)
{
    if (!CallAndroidLogAborterIfRegistered(message))
        std::abort();
}

bool InvokeRegisteredAborter(const char* message)
{
    if (const auto translated = translatedAborter.load(std::memory_order_acquire)) {
        (*translated)(message);
    }
    else if (const auto callback = abortCallback.load(std::memory_order_acquire)) {
        CallAndroidCallback(callback, const_cast<char*>(message));
    }
    else {
        return false;
    }
    return true;
}

void AppendEventText(std::string& line, const char* input, size_t size)
{
    constexpr char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < size; ++i) {
        const auto byte = static_cast<unsigned char>(input[i]);
        if (byte >= 0x20 && byte != 0x7f && byte != '\\') {
            line += static_cast<char>(byte);
        }
        else {
            line += "\\x";
            line += hex[byte >> 4];
            line += hex[byte & 15];
        }
    }
}

int WriteStringEvent(int32_t tag, const char* text)
{
    if (!text)
        return -EINVAL;
    constexpr size_t HeaderBytes = 9, MaxPayload = 4068;
    const auto length = strnlen(text, MaxPayload - HeaderBytes);
    const int saved = errno;
    try {
        std::string message = "[android-event " + std::to_string(tag) + "] ";
        AppendEventText(message, text, length);
        message += '\n';
        const bool emitted = std::fwrite(message.data(), 1, message.size(), stderr) == message.size();
        errno = saved;
        return emitted ? static_cast<int>(HeaderBytes + length) : -EIO;
    }
    catch (const std::bad_alloc&) {
        errno = saved;
        return -ENOMEM;
    }
}

int WriteErrorEvent(int tag, const char* name, int32_t uid, const char* data, uint32_t length)
{
    if (!name || (!data && length))
        return -EINVAL;
    // The events encoding contains a tag, a list header and three typed values.
    constexpr size_t HeaderBytes = 21, MaxPayload = 4068;
    const auto nameLength = strnlen(name, 32);
    const auto dataLength = data ? strnlen(data, std::min<size_t>(length, MaxPayload - HeaderBytes - nameLength)) : 0;
    const int saved = errno;
    try {
        std::string message = "[android-event " + std::to_string(tag) + "] uid=" + std::to_string(uid) + " ";
        AppendEventText(message, name, nameLength);
        if (dataLength) {
            message += ": ";
            AppendEventText(message, data, dataLength);
        }
        message += '\n';
        const bool emitted = std::fwrite(message.data(), 1, message.size(), stderr) == message.size();
        errno = saved;
        return emitted ? static_cast<int>(HeaderBytes + nameLength + dataLength) : -EIO;
    }
    catch (const std::bad_alloc&) {
        errno = saved;
        return -ENOMEM;
    }
}
}

extern "C" int StoneAndroidLog(int priority, const char* tag, const char* format, AndroidVaList* args)
{
    if (!LogIsEnabled(priority, tag, VerbosePriority))
        return -EPERM;
    char message[1024];
    const int saved = errno;
    const int count = StoneAndroidFormat(message, sizeof(message), format, args);
    errno = saved;
    if (count < 0)
        return -1;
    const AndroidLogMessage entry{sizeof(entry), MainLog, priority, tag, nullptr, 0, message};
    WriteLogMessage(&entry);
    return 1;
}

extern "C" [[noreturn]] void StoneAndroidLogAssert(const char* condition, const char* tag, const char* format,
                                                   AndroidVaList* args)
{
    char message[1024];
    if (!format || StoneAndroidFormat(message, sizeof(message), format, args) < 0) {
        if (condition)
            std::snprintf(message, sizeof(message), "Assertion failed: %s", condition);
        else
            std::memcpy(message, "Assertion failed", sizeof("Assertion failed"));
    }
    std::fprintf(stderr, "%s\n", message);
    WriteLogBuffer(MainLog, FatalPriority, tag, message);
    CallLogAborter(message);
    std::abort();
}

uintptr_t AndroidLogImport(std::string_view name)
{
#define BIND(symbol, ...)                                                                                              \
    if (name == #symbol)                                                                                               \
    return BridgeAndroidImport(name, reinterpret_cast<uintptr_t>(__VA_ARGS__))
    if (name == "__android_log_print")
        return reinterpret_cast<uintptr_t>(&AndroidVarargsLog);
    if (name == "__android_log_assert")
        return reinterpret_cast<uintptr_t>(&AndroidVarargsLogAssert);
    BIND(__android_log_vprint, &StoneAndroidLog);
    BIND(
        __android_log_write, +[](int priority, const char* tag, const char* text) {
            return WriteLogBuffer(MainLog, priority, tag, text);
        });
    BIND(__android_log_buf_write, &WriteLogBuffer);
    BIND(__android_log_bswrite, &WriteStringEvent);
    BIND(__android_log_error_write, &WriteErrorEvent);
    BIND(__android_log_write_log_message, &WriteLogMessage);
    BIND(__android_log_logd_logger, &WriteLogToStderr);
    BIND(__android_log_stderr_logger, &WriteLogToStderr);
    BIND(__android_log_is_loggable, &LogIsEnabled);
    BIND(
        __android_log_get_minimum_priority, +[] {
            return minimumLogPriority.load(std::memory_order_relaxed);
        });
    BIND(
        __android_log_set_minimum_priority, +[](int priority) {
            return minimumLogPriority.exchange(priority, std::memory_order_relaxed);
        });
    BIND(__android_log_set_default_tag, &SetAndroidLogDefaultTag);
    BIND(
        __android_log_set_logger, +[](uintptr_t callback) {
            translatedLogger.store({}, std::memory_order_release);
            logCallback.store(callback, std::memory_order_release);
        });
    BIND(
        __android_log_set_aborter, +[](uintptr_t callback) {
            translatedAborter.store({}, std::memory_order_release);
            abortCallback.store(callback, std::memory_order_release);
        });
    BIND(__android_log_call_aborter, &CallLogAborter);
#undef BIND
    return 0;
}

int WriteAndroidLog(int priority, const char* tag, const char* message, int buffer)
{
    return WriteLogBuffer(buffer, priority, tag, message);
}

void SetAndroidLogDefaultTag(const char* tag)
{
    const std::lock_guard lock(logTagMutex);
    const auto size = tag ? strnlen(tag, defaultLogTag.size() - 1) : 0;
    if (size)
        std::memcpy(defaultLogTag.data(), tag, size);
    defaultLogTag[size] = '\0';
}

int IsAndroidLogEnabled(int priority, const char* tag, int fallback)
{
    return LogIsEnabled(priority, tag, fallback);
}

void WriteAndroidLogMessage(const AndroidLogMessage* message)
{
    WriteLogMessage(message);
}

void WriteAndroidLogToStderr(const AndroidLogMessage* message)
{
    WriteLogToStderr(message);
}

void CallAndroidLogAborter(const char* message)
{
    CallLogAborter(message);
}

bool CallAndroidLogAborterIfRegistered(const char* message)
{
    return InvokeRegisteredAborter(message);
}

void SetTranslatedAndroidLogger(TranslatedLogger callback)
{
    auto entry = callback ? std::make_shared<const TranslatedLogger>(std::move(callback)) : nullptr;
    logCallback.store(0, std::memory_order_release);
    translatedLogger.store(std::move(entry), std::memory_order_release);
}

void SetTranslatedAndroidAborter(TranslatedAborter callback)
{
    auto entry = callback ? std::make_shared<const TranslatedAborter>(std::move(callback)) : nullptr;
    abortCallback.store(0, std::memory_order_release);
    translatedAborter.store(std::move(entry), std::memory_order_release);
}

uintptr_t Arm64LogImport(std::string_view symbol)
{
    static const uint8_t logBufferPrint = 3;
    return symbol == "__android_log_buf_print" ? reinterpret_cast<uintptr_t>(&logBufferPrint) : 0;
}

}
