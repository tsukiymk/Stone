#include "ProcessLog.h"
#include "UserPaths.h"

#include <windows.h>
#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <mutex>
#include <new>
#include <string>
#include <string_view>

namespace stone::windows {
namespace {
struct LogStream {
    bool Start(FILE* stream, DWORD standardHandle);
    void Restore();
    bool Drain();

private:
    static DWORD WINAPI CopyOutput(void* context);

    int descriptor = -1;
    int original = -1;
    DWORD standardHandle = 0;
    HANDLE reader = nullptr;
    HANDLE forwardTo = nullptr;
    HANDLE worker = nullptr;
    bool redirected = false;
};

std::mutex logWriteMutex;
HANDLE logFile = INVALID_HANDLE_VALUE;
std::filesystem::path logPath;
std::filesystem::path logDirectory;
std::wstring logSuffix;
std::wstring logName;
bool logging = false;

std::wstring TimestampSuffix()
{
    FILETIME utc{}, local{};
    GetSystemTimePreciseAsFileTime(&utc);
    FileTimeToLocalFileTime(&utc, &local);
    SYSTEMTIME now{};
    FileTimeToSystemTime(&local, &now);
    ULARGE_INTEGER ticks{};
    ticks.LowPart = local.dwLowDateTime;
    ticks.HighPart = local.dwHighDateTime;
    return std::format(L"{:04}{:02}{:02}-{:02}{:02}{:02}-{:07}.log", now.wYear, now.wMonth, now.wDay, now.wHour,
                       now.wMinute, now.wSecond, ticks.QuadPart % 10000000);
}

bool SafeLabel(std::wstring_view label, std::wstring_view allowed)
{
    return !label.empty() && label.front() != L'.' && label.back() != L'.' && label.find(L"..") == label.npos &&
           label.find_first_not_of(allowed) == label.npos;
}

bool SelectLog(std::wstring_view category, std::wstring_view name)
{
    const std::lock_guard lock(logWriteMutex);
    if (!logging)
        return false;
    const auto directory = logDirectory / category;
    auto suffix = logSuffix;
    auto path = directory / (std::wstring(name) + L'-' + suffix);
    if (path == logPath)
        return true;
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error)
        return false;
    if (logFile != INVALID_HANDLE_VALUE) {
        for (int attempt = 0; !MoveFileExW(logPath.c_str(), path.c_str(), 0); ++attempt) {
            const auto failure = GetLastError();
            if (attempt == 7 || (failure != ERROR_ALREADY_EXISTS && failure != ERROR_FILE_EXISTS))
                return false;
            suffix = TimestampSuffix();
            path = directory / (std::wstring(name) + L'-' + suffix);
        }
    }
    logPath = path;
    logSuffix = suffix;
    logName = std::wstring(name) + L'-';
    return true;
}

// Called under logWriteMutex, only when there is actual output to retain.
HANDLE LogFile()
{
    for (int attempt = 0; logFile == INVALID_HANDLE_VALUE && !logPath.empty() && attempt != 8; ++attempt) {
        logFile = CreateFileW(logPath.c_str(), FILE_APPEND_DATA | FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (logFile != INVALID_HANDLE_VALUE)
            break;
        const auto failure = GetLastError();
        if (failure != ERROR_ALREADY_EXISTS && failure != ERROR_FILE_EXISTS)
            break;
        logSuffix = TimestampSuffix();
        logPath = logPath.parent_path() / (logName + logSuffix);
    }
    return logFile;
}

std::string_view BracketField(std::string_view& text)
{
    if (!text.starts_with('['))
        return {};
    const auto end = text.find("] ");
    if (end == text.npos || end == 1)
        return {};
    const auto value = text.substr(1, end - 1);
    text.remove_prefix(end + 2);
    return value;
}

void WriteOutput(HANDLE file, const char* data, DWORD size)
{
    while (size != 0) {
        DWORD written = 0;
        if (!WriteFile(file, data, size, &written, nullptr) || written == 0)
            return;
        data += written;
        size -= written;
    }
}

void WriteRecord(std::string_view line, bool errorStream)
{
    while (line.ends_with('\r'))
        line.remove_suffix(1);
    if (line.empty())
        return;
    std::string_view source = errorStream ? "stderr" : "stdout";
    std::string_view level = errorStream ? "ERROR" : "INFO";
    std::string_view tag;
    if (const auto field = BracketField(line); !field.empty()) {
        constexpr std::string_view android = "android-log ";
        if (field.starts_with(android) && field.size() == android.size() + 1 && field.back() >= '0' &&
            field.back() <= '8') {
            constexpr std::string_view priorities[]{"INFO", "INFO",  "VERBOSE", "DEBUG", "INFO",
                                                    "WARN", "ERROR", "FATAL",   "INFO"};
            source = "android-log";
            level = priorities[field.back() - '0'];
            tag = BracketField(line);
            if (tag == "Stone")
                tag = {};
        }
        else {
            source = field;
            level = "INFO";
            auto remaining = line;
            const auto explicitLevel = BracketField(remaining);
            if (explicitLevel == "TRACE" || explicitLevel == "DEBUG" || explicitLevel == "INFO" ||
                explicitLevel == "WARN" || explicitLevel == "ERROR" || explicitLevel == "FATAL") {
                level = explicitLevel;
                line = remaining;
            }
        }
    }
    else if (line.starts_with("Stone: ")) {
        source = "stone";
        line.remove_prefix(7);
    }

    SYSTEMTIME now{};
    GetLocalTime(&now);
    auto record =
        std::format("[{:04}-{:02}-{:02} {:02}:{:02}:{:02}{:03}] [p-{}] [{}] [{}] ", now.wYear, now.wMonth, now.wDay,
                    now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, GetCurrentProcessId(), source, level);
    if (!tag.empty())
        record += std::format("[{}] ", tag);
    record.append(line);
    record += "\r\n";
    const std::lock_guard lock(logWriteMutex);
    WriteOutput(LogFile(), record.data(), static_cast<DWORD>(record.size()));
}

DWORD WINAPI LogStream::CopyOutput(void* context)
{
    const auto& stream = *static_cast<LogStream*>(context);
    char buffer[8192];
    DWORD size = 0;
    std::string pending;
    constexpr size_t MaxLineSize = 64 * 1024;
    const bool errorStream = stream.standardHandle == STD_ERROR_HANDLE;
    while (ReadFile(stream.reader, buffer, sizeof(buffer), &size, nullptr) && size != 0) {
        WriteOutput(stream.forwardTo, buffer, size);
        try {
            std::string_view chunk(buffer, size);
            while (!chunk.empty()) {
                const auto newline = chunk.find('\n');
                const auto count = std::min({newline, chunk.size(), MaxLineSize - pending.size()});
                pending.append(chunk.substr(0, count));
                chunk.remove_prefix(count);
                if (count == newline || pending.size() == MaxLineSize) {
                    WriteRecord(pending, errorStream);
                    pending.clear();
                    if (count == newline)
                        chunk.remove_prefix(1);
                }
            }
        }
        catch (const std::bad_alloc&) {
            // Preserve diagnostics if the process is too low on memory to format records.
            const std::lock_guard lock(logWriteMutex);
            WriteOutput(LogFile(), buffer, size);
            pending.clear();
        }
    }
    if (!pending.empty()) {
        try {
            WriteRecord(pending, errorStream);
        }
        catch (const std::bad_alloc&) {
            const std::lock_guard lock(logWriteMutex);
            WriteOutput(LogFile(), pending.data(), static_cast<DWORD>(pending.size()));
        }
    }
    return 0;
}

bool LogStream::Start(FILE* stream, DWORD slot)
{
    // The CRT uses a special descriptor for processes started without a console.
    if (_fileno(stream) < 0) {
        FILE* sink = nullptr;
        if (freopen_s(&sink, "NUL", "w", stream) != 0)
            return false;
    }
    descriptor = _fileno(stream);
    original = _dup(descriptor);
    if (original < 0)
        return false;
    forwardTo = reinterpret_cast<HANDLE>(_get_osfhandle(original));
    standardHandle = slot;

    HANDLE writer = nullptr;
    if (!CreatePipe(&reader, &writer, nullptr, 0))
        return false;
    const int pipe = _open_osfhandle(reinterpret_cast<intptr_t>(writer), _O_WRONLY | _O_TEXT | _O_NOINHERIT);
    if (pipe < 0) {
        CloseHandle(writer);
        return false;
    }
    worker = CreateThread(nullptr, 0, CopyOutput, this, 0, nullptr);
    redirected = worker && _dup2(pipe, descriptor) == 0;
    _close(pipe);
    if (!redirected)
        return false;

    // OpenJDK's Java streams obtain the Win32 handles independently of the CRT.
    const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(descriptor));
    return SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0) && SetStdHandle(slot, handle);
}

void LogStream::Restore()
{
    if (!redirected)
        return;
    if (_dup2(original, descriptor) == 0) {
        SetStdHandle(standardHandle, reinterpret_cast<HANDLE>(_get_osfhandle(descriptor)));
    }
    else {
        _close(descriptor);
        SetStdHandle(standardHandle, nullptr);
    }
    redirected = false;
}

bool LogStream::Drain()
{
    // A child process with inherited output may keep a pipe writer open.
    if (worker && WaitForSingleObject(worker, 1000) != WAIT_OBJECT_0)
        return false;
    if (worker)
        CloseHandle(worker);
    if (reader)
        CloseHandle(reader);
    if (original >= 0)
        _close(original);
    worker = nullptr;
    reader = nullptr;
    original = -1;
    return true;
}

LogStream standardOutput;
LogStream standardError;

void FinishProcessLog()
{
    if (!logging)
        return;
    std::fflush(stdout);
    std::fflush(stderr);
    standardOutput.Restore();
    standardError.Restore();
    const bool outputDone = standardOutput.Drain();
    const bool errorDone = standardError.Drain();
    if (outputDone && errorDone) {
        const std::lock_guard lock(logWriteMutex);
        if (logFile != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER size{};
            const bool empty = GetFileSizeEx(logFile, &size) && size.QuadPart == 0;
            CloseHandle(logFile);
            if (empty)
                DeleteFileW(logPath.c_str());
        }
        logFile = INVALID_HANDLE_VALUE;
        logPath.clear();
        logging = false;
    }
}
} // namespace

void StopProcessLog()
{
    FinishProcessLog();
}

std::filesystem::path ProcessLogPath()
{
    const std::lock_guard lock(logWriteMutex);
    return logPath;
}

bool StartProcessLog(std::wstring_view operation)
{
    if (logging)
        return true;
    std::filesystem::path directory;
    try {
        std::wstring executable(32768, L'\0');
        const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        if (!length || length >= executable.size())
            return false;
        executable.resize(length);
        directory = UserLogDirectory() / std::filesystem::path(executable).stem();
    }
    catch (const std::exception&) {
        return false;
    }
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error)
        return false;

    logSuffix = TimestampSuffix();
    logName.clear();
    // HotSpot also uses the CRT exit path when Java calls System.exit.
    if (std::atexit(FinishProcessLog) != 0)
        return false;
    logPath = directory / logSuffix;
    logDirectory = directory;
    logging = true;
    if (!operation.empty() && !SetProcessLogOperation(operation)) {
        logging = false;
        logPath.clear();
        return false;
    }
    if (!standardOutput.Start(stdout, STD_OUTPUT_HANDLE) || !standardError.Start(stderr, STD_ERROR_HANDLE)) {
        FinishProcessLog();
        return false;
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    return true;
}

bool SetProcessLogPackage(std::wstring_view packageName)
{
    // Package names enter a filename: reject separators, traversal and Windows
    // special characters rather than rewriting an application's identity.
    if (!SafeLabel(packageName, L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_."))
        return false;
    return SelectLog(L"Applications", packageName);
}

bool SetProcessLogOperation(std::wstring_view operation)
{
    if (!SafeLabel(operation, L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-"))
        return false;
    return SelectLog(L"Operations", operation);
}
} // namespace stone::windows
